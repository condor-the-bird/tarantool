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

#include "say.h"
#include "cbus.h"
#include "coio.h"
#include "evio.h"
#include "main.h"
#include "fiber.h"
#include "iobuf.h"
#include "bit/bit.h"
#include "session.h"
#include "coio_buf.h"

#include "memcached.h"
#include "memcached_layer.h"

static int
memcached_process_request(struct memcached_connection *con) {
	try {
		/* Process message */
		con->noreply = false;
		switch (con->hdr->cmd) {
		case (MEMCACHED_BIN_CMD_ADDQ):
		case (MEMCACHED_BIN_CMD_SETQ):
		case (MEMCACHED_BIN_CMD_REPLACEQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_ADD):
		case (MEMCACHED_BIN_CMD_SET):
		case (MEMCACHED_BIN_CMD_REPLACE):
			mc_process_set(con);
			break;
		case (MEMCACHED_BIN_CMD_GETQ):
		case (MEMCACHED_BIN_CMD_GETKQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_GET):
		case (MEMCACHED_BIN_CMD_GETK):
			mc_process_get(con);
			break;
		case (MEMCACHED_BIN_CMD_DELETEQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_DELETE):
			mc_process_del(con);
			break;
		case (MEMCACHED_BIN_CMD_NOOP):
			mc_process_nop(con);
			break;
		case (MEMCACHED_BIN_CMD_QUITQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_QUIT):
			mc_process_quit(con);
			break;
		case (MEMCACHED_BIN_CMD_FLUSHQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_FLUSH):
			mc_process_flush(con);
			break;
		case (MEMCACHED_BIN_CMD_STAT):
			mc_process_stats(con);
			break;
		case (MEMCACHED_BIN_CMD_GATQ):
		case (MEMCACHED_BIN_CMD_GATKQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_GAT):
		case (MEMCACHED_BIN_CMD_GATK):
		case (MEMCACHED_BIN_CMD_TOUCH):
			mc_process_gat(con);
			break;
		case (MEMCACHED_BIN_CMD_VERSION):
			mc_process_version(con);
			break;
		case (MEMCACHED_BIN_CMD_INCRQ):
		case (MEMCACHED_BIN_CMD_DECRQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_INCR):
		case (MEMCACHED_BIN_CMD_DECR):
			mc_process_delta(con);
			break;
		case (MEMCACHED_BIN_CMD_APPENDQ):
		case (MEMCACHED_BIN_CMD_PREPENDQ):
			con->noreply = true;
		case (MEMCACHED_BIN_CMD_APPEND):
		case (MEMCACHED_BIN_CMD_PREPEND):
			mc_process_pend(con);
			break;
		default:
			assert(false);
		}
	} catch (Exception *e) {
		// memcached_reply_error(out, e, msg->hdr->opaque);
	}
	con->write_end = obuf_create_svp(&con->iobuf->out);
	con->iobuf->in.rpos += con->len;
	return 0;
}

static int
memcached_parse_request(struct memcached_connection *con) {
	struct ibuf *in      = &con->iobuf->in;
	const char *reqstart = in->rpos;
	/* Check that we have enough data for header */
	if (reqstart + sizeof(struct memcached_hdr) > in->wpos) {
		return sizeof(struct memcached_hdr) - (in->wpos - reqstart);
	}
	struct memcached_hdr *hdr = (struct memcached_hdr *)reqstart;
	/* error while parsing */
	if (hdr->magic != MEMCACHED_BIN_REQUEST) {
		say_error("Wrong magic, closing connection");
		return -1;
	}
	const char *reqend = reqstart + sizeof(struct memcached_hdr) +
			     bswap_u32(hdr->tot_len);
	/* Check that we have enough data for body */
	if (reqend > in->wpos) {
		return (reqend - in->wpos);
	}
	hdr->key_len = bswap_u16(hdr->key_len);
	hdr->tot_len = bswap_u32(hdr->tot_len);
	hdr->opaque  = bswap_u32(hdr->opaque);
	hdr->cas     = bswap_u64(hdr->cas);
	con->hdr = hdr;
	const char *pos = reqstart + sizeof(struct memcached_hdr);
	if ((con->body.ext_len = hdr->ext_len)) {
		con->body.ext = pos;
		pos += hdr->ext_len;
	} else {
		con->body.ext = NULL;
	}
	if ((con->body.key_len = hdr->key_len)) {
		con->body.key = pos;
		pos += hdr->key_len;
	} else {
		con->body.key = NULL;
	}
	uint32_t val_len = hdr->tot_len - (hdr->ext_len + hdr->key_len);
	if ((con->body.val_len = val_len)) {
		con->body.val = pos;
		pos += val_len;
	} else {
		con->body.val = NULL;
	}
	con->len = sizeof(struct memcached_hdr) + hdr->tot_len;
	assert(pos == reqend);
	return 0;
}

static ssize_t
memcached_flush(struct memcached_connection *con) {
	struct ev_io *coio  = con->coio;
	struct iobuf *iobuf = con->iobuf;
	ssize_t total = coio_writev(coio, iobuf->out.iov,
				    obuf_iovcnt(&iobuf->out),
				    obuf_size(&iobuf->out));
	if (ibuf_used(&iobuf->in) == 0)
		ibuf_reset(&iobuf->in);
	obuf_reset(&iobuf->out);
	// ibuf_reserve(&iobuf->in, cfg->readahead);
	return total;
}

static void
memcached_loop(struct memcached_connection *con)
{
	struct ev_io *coio  = con->coio;
	struct iobuf *iobuf = con->iobuf;
	int rc = 0;
	struct ibuf *in = &iobuf->in;
	size_t to_read = 1;

	for (;;) {
		ssize_t read = coio_bread(coio, in, to_read);
		if (read <= 0) return;
		to_read = 1;
next:
		rc = memcached_parse_request(con);
		if (rc < 0) {
			/* We close connection, because of wrong magic */
			return;
		} else if (rc > 0) {
			to_read = rc;
			continue;
		}
		/**
		 * Return -1 on force connection close
		 * Return 0 if everything is parsed
		 */
		rc = memcached_process_request(con);
		if (rc < 0 || con->close_connection) {
			say_debug("Requesting exit. Exiting.");
			return;
		} else if (rc > 0) {
			to_read = rc;
			continue;
		} else if (rc == 0 && ibuf_used(in) > 0) {
			/* Need to add check for batch count */
			goto next;
		}
		/* Write back answer */
		if (!con->noreply) memcached_flush(con);
	}
}

static void
memcached_handler(va_list ap)
{
	struct ev_io     coio    = va_arg(ap, struct ev_io);
	struct sockaddr *addr    = va_arg(ap, struct sockaddr *);
	socklen_t        addrlen = va_arg(ap, socklen_t); (void )addrlen;
	struct iobuf    *iobuf   = va_arg(ap, struct iobuf *);
	struct memcached_pair *p = va_arg(ap, struct memcached_pair *);

	struct memcached_connection con;
	/* TODO: move to connection_init */
	memset(&con, 0, sizeof(struct memcached_connection));
	con.coio      = &coio;
	con.iobuf     = iobuf;
	con.write_end = obuf_create_svp(&iobuf->out);
	con.cookie    = *(uint64_t *) addr;
	con.session   = session_create(con.coio->fd, con.cookie);
	con.cfg       = p->cfg;
	con.stat      = p->stat;

	/* read-write cycle */
	memcached_loop(&con);
}

void
memcached_set_listen(const char *name, const char *uri,
		     struct memcached_pair *pair)
{
	/* TODO: move to internal alloc */
	struct coio_service *mc_service = (struct coio_service *)
			calloc(1, sizeof(struct coio_service));
	coio_service_init(mc_service, name, memcached_handler, pair);
	coio_service_start((struct evio_service *)mc_service, uri);
}
