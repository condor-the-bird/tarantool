#ifndef TARANTOOL_REPLICA_H_INCLUDED
#define TARANTOOL_REPLICA_H_INCLUDED
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
#include <netinet/in.h>
#include <sys/socket.h>

#include "trivia/util.h"
#include "uri.h"

#include "third_party/tarantool_ev.h"
#define RB_COMPACT 1
#include <third_party/rb.h>

struct recovery_state;
struct fiber;

enum { REMOTE_SOURCE_MAXLEN = 1024 }; /* enough to fit URI with passwords */

#define REMOTE_STATE(_)                                             \
	_(REMOTE_OFF, 0)                                            \
	_(REMOTE_CONNECT, 1)                                        \
	_(REMOTE_AUTH, 2)                                           \
	_(REMOTE_CONNECTED, 3)                                      \
	_(REMOTE_BOOTSTRAP, 4)                                      \
	_(REMOTE_BOOTSTRAPPED, 5)                                   \
	_(REMOTE_FOLLOW, 6)                                         \
	_(REMOTE_STOPPED, 7)                                        \
	_(REMOTE_DISCONNECTED, 8)                                   \

ENUM(remote_state, REMOTE_STATE);
extern const char *remote_state_strs[];

/** State of a replication connection to the master */
struct remote {
	rb_node(struct remote) link;
	struct fiber *reader;
	enum remote_state state;
	ev_tstamp lag, last_row_time;
	bool warning_said;
	char source[REMOTE_SOURCE_MAXLEN];
	struct uri uri;
	union {
		struct sockaddr addr;
		struct sockaddr_storage addrstorage;
	};
	socklen_t addr_len;
	struct ev_io io; /* store fd to re-use connections between JOIN and SUBSCRIBE */
	bool expire_flag; /* used by expire_remotes()/prune_remotes() for merge */
};

typedef rb_tree(struct remote) remoteset_t;
rb_proto(, remoteset_, remoteset_t, struct remote)

#define recovery_foreach_remote(r, var) \
for (struct remote *var = remoteset_first(&(r)->var); \
	var != NULL; var = remoteset_next(&(r)->remote, var))

/** Connect to a master and request a snapshot.
 * Raises an exception on error.
 *
 * @return A connected socket, ready too receive
 * data.
 */
void
replica_bootstrap(struct recovery_state *r);

void
recovery_follow_remote(struct recovery_state *r);

/**
 * Mark all remote sources as expired.
 * Used by box_set_replication_source() to merge configuration.
 * \sa recovery_prune_remote()
 * \sa recovery_set_remote()
 */
void
recovery_expire_remotes(struct recovery_state *r);

/**
 * Remove all expired remote sources;
 * Used by box_set_replication_source() to merge configuration.
 * \sa recovery_expire_remote()
 * \sa recovery_set_remote()
 */
void
recovery_prune_remotes(struct recovery_state *r);

/**
 * Create or update replication source.
 * \sa recovery_expire_remote()
 * \sa recovery_prune_remote()
 */
void
recovery_set_remote(struct recovery_state *r, const char *source);

const char *
remote_status(struct remote *remote);

#endif /* TARANTOOL_REPLICA_H_INCLUDED */
