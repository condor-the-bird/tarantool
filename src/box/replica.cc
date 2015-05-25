/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "replica.h"

#include <ctype.h> /* tolower() */

#include "main.h"

#include "recovery.h"
#include "xlog.h"
#include "fiber.h"
#include "scoped_guard.h"
#include "coio_buf.h"
#include "recovery.h"
#include "xrow.h"
#include "msgpuck/msgpuck.h"
#include "box/cluster.h"
#include "iproto_constants.h"

static const int RECONNECT_DELAY = 1.0;
STRS(remote_state, REMOTE_STATE);

static int
remote_compare(const struct remote *a, const struct remote *b)
{
	return strcmp(a->source, b->source);
}

rb_gen(, remoteset_, remoteset_t, struct remote, link, remote_compare);

const char *
remote_status(struct remote *remote)
{
	/* Return current state in lower case */
	static char status[16];
	char *d = status;
	const char *s = remote_state_strs[remote->state] + strlen("REMOTE_");
	assert(strlen(s) < sizeof(status));
	while ((*(d++) = tolower(*(s++))));
	return status;
}

static void
remote_set_state(struct remote *remote, enum remote_state state)
{
	remote->state = state;
	say_debug(" => %s", remote_status(remote));
}

static inline void
remote_set_source(struct remote *remote, const char *source)
{
	snprintf(remote->source, sizeof(remote->source), "%s", source);
	/* uri_parse() sets pointers to remote->source buffer */
	int rc = uri_parse(&remote->uri, remote->source);
	/* checked by box_set_replication_source() */
	assert(rc == 0 && remote->uri.service != NULL);
	(void) rc;
}

static void
remote_read_row(struct ev_io *coio, struct iobuf *iobuf,
		struct xrow_header *row)
{
	struct ibuf *in = &iobuf->in;

	/* Read fixed header */
	if (ibuf_size(in) < 1)
		coio_breadn(coio, in, 1);

	/* Read length */
	if (mp_typeof(*in->pos) != MP_UINT) {
		tnt_raise(ClientError, ER_INVALID_MSGPACK,
			  "packet length");
	}
	ssize_t to_read = mp_check_uint(in->pos, in->end);
	if (to_read > 0)
		coio_breadn(coio, in, to_read);

	uint32_t len = mp_decode_uint((const char **) &in->pos);

	/* Read header and body */
	to_read = len - ibuf_size(in);
	if (to_read > 0)
		coio_breadn(coio, in, to_read);

	xrow_header_decode(row, (const char **) &in->pos, in->pos + len);
}

static void
remote_write_row(struct ev_io *coio, const struct xrow_header *row)
{
	struct iovec iov[XROW_IOVMAX];
	int iovcnt = xrow_to_iovec(row, iov);
	coio_writev(coio, iov, iovcnt, 0);
}

static void
remote_connect(struct remote *remote, struct ev_io *coio,
	       struct iobuf *iobuf)
{
	if (evio_is_active(coio))
		return; /* Already connected, skip */
	remote_set_state(remote, REMOTE_CONNECT);
	char greeting[IPROTO_GREETING_SIZE];

	struct uri *uri = &remote->uri;

	/*
	 * coio_connect() stores resolved address to \a &remote->addr
	 * on success. &remote->addr_len is a value-result argument which
	 * must be initialized to the size of associated buffer (addrstorage)
	 * before calling coio_connect(). Since coio_connect() performs
	 * DNS resolution under the hood it is theoretically possible that
	 * remote->addr_len will be different even for same uri.
	 */
	remote->addr_len = sizeof(remote->addrstorage);
	coio_connect(coio, uri, &remote->addr, &remote->addr_len);
	assert(coio->fd >= 0);
	coio_readn(coio, greeting, sizeof(greeting));

	if (!remote->warning_said) {
		say_info("connected to %s", sio_strfaddr(&remote->addr,
			 remote->addr_len));
	}

	/* Don't display previous error messages in box.info.replication */
	Exception::clear(&fiber()->exception);

	/* Perform authentication if user provided at least login */
	if (!uri->login) {
		remote_set_state(remote, REMOTE_CONNECTED);
		return;
	}

	/* Authenticate */
	remote_set_state(remote, REMOTE_AUTH);
	struct xrow_header row;
	xrow_encode_auth(&row, greeting, uri->login,
			 uri->login_len, uri->password,
			 uri->password_len);
	remote_write_row(coio, &row);
	remote_read_row(coio, iobuf, &row);
	if (row.type != IPROTO_OK)
		xrow_decode_error(&row); /* auth failed */

	/* auth successed */
	remote_set_state(remote, REMOTE_CONNECTED);
}

static void
remote_join(struct remote *remote, struct recovery_state *r,
	    struct ev_io *coio, struct iobuf *iobuf)
{
	if (!remote->warning_said)
		say_info("bootstrapping a replica from %s", remote->source);

	remote_connect(remote, coio, iobuf);
	/* Send JOIN request */
	struct xrow_header row;
	xrow_encode_join(&row, &r->server_uuid);
	remote_write_row(coio, &row);
	remote_set_state(remote, REMOTE_BOOTSTRAP);

	assert(vclock_has(&r->vclock, 0)); /* check for surrogate server_id */

	while (true) {
		remote_read_row(coio, iobuf, &row);
		if (row.type == IPROTO_OK) {
			/* End of stream */
			say_info("done");
			break;
		} else if (iproto_type_is_dml(row.type)) {
			/* Regular snapshot row  (IPROTO_INSERT) */
			recovery_apply_row(r, &row);
		} else /* error or unexpected packet */ {
			xrow_decode_error(&row);  /* rethrow error */
		}
	}

	/* Decode end of stream packet */
	struct vclock vclock;
	vclock_create(&vclock);
	assert(row.type == IPROTO_OK);
	xrow_decode_vclock(&row, &vclock);

	/* Replace server vclock using data from snapshot */
	vclock_copy(&r->vclock, &vclock);

	remote_set_state(remote, REMOTE_BOOTSTRAPPED);
	/* keep connection */
}

static void
remote_subscribe(struct remote *remote, struct recovery_state *r,
		 struct ev_io *coio, struct iobuf *iobuf)
{
	remote_connect(remote, coio, iobuf);

	/* Send SUBSCRIBE request */
	struct xrow_header row;
	xrow_encode_subscribe(&row, &cluster_id,
		&r->server_uuid, &r->vclock);
	remote_write_row(coio, &row);
	remote_set_state(remote, REMOTE_FOLLOW);
	remote->warning_said = false;

	/**
	 * If there is an error in subscribe, it's
	 * sent directly in response to subscribe.
	 * If subscribe is successful, there is no
	 * "OK" response, but a stream of rows.
	 * from the binary log.
	 */
	while (true) {
		remote_read_row(coio, iobuf, &row);
		remote->lag = ev_now(loop()) - row.tm;
		remote->last_row_time = ev_now(loop());

		if (iproto_type_is_error(row.type))
			xrow_decode_error(&row);  /* error */
		recovery_apply_row(r, &row);

		iobuf_reset(iobuf);
		fiber_gc();
	}
}

static inline void
remote_log_exception(struct remote *remote, Exception *e)
{
	if (remote->warning_said)
		return;
	switch (remote->state) {
	case REMOTE_CONNECT:
		say_info("can't connect to master");
		break;
	case REMOTE_CONNECTED:
		say_info("can't join/subscribe");
		break;
	case REMOTE_AUTH:
		say_info("failed to authenticate");
		break;
	case REMOTE_FOLLOW:
	case REMOTE_BOOTSTRAP:
		say_info("can't read row");
		break;
	default:
		break;
	}
	e->log();
}

static inline void
remote_pull(struct remote *remote, struct recovery_state *r)
{
	struct iobuf *iobuf = iobuf_new(fiber_name(fiber()));
	struct ev_io *coio = &remote->io;
	coio_init(coio);

	while (true) {
		try {
			if (!r->finalize) {
				remote_join(remote, r, coio, iobuf);
				ev_io_stop(loop(), coio);
				/* keep connection */
				return;
			} else {
				remote_subscribe(remote, r, coio, iobuf);
				assert(0); /* unreachable */
				break;
			}
		} catch (ClientError *e) {
			remote_log_exception(remote, e);
			remote->warning_said = true;
			evio_close(loop(), coio);
			iobuf_delete(iobuf);
			remote_set_state(remote, REMOTE_STOPPED);
			throw;
		} catch (FiberCancelException *e) {
			evio_close(loop(), coio);
			iobuf_delete(iobuf);
			remote_set_state(remote, REMOTE_OFF);
			throw;
		} catch (SocketError *e) {
			remote_log_exception(remote, e);
			evio_close(loop(), coio);
			remote_set_state(remote, REMOTE_DISCONNECTED);
			if (!r->finalize) {
				/*
				 * For JOIN re-connection logic handled
				 * by replica_bootstrap()
				 */
				remote->warning_said = true;
				throw;
			}
			/* fall through */
		}

		if (!remote->warning_said)
			say_info("will retry every %i second", RECONNECT_DELAY);
		remote->warning_said = true;
		iobuf_reset(iobuf);
		fiber_gc();

		/* Put fiber_sleep() out of catch block.
		 *
		 * This is done to avoid situation, when two or more
		 * fibers yield's inside their try/catch blocks and
		 * throws an exceptions. Seems like exception unwinder
		 * stores some global state while being inside a catch
		 * block.
		 *
		 * This could lead to incorrect exception processing
		 * and crash the server.
		 *
		 * See: https://github.com/tarantool/tarantool/issues/136
		*/

		try {
			fiber_sleep(RECONNECT_DELAY);
		} catch (Exception *e) {
			iobuf_delete(iobuf);
			throw;
		}
	}
}

static void
remote_pull_f(va_list ap)
{
	struct remote *remote = va_arg(ap, struct remote *);
	struct recovery_state *r = va_arg(ap, struct recovery_state *);
	return remote_pull(remote, r);
}

static void
remote_start(struct remote *remote, struct recovery_state *r)
{
	char name[FIBER_NAME_MAX];
	assert(remote->reader == NULL);

	const char *uri = uri_format(&remote->uri);
	say_debug("starting replication from %s", uri);
	snprintf(name, sizeof(name), "replica/%s", uri);

	struct fiber *f = fiber_new(name, remote_pull_f);
	/**
	 * So that we can safely grab the status of the
	 * fiber any time we want.
	 */
	fiber_set_joinable(f, true);
	remote->reader = f;
	fiber_start(f, remote, r);
}

static void
remote_stop(struct remote *remote)
{
	const char *uri = uri_format(&remote->uri);
	say_debug("shutting down the replica %s", uri);
	struct fiber *f = remote->reader;
	assert(f != NULL);
	fiber_cancel(f);
	/**
	 * If the remote died from an exception, don't throw it
	 * up.
	 */
	Exception::clear(&f->exception);
	fiber_join(f);
	remote->reader = NULL;
}


void
recovery_set_remote(struct recovery_state *r, const char *source)
{
	struct uri uri;
	assert(uri_parse(&uri, source) == 0 && uri.service != NULL);
	(void) uri;
	struct remote key;
	snprintf(key.source, sizeof(key.source), "%s", source);
	struct remote *remote = remoteset_search(&r->remote, &key);
	if (remote != NULL) {
		remote->warning_said = false;
		remote->expire_flag = false;
		if (remote->reader != NULL) {
			if ((remote->state != REMOTE_STOPPED &&
			     remote->state != REMOTE_OFF))
				return;
			/* Re-start faulted replicas */
			remote_stop(remote);
			assert(remote->reader == NULL);
		}
		goto done;
	}

	remote = (struct remote *) calloc(1, sizeof(*remote));
	if (remote == NULL) {
		tnt_raise(OutOfMemory, sizeof(*remote), "malloc",
			"struct remote");
	}

	remote->io.fd = -1;
	remote->expire_flag = false;
	remote_set_source(remote, source);
	remoteset_insert(&r->remote, remote);

done:
	/* Automatically start new replicas after adding */
	if (r->finalize)
		remote_start(remote, r);
}

static void
recovery_clear_remote(struct recovery_state *r, struct remote *remote)
{
	if (remote->reader != NULL)
		remote_stop(remote);
	remoteset_remove(&r->remote, remote);
	free(remote);
}

void
recovery_expire_remotes(struct recovery_state *r)
{
	/* Mark to delete old connections. */
	recovery_foreach_remote(r, remote) {
		remote->expire_flag = true;
	}
}

void
recovery_prune_remotes(struct recovery_state *r)
{
	/* Stop old connections. */
	for (struct remote *remote = remoteset_first(&r->remote);
	     remote != NULL;) {
	        struct remote *next = remoteset_next(&r->remote, remote);
		if (remote->expire_flag) {
			/* doesn't throw */
			recovery_clear_remote(r, remote);
		}
		remote = next;
	}
}

void
recovery_follow_remote(struct recovery_state *r)
{
	assert(r->finalize);
	recovery_foreach_remote(r, remote) {
		remote_start(remote, r);
	}
}

void
replica_bootstrap(struct recovery_state *r)
{
	assert(!r->finalize);

	/* Generate Server-UUID */
	tt_uuid_create(&r->server_uuid);

	/* Add a surrogate server id for snapshot rows */
	vclock_add_server(&r->vclock, 0);

	bool warning_said = false;
	while (true) {
		/* cfg has at least one replica */
		assert(remoteset_first(&r->remote) != NULL);

		recovery_foreach_remote(r, remote) {
			/* Fail if some records already processed */
			if (vclock_signature(&r->vclock) > 0)
				panic("failed too resolve partial bootstrap");

			/* remote_start(remote, r); */
			try {
				/* Bootstrap using current fiber */
				assert(remote->reader == NULL);
				remote->reader = fiber();
				remote_pull(remote, r);
				/* fiber_join(remote->reader); */
				remote->reader = NULL;
				return; /* Success */
			} catch (Exception *e) {
				remote->reader = NULL;
				continue; /* Try a next replica */
			}
		}
		if (!warning_said)
			say_info("will retry every %i second", RECONNECT_DELAY);
		warning_said = true;
		fiber_sleep(RECONNECT_DELAY);
	}
}
