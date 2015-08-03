#ifndef TARANTOOL_MEMCACHED_H_INCLUDED
#define TARANTOOL_MEMCACHED_H_INCLUDED

/*
 ** Old text memcached API
 * int
 * memcached_parse_text(struct mc_request *req,
 * 		     const char **p,
 * 		     const char *pe);
 */

#include "memcached_constants.h"

void
memcached_init();

extern "C" void
memcached_set_listen(const char *name, const char *uri,
		     struct memcached_pair *pair);

/**
 * Single connection object, handles information about
 * 1) pointer to memcached stats
 * 2) pointer to memcached config
 * 3) connection data, that's created by coio
 * 4) internal tarantool session for access limitations
 * 4) last decoded memcached message (we can do it since memcached
 * 				      binary proto is synchronious)
 */
struct memcached_connection {
	/* memcached_specific data */
	struct memcached_stats *stat;
	struct memcached_cfg   *cfg;
	/* connection data */
	struct ev_io   *coio;
	struct iobuf   *iobuf;
	struct obuf_svp write_end;
	bool            noreply;
	bool            close_connection;
	/* session data */
	uint64_t        cookie;
	struct session *session;
	/* request data */
	struct memcached_hdr *hdr;
	struct memcached_body body;
	size_t                len;
};

struct memcached_cfg {
	uint32_t spaceid;
	uint64_t cas;
	bool     flush_enabled;
	uint64_t flush;
};

struct memcached_stats {
	unsigned int  curr_items;
	unsigned int  total_items;
	unsigned int  curr_conns;
	unsigned int  total_conns;
	uint64_t      bytes_read;
	uint64_t      bytes_written;
	time_t        started;          /* when the process was started */
	/* get statistics */
	uint64_t      cmd_get;
	uint64_t      get_hits;
	uint64_t      get_misses;
	/* delete stats */
	uint64_t      cmd_delete;
	uint64_t      delete_hits;
	uint64_t      delete_misses;
	/* set statistics */
	uint64_t      cmd_set;
	uint64_t      cas_hits;
	uint64_t      cas_badval;
	uint64_t      cas_misses;
	/* incr/decr stats */
	uint64_t      cmd_incr;
	uint64_t      incr_hits;
	uint64_t      incr_misses;
	uint64_t      cmd_decr;
	uint64_t      decr_hits;
	uint64_t      decr_misses;
	/* touch/flush stats */
	uint64_t      cmd_touch;
	uint64_t      touch_hits;
	uint64_t      touch_misses;
	uint64_t      cmd_flush;
	/* expiration stats */
	uint64_t      evictions;
	uint64_t      reclaimed;
	/* authentication stats */
	uint64_t      auth_cmds;
	uint64_t      auth_errors;
};

struct memcached_pair {
	struct memcached_cfg   *cfg;
	struct memcached_stats *stat;
};

#endif
