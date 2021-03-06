#ifndef TARANTOOL_WAL_WRITER_H_INCLUDED
#define TARANTOOL_WAL_WRITER_H_INCLUDED
/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
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
#include  <third_party/queue.h>
#include <stdint.h>
#include "cbus.h"

struct fiber;
struct recovery_state;

enum wal_mode { WAL_NONE = 0, WAL_WRITE, WAL_FSYNC, WAL_MODE_MAX };

/** String constants for the supported modes. */
extern const char *wal_mode_STRS[];

struct wal_request: public cmsg {
	/* Auxiliary. */
	int64_t res;
	struct fiber *fiber;
	/* Relative position of the start of request (used for rollback) */
	off_t start_offset;
	/* Relative position of the end of request (used for rollback) */
	off_t end_offset;
	int n_rows;
	struct xrow_header *rows[];
};

int64_t
wal_write(struct recovery_state *r, struct wal_request *req);

int
wal_writer_start(struct recovery_state *state, int rows_per_wal);

void
wal_writer_stop(struct recovery_state *r);

#endif /* TARANTOOL_WAL_WRITER_H_INCLUDED */
