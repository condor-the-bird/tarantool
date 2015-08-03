#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

#include "msgpuck/msgpuck.h"
#include "iobuf.h"
#include "fiber.h"
#include "say.h"
#include "memory.h"
#include "cbus.h"
#include "bit/bit.h"

#include "box.h"
#include "error.h"
#include "tuple.h"
#include "index.h"

#include "main.h"

#include "memcached.h"
#include "memcached_layer.h"

/* MEMCACHED_CONVERTION_FUNCTIONS */

#define xisspace(c) isspace((unsigned char)c)

bool safe_strtoull(const char *begin, const char *end, uint64_t *out) {
	assert(out != NULL);
	errno = 0;
	*out = 0;
	char *endptr;
	unsigned long long ull = strtoull(begin, &endptr, 10);
	if ((errno == ERANGE) || (begin == endptr) || (endptr != end)) {
		return false;
	}

	if (xisspace(*endptr) || (*endptr == '\0' && endptr != begin) ||
	    endptr == end) {
		if ((long long) ull < 0) {
			/* only check for negative signs in the uncommon
			 * case when the unsigned number is so big that
			 * it's negative as a signed number. */
			if (strchr(begin, '-') != NULL) {
				return false;
			}
		}
		*out = ull;
		return true;
	}
	return false;
}

/*
 * default exptime is 30*24*60*60 seconds
 * \* 1000000 to convert it to usec (need this precision)
 **/
#define DEFAULT_EXPTIME (30*24*60*60*1000000LL)

static __attribute__((unused)) void
memcached_dump_hdr(struct memcached_hdr *hdr) {
	if (!hdr) return;
	say_debug("memcached package");
	say_debug("magic:     0x%" PRIX8,        hdr->magic);
	say_debug("cmd:       0x%" PRIX8,        hdr->cmd);
	if (hdr->key_len > 0)
		say_debug("key_len:   %" PRIu16, hdr->key_len);
	if (hdr->ext_len > 0)
		say_debug("ext_len:   %" PRIu8,  hdr->ext_len);
	say_debug("tot_len:   %" PRIu32,         hdr->tot_len);
	say_debug("opaque:    0x%" PRIX32,       bswap_u32(hdr->opaque));
	say_debug("cas:       %" PRIu64,         hdr->cas);
}

static inline uint64_t
get_current_time() {
	return fiber_time64();
}

static inline uint64_t
convert_exptime (uint64_t exptime) {
	if (exptime == 0) return 0; /* 0 means never expire */
	if (exptime < DEFAULT_EXPTIME)
		return get_current_time() + exptime * 1000000;
	return exptime * 1000000;
}

static inline int
is_expired (const char *key, uint32_t key_len,
	    uint64_t exptime, uint64_t time, uint64_t flush) {
	(void )time;
	uint64_t curtime = get_current_time();
	/* Expired by flush */
	if (flush <= curtime && time <= flush) {
		say_debug("key '%.*s' expired by flush", key_len, key);
		say_debug("flush time %lu, current time %lu, time of creation %lu",
			 flush, curtime, time);
		return 1;
	}
	/* Expired by TTL */
	if (exptime != 0 && exptime < curtime) {
		say_debug("key '%.*s' expired by expire", key_len, key);
		say_debug("exptime is %lu, current time %lu",
			 exptime, curtime);
		return 1;
	}
	return 0;
}

static inline int
is_expired_tuple(box_tuple_t *tuple, uint64_t flush) {
	const char *pos  = box_tuple_field(tuple, 0);
	uint32_t key_len = 0;
	const char *key  = mp_decode_str(&pos, &key_len);
	uint64_t exptime = mp_decode_uint(&pos);
	uint64_t time    = mp_decode_uint(&pos);
	return is_expired(key, key_len, exptime, time, flush);
}

/* This function swaps byte order, so ... */
static void
write_output(struct obuf *out, struct memcached_hdr *hdri,
	     uint16_t err, uint64_t cas,
	     uint8_t ext_len, uint16_t key_len, uint32_t val_len,
	     const char *ext, const char *key, const char *val
	     ) {
	struct memcached_hdr hdro;
	memcpy(&hdro, hdri, sizeof(struct memcached_hdr));
	hdro.magic   = MEMCACHED_BIN_RESPONSE;
	hdro.ext_len = ext_len;
	hdro.key_len = bswap_u16(key_len);
	hdro.status  = bswap_u16(err);
	hdro.tot_len = bswap_u32(ext_len + key_len + val_len);
	hdro.opaque  = bswap_u32(hdro.opaque);
	hdro.cas     = bswap_u64(cas);
	obuf_dup(out, &hdro, sizeof(struct memcached_hdr));
	if (ext && ext_len > 0) obuf_dup(out, ext, ext_len);
	if (key && key_len > 0) obuf_dup(out, key, key_len);
	if (val && val_len > 0) obuf_dup(out, val, val_len);
}

static int
mc_insert_tuple(const char *kpos, uint32_t klen, uint64_t expire,
		const char *vpos, uint32_t vlen, uint64_t cas,
		uint32_t flags, uint32_t spaceid)
{
	uint64_t time = get_current_time();
	uint32_t len = mp_sizeof_array(6)      +
		       mp_sizeof_str  (klen)   +
		       mp_sizeof_uint (expire) +
		       mp_sizeof_uint (time)   +
		       mp_sizeof_str  (vlen)   +
		       mp_sizeof_uint (cas)    +
		       mp_sizeof_uint (flags);
	char *begin  = (char *)region_alloc(&fiber()->gc, len);
	char *end    = NULL;
	end = mp_encode_array(begin, 6);
	end = mp_encode_str  (end, kpos, klen);
	end = mp_encode_uint (end, expire);
	end = mp_encode_uint (end, time);
	end = mp_encode_str  (end, vpos, vlen);
	end = mp_encode_uint (end, cas);
	end = mp_encode_uint (end, flags);
	assert(end <= begin + len);
	return box_replace(spaceid, begin, end, NULL);
}

static void
mc_process_error(struct obuf *out, struct memcached_hdr *hdr,
		 uint16_t err, const char *errstr) {
	if (!errstr) {
		switch (err) {
		case MEMCACHED_BIN_RES_ENOMEM:
			errstr = "Out of memory";
			break;
		case MEMCACHED_BIN_RES_UNKNOWN_COMMAND:
			errstr = "Unknown command";
			break;
		case MEMCACHED_BIN_RES_KEY_ENOENT:
			errstr = "Not found";
			break;
		case MEMCACHED_BIN_RES_EINVAL:
			errstr = "Invalid arguments";
			break;
		case MEMCACHED_BIN_RES_KEY_EEXISTS:
			errstr = "Data exists for key.";
			break;
		case MEMCACHED_BIN_RES_E2BIG:
			errstr = "Too large.";
			break;
		case MEMCACHED_BIN_RES_DELTA_BADVAL:
			errstr = "Non-numeric server-side value for incr or decr";
			break;
		case MEMCACHED_BIN_RES_NOT_STORED:
			errstr = "Not stored.";
			break;
		case MEMCACHED_BIN_RES_AUTH_ERROR:
			errstr = "Auth failure.";
			break;
		default:
			say_error("UNHANDLED ERROR: %d", err);
			assert(false);
			errstr = "UNHANDLED ERROR";
		}
	}
	size_t len = 0;
	if (errstr) len = strlen(errstr);
	write_output(out, hdr, err, 0, 0, 0, len, NULL, NULL, errstr);
}

static void
mc_process_internal_error(struct obuf *out, struct memcached_hdr *hdr) {
	const box_error_t *err = box_error_last();
	uint16_t       errcode = box_error_code(err);
	const char     *errstr = box_error_message(err);
	switch(errcode) {
	case (ER_MEMORY_ISSUE):
		errcode = MEMCACHED_BIN_RES_ENOMEM;
		errstr  = NULL;
		break;
	case (ER_TUPLE_NOT_FOUND):
		errcode = MEMCACHED_BIN_RES_KEY_ENOENT;
		errstr  = NULL;
		break;
	case (ER_TUPLE_FOUND):
		errcode = MEMCACHED_BIN_RES_KEY_EEXISTS;
		errstr  = NULL;
		break;
	default:
		break;
	}
	mc_process_error(out, hdr, errcode, errstr);
}

/*
 * Tuple schema is:
 *
 * - key
 * - exptime - expire time
 * - time - time of creation/latest access
 * - value
 * - cas
 * - flags
 */
void
mc_process_set(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext == NULL || b->key == NULL || b->val == NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext == 0) say_error("package has no ext");
		if (b->ext_len != sizeof(struct memcached_set_ext)){
			say_error("ext length differs, expected %zu, get %u",
				  sizeof(struct memcached_set_ext), b->ext_len);
		}
		if (b->key == 0) say_error("package has no key");
		if (b->val == 0) say_error("package has no val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	say_debug("%s '%.*s' '%.*s'", mc_get_command_name(h->cmd), b->key_len,
		  b->key, b->val_len, b->val);
	con->stat->cmd_set++;
	struct memcached_set_ext *ext = (struct memcached_set_ext *)b->ext;
	ext->flags = bswap_u32(ext->flags);
	uint64_t exptime = convert_exptime(bswap_u32(ext->expire));
	uint64_t cas     = con->cfg->cas++;
	uint32_t len = mp_sizeof_array(1) +
		       mp_sizeof_str  (b->key_len);
	char *begin  = (char *) region_alloc(&fiber()->gc, len);
	char *end = NULL;
	assert(end <= begin + len);
	box_tuple_t *tuple = NULL;
	end = mp_encode_array(begin, 1);
	end = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);
	if (box_index_get(con->cfg->spaceid, 0, begin, end, &tuple) == -1) {
		return mc_process_internal_error(out, h);
	}
	if (h->cmd == MEMCACHED_BIN_CMD_REPLACE &&
			(tuple == NULL || is_expired_tuple(tuple, con->cfg->flush))) {
		return mc_process_error(out, h,
				MEMCACHED_BIN_RES_KEY_ENOENT, NULL);
	} else if (h->cmd == MEMCACHED_BIN_CMD_ADD &&
			!(tuple == NULL || is_expired_tuple(tuple, con->cfg->flush))) {
		return mc_process_error(out, h,
				MEMCACHED_BIN_RES_KEY_EEXISTS, NULL);
	} else if (h->cas != 0) {
		if (!tuple || is_expired_tuple(tuple, con->cfg->flush)) {
			con->stat->cas_misses++;
			say_debug("CAS is there, but no tuple");
			return mc_process_error(out, h,
				MEMCACHED_BIN_RES_KEY_ENOENT, NULL);
		} else if (tuple) {
			con->stat->cas_badval++;
			const char *pos   = box_tuple_field(tuple, 4);
			uint64_t cas_prev = mp_decode_uint(&pos);
			if (cas_prev != h->cas) {
				say_debug("CAS is there, tuple is there, no match");
				return mc_process_error(out, h,
					MEMCACHED_BIN_RES_KEY_EEXISTS, NULL);
			}
		}
		con->stat->cas_hits++;
	}
	if (mc_insert_tuple(b->key, b->key_len, exptime, b->val, b->val_len,
			    cas, ext->flags, con->cfg->spaceid) == -1) {
		mc_process_internal_error(out, h);
	} else {
		if (!con->noreply) {
			write_output(out, h, MEMCACHED_BIN_RES_OK, cas, 0, 0,
				     0, NULL, NULL, NULL);
		}
	}
}

void
mc_process_get(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext != NULL || b->key == NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key == 0) say_error("package has no key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	con->stat->cmd_get++;
	say_debug("%s '%.*s'", mc_get_command_name(h->cmd), b->key_len, b->key);
	uint32_t len = mp_sizeof_array(1) +
		       mp_sizeof_str  (b->key_len);
	char *begin = (char *) region_alloc(&fiber()->gc, len);
	char *end   = mp_encode_array(begin, 1);
	      end   = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);
	box_tuple_t *tuple = NULL;
	if (box_index_get(con->cfg->spaceid, 0, begin, end, &tuple) == -1) {
		mc_process_internal_error(out, h);
	} else if (tuple != NULL && !is_expired_tuple(tuple, con->cfg->flush)) {
		struct memcached_get_ext ext;
		uint32_t vlen = 0, klen = 0;
		const char *pos  = box_tuple_field(tuple, 0);
		const char *kpos = mp_decode_str(&pos, &klen);
		mp_next(&pos); mp_next(&pos);
		const char *vpos = mp_decode_str(&pos, &vlen);
		uint64_t cas     = mp_decode_uint(&pos);
		uint32_t flags   = mp_decode_uint(&pos);
		if (h->cmd == MEMCACHED_BIN_CMD_GET ||
		    h->cmd == MEMCACHED_BIN_CMD_GETQ) {
			kpos = NULL;
			klen = 0;
		}
		ext.flags = bswap_u32(flags);
		write_output(out, h, MEMCACHED_BIN_RES_OK, cas,
			     sizeof(struct memcached_get_ext), 0, vlen,
			     (const char *)&ext, kpos, vpos);
		con->stat->get_hits++;
	} else {
		con->stat->get_misses++;
		if (!con->noreply) {
			mc_process_error(out, h, MEMCACHED_BIN_RES_KEY_ENOENT,
					 NULL);
		}
	}
}

void
mc_process_del(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext != NULL || b->key == NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key == 0) say_error("package has no key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	con->stat->cmd_delete++;
	uint32_t len = mp_sizeof_array(1) +
		       mp_sizeof_str  (b->key_len);
	char *begin = (char *) region_alloc(&fiber()->gc, len);
	char *end   = mp_encode_array(begin, 1);
	      end   = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);
	box_tuple_t *tuple = NULL;
	if (box_delete(con->cfg->spaceid, 0, begin, end, &tuple) == -1) {
		mc_process_internal_error(out, h);
	} else if (tuple != NULL) {
		con->stat->delete_hits++;
		if (!con->noreply) {
			write_output(out, h, MEMCACHED_BIN_RES_OK, 0, 0, 0, 0,
				     NULL, NULL, NULL);
		}
	} else {
		con->stat->delete_misses++;
		mc_process_error(out, h, MEMCACHED_BIN_RES_KEY_ENOENT, NULL);
	}
}

void
mc_process_version(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext != NULL || b->key != NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key != 0) say_error("package has key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	const char *vers = tarantool_version();
	int vlen = strlen(vers);
	if (!con->noreply) {
		write_output(out, h, MEMCACHED_BIN_RES_OK, 0, 0, 0, vlen, NULL,
			     NULL, vers);
	}
}

void
mc_process_nop(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext != NULL || b->key != NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key != 0) say_error("package has key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	if (!con->noreply) {
		write_output(out, h, MEMCACHED_BIN_RES_OK, 0, 0, 0, 0, NULL,
			     NULL, NULL);
	}
}

void
mc_process_flush(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->key != NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext && b->ext_len != sizeof(struct memcached_flush_ext)) {
			say_error("ext length differs, expected %zu, get %u",
				  sizeof(struct memcached_flush_ext), b->ext_len);
		}
		if (b->key != 0) say_error("package has key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	con->stat->cmd_flush++;
	struct memcached_flush_ext *ext = (struct memcached_flush_ext *)b->ext;
	uint64_t exptime = 0;
	if (ext != NULL) exptime = convert_exptime(bswap_u32(ext->expire));
	con->cfg->flush = (exptime > 0 ? exptime : get_current_time());
	if (!con->noreply) {
		write_output(out, h, MEMCACHED_BIN_RES_OK, 0, 0, 0, 0, NULL,
			     NULL, NULL);
	}
}

void
mc_process_gat(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext == NULL || b->key == NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext == 0) say_error("package has no ext");
		if (b->ext_len != sizeof(struct memcached_touch_ext)) {
			say_error("ext length differs, expected %zu, get %u",
				  sizeof(struct memcached_touch_ext), b->ext_len);
		}
		if (b->key == 0) say_error("package has no key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	con->stat->cmd_touch++;
	struct memcached_touch_ext *ext = (struct memcached_touch_ext *)b->ext;
	uint64_t exptime = convert_exptime(bswap_u32(ext->expire));
	uint64_t current = get_current_time();

	uint32_t len  = mp_sizeof_array (2)   +
			mp_sizeof_array (3)   +
			mp_sizeof_str   (1)   +
			mp_sizeof_uint  (1)   +
			mp_sizeof_uint  (exptime) +
			mp_sizeof_array (3)   +
			mp_sizeof_str   (1)   +
			mp_sizeof_uint  (2)   +
			mp_sizeof_uint  (current) +
			mp_sizeof_array (1)   +
			mp_sizeof_str   (b->key_len);
	char *begin  = (char *) region_alloc(&fiber()->gc, len);
	char *end = NULL, *key = NULL;
	/* Encode  */
	end = mp_encode_array(begin, 2);
	/* Encode expire update */
	end = mp_encode_array(end, 3);
	end = mp_encode_str  (end, "=", 1);
	end = mp_encode_uint (end, 1);
	end = mp_encode_uint (end, exptime);
	/* Encode tuple touch time update */
	end = mp_encode_array(end, 3);
	end = mp_encode_str  (end, "=", 1);
	end = mp_encode_uint (end, 2);
	end = key = mp_encode_uint (end, current);
	/* Encode key for update */
	end = mp_encode_array(end, 1);
	end = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);

	box_tuple_t *tuple = NULL;
	if (box_index_get(con->cfg->spaceid, 0, key, end, &tuple) == -1) {
		say_info("error here");
		return mc_process_internal_error(out, h);
	} else if (tuple == NULL || is_expired_tuple(tuple, con->cfg->flush)) {
		con->stat->touch_misses++;
		if (!con->noreply) {
			mc_process_error(out, h,
					MEMCACHED_BIN_RES_KEY_ENOENT, NULL);
		}
		return;
	}
	con->stat->touch_hits++;

	/* Tuple can't be NULL, because we already found this element */
	if (box_update(con->cfg->spaceid, 0, key, end,
		       begin, key, 0, &tuple) == -1) {
		say_info("or there");
		mc_process_internal_error(out, h);
	} else {
		uint32_t vlen = 0, klen = 0, elen = 0;
		const char *kpos = NULL, *vpos = NULL;
		uint32_t flags = 0; uint64_t cas = 0;
		struct memcached_get_ext *epos = NULL;
		if (h->cmd >= MEMCACHED_BIN_CMD_GAT) {
			const char *pos  = box_tuple_field(tuple, 0);
			kpos = mp_decode_str(&pos, &klen);
			mp_next(&pos); mp_next(&pos);
			vpos = mp_decode_str(&pos, &vlen);
			say_info("%.*s", vlen, vpos);
			cas = mp_decode_uint(&pos);
			flags = mp_decode_uint(&pos);
			epos = (struct memcached_get_ext *)&flags;
			elen = sizeof(struct memcached_get_ext);
			if (h->cmd == MEMCACHED_BIN_CMD_GAT ||
			h->cmd == MEMCACHED_BIN_CMD_GATQ) {
				kpos = NULL;
				klen = 0;
			}
		}
		write_output(out, h, MEMCACHED_BIN_RES_OK, cas,
				    elen, klen, vlen,
				    (const char *)epos, kpos, vpos);
	}
	return;
}

void
mc_process_delta(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext == NULL || b->key == NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext == 0) say_error("package has no ext");
		if (b->ext_len != sizeof(struct memcached_delta_ext)) {
			say_error("ext length differs, expected %zu, get %u",
				  sizeof(struct memcached_delta_ext), b->ext_len);
		}
		if (b->key == 0) say_error("package has no key");
		if (b->val != 0) say_error("package has val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	struct memcached_delta_ext *ext = (struct memcached_delta_ext *)b->ext;
	ext->expire  = bswap_u32(ext->expire);
	ext->delta   = bswap_u64(ext->delta);
	ext->initial = bswap_u64(ext->initial);
	say_debug("%s '%.*s' by %lu", mc_get_command_name(h->cmd), b->key_len,
		 b->key, ext->delta);
	uint32_t len = mp_sizeof_array(1) +
		       mp_sizeof_str  (b->key_len);
	char *begin = (char *) region_alloc(&fiber()->gc, len);
	char *end   = mp_encode_array(begin, 1);
	      end   = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);
	box_tuple_t *tuple = NULL;
	uint64_t val = 0;
	uint64_t cas = con->cfg->cas++;
	const char *vpos = NULL;
	uint32_t    vlen = 0;
	char        strval[22]; uint8_t strvallen = 0;
	if (box_index_get(con->cfg->spaceid, 0, begin, end, &tuple) == -1) {
		return mc_process_internal_error(out, h);
	} else if (tuple == NULL || is_expired_tuple(tuple, con->cfg->flush)) {
		if (ext->expire == 0xFFFFFFFFLL) {
			return mc_process_error(out, h,
					MEMCACHED_BIN_RES_KEY_ENOENT, NULL);
		} else {
			ext->expire = 0;
			val = ext->initial;
			/* Insert value */
			strvallen = snprintf(strval, 21, "%lu", val);
			int retval = mc_insert_tuple(b->key, b->key_len,
					ext->expire, (const char *)strval,
					strvallen, cas, 0, con->cfg->spaceid);
			if (retval == -1) {
				return mc_process_internal_error(out, h);
			}
		}
	} else {
		if (ext->expire == 0xFFFFFFFFLL)
			ext->expire = 0;
		const char *pos = box_tuple_field(tuple, 0);
		mp_next(&pos); mp_next(&pos); mp_next(&pos);
		vpos = mp_decode_str(&pos, &vlen);
		say_debug("value before '%.*s', %lu", vlen, vpos, val);
		if (!safe_strtoull(vpos, vpos + vlen, &val)) {
			say_debug("ERROR DELTA_BADVAL");
			return mc_process_error(out, h,
					MEMCACHED_BIN_RES_DELTA_BADVAL, NULL);
		}
		if (h->cmd == MEMCACHED_BIN_CMD_INCR ||
		    h->cmd == MEMCACHED_BIN_CMD_INCRQ) {
			val += ext->delta;
		} else if (ext->delta > val) {
			say_debug("becoming null");
			val = 0;
		} else {
			val -= ext->delta;
		}
		say_debug("value after %lu", val);
		/* Insert value */
		strvallen = snprintf(strval, 21, "%lu", val);
		if (mc_insert_tuple(b->key, b->key_len,
				ext->expire, (const char *)strval,
				strvallen, cas, 0, con->cfg->spaceid) == -1) {
			return mc_process_internal_error(out, h);
		}
	}
	/* Send response */
	if (!con->noreply) {
		val = bswap_u64(val);
		write_output(out, h, MEMCACHED_BIN_RES_OK, cas, 0, 0,
			     sizeof(val), NULL, NULL, (const char *)&val);
	}
}

void
mc_process_pend(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	if (b->ext != NULL || b->key == NULL || b->val == NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key == 0) say_error("package has no key");
		if (b->val == 0) say_error("package has no val");
		con->close_connection = true;
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	con->stat->cmd_set++;
	uint64_t cas = con->cfg->cas++;
	uint32_t len  = mp_sizeof_array (2)      +
			/* splice (app/prepend) operation */
			mp_sizeof_array (5)      +
			mp_sizeof_str   (1)      +
			mp_sizeof_uint  (3)      +
			/* in case of prepend */
			mp_sizeof_uint  (1)      +
			/* in case of append */
			mp_sizeof_int   (-1)     +
			mp_sizeof_uint  (0)      +
			mp_sizeof_str   (b->val_len) +
			/* set cas */
			mp_sizeof_array (3)      +
			mp_sizeof_str   (1)      +
			mp_sizeof_uint  (4)      +
			mp_sizeof_uint  (cas)    +
			mp_sizeof_array (1)      +
			mp_sizeof_str   (b->key_len);
	char *begin  = (char *) region_alloc(&fiber()->gc, len);
	char *end = NULL, *key = NULL;
	/* Encode  */
	end = mp_encode_array(begin, 2);
	/* Encode (app/prepend) */
	end = mp_encode_array(end, 5);
	end = mp_encode_str  (end, ":", 1);
	end = mp_encode_uint (end, 3);
	if (h->cmd == MEMCACHED_BIN_CMD_PREPEND ||
	    h->cmd == MEMCACHED_BIN_CMD_PREPENDQ)
		end = mp_encode_uint(end,  0);
	else
		end = mp_encode_int (end, -1);
	end = mp_encode_uint (end, 0);
	end = mp_encode_str  (end, b->val, b->val_len);
	/* Encode cas update */
	end = mp_encode_array(end, 3);
	end = mp_encode_str  (end, "=", 1);
	end = mp_encode_uint (end, 4);
	end = key = mp_encode_uint (end, cas);
	/* Encode key for update */
	end = mp_encode_array(end, 1);
	end = mp_encode_str  (end, b->key, b->key_len);
	assert(end <= begin + len);

	box_tuple_t *tuple = NULL;
	if (box_index_get(con->cfg->spaceid, 0, key, end, &tuple) == -1) {
		return mc_process_internal_error(out, h);
	} else if (tuple == NULL || is_expired_tuple(tuple, con->cfg->flush)) {
		return mc_process_error(out, h, MEMCACHED_BIN_RES_KEY_ENOENT,
		       NULL);
	}

	/* Tuple can't be NULL, because we already found this element */
	if (box_update(con->cfg->spaceid, 0, key,
		       end, begin, key, 0, &tuple) == -1) {
		mc_process_internal_error(out, h);
	} else {
		if (!con->noreply) {
			write_output(out, h, MEMCACHED_BIN_RES_OK, cas,
				     0, 0, 0, NULL, NULL, NULL);
		}
	}
	return;
}

void
mc_process_quit(struct memcached_connection *con)
{
	/* default declarations */
	struct memcached_hdr  *h = con->hdr;
	struct memcached_body *b = &con->body;
	struct obuf *out = &(con->iobuf->out);

	con->close_connection = true;
	if (b->ext != NULL || b->key != NULL || b->val != NULL) {
		say_error("problem while parsing package '%s'"
			  " with opaque %" PRIu32,
			  mc_get_command_name(h->cmd), h->opaque);
		if (b->ext != 0) say_error("package has ext");
		if (b->key != 0) say_error("package has key");
		if (b->val != 0) say_error("package has val");
		return mc_process_error(out, h, MEMCACHED_BIN_RES_EINVAL, NULL);
	}
	if (!con->noreply) {
		write_output(out, h, MEMCACHED_BIN_RES_OK, 0, 0, 0, 0, NULL,
			     NULL, NULL);
	}
}

void stat_append(struct memcached_connection *con,
		const char *key, const char *valfmt, ...) {
	struct memcached_hdr *h   = con->hdr;
	struct obuf          *out = &(con->iobuf->out);
	size_t key_len = strlen(key);
	va_list va; va_start(va, valfmt);
	char val[256] = {0};
	size_t val_len = vsnprintf(val, 256, valfmt, va);
	write_output(out, h, 0, 0, 0, key_len, val_len, NULL, key, val);
};

void
mc_process_stats(struct memcached_connection *con)
{
	/* server specific data */
	stat_append(con, "pid", "%d", getpid());
	stat_append(con, "uptime", "%lf", tarantool_uptime());
	stat_append(con, "time", "%lf", fiber_time());
	stat_append(con, "version", "Memcached (Tarantool " PACKAGE_VERSION ")");
/*	stat_append(con, "libev")   */
	stat_append(con, "pointer_size", "%d", (int )(8 * sizeof(void *)));

	/* storage specific data */
	stat_append(con, "cmd_get",       "%lu", con->stat->cmd_get);
	stat_append(con, "get_hits",      "%lu", con->stat->get_hits);
	stat_append(con, "get_misses",    "%lu", con->stat->get_misses);
	stat_append(con, "cmd_set",       "%lu", con->stat->cmd_set);
	stat_append(con, "cas_hits",      "%lu", con->stat->cas_hits);
	stat_append(con, "cas_badval",    "%lu", con->stat->cas_badval);
	stat_append(con, "cas_misses",    "%lu", con->stat->cas_misses);
	stat_append(con, "cmd_delete",    "%lu", con->stat->cmd_delete);
	stat_append(con, "delete_hits",   "%lu", con->stat->delete_hits);
	stat_append(con, "delete_misses", "%lu", con->stat->delete_misses);
	stat_append(con, "cmd_incr",      "%lu", con->stat->cmd_incr);
	stat_append(con, "incr_hits",     "%lu", con->stat->incr_hits);
	stat_append(con, "incr_misses",   "%lu", con->stat->incr_misses);
	stat_append(con, "cmd_decr",      "%lu", con->stat->cmd_decr);
	stat_append(con, "decr_hits",     "%lu", con->stat->decr_hits);
	stat_append(con, "decr_misses",   "%lu", con->stat->decr_misses);
	stat_append(con, "cmd_flush",     "%lu", con->stat->cmd_flush);
	stat_append(con, "cmd_touch",     "%lu", con->stat->cmd_touch);
	stat_append(con, "touch_hits",    "%lu", con->stat->touch_hits);
	stat_append(con, "touch_misses",  "%lu", con->stat->touch_misses);
	/* finish */
	stat_append(con, "", "");
}
