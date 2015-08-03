-- memcached.lua

local ffi = require('ffi')
ffi.cdef[[
struct memcached_cfg {
    uint32_t spaceid;
    uint64_t cas;
    bool     flush_enabled;
    uint64_t flush;
};

typedef double time_t;

struct memcached_stats {
    unsigned int  curr_items;
    unsigned int  total_items;
    unsigned int  curr_conns;
    unsigned int  total_conns;
    uint64_t      bytes_read;
    uint64_t      bytes_written;
    time_t        started;
    uint64_t      cmd_get;
    uint64_t      get_hits;
    uint64_t      get_misses;
    uint64_t      cmd_delete;
    uint64_t      delete_hits;
    uint64_t      delete_misses;
    uint64_t      cmd_set;
    uint64_t      cas_hits;
    uint64_t      cas_badval;
    uint64_t      cas_misses;
    uint64_t      cmd_incr;
    uint64_t      incr_hits;
    uint64_t      incr_misses;
    uint64_t      cmd_decr;
    uint64_t      decr_hits;
    uint64_t      decr_misses;
    uint64_t      cmd_touch;
    uint64_t      touch_hits;
    uint64_t      touch_misses;
    uint64_t      cmd_flush;
    uint64_t      evictions;
    uint64_t      reclaimed;
    uint64_t      auth_cmds;
    uint64_t      auth_errors;
};

struct memcached_pair {
    struct memcached_cfg   *cfg;
    struct memcached_stats *stat;
};

void
memcached_set_listen(const char *name, const char *uri,
                     struct memcached_pair *pair);
]]

local mccfg_t   = ffi.typeof('struct memcached_cfg[1]');
local mcstats_t = ffi.typeof('struct memcached_stats[1]');
local mcpair_t  = ffi.typeof('struct memcached_pair[1]');

local mc = {
}

local function memcached_start(opts)
    opts = opts or {}
    if opts.name == nil then
        box.error(box.error.PROC_LUA, 'name must be provided')
    elseif mc[opts.name] ~= nil then
        box.error(box.error.PROC_LUA, 'instance already started')
    end
    opts.uri = opts.uri or '0.0.0.0:11211'
    local conf = {}
    conf.space = box.schema.create_space(opts.name)
    conf.space:create_index('primary', { parts = {1, 'str'} })
    box.schema.user.grant('guest', 'read,write', 'space', opts.name)
    conf.opts = opts
    conf.cfg  = mccfg_t(); ffi.fill(conf.cfg, ffi.sizeof(mccfg_t))
    require('log').info("%d", ffi.sizeof(mccfg_t))
    conf.stat = mcstats_t(); ffi.fill(conf.stat, ffi.sizeof(mcstats_t))
    require('log').info("%d", ffi.sizeof(mcstats_t))
    conf.pair = mcpair_t()
    conf.pair[0].cfg = conf.cfg
    conf.pair[0].stat = conf.stat
    conf.cfg[0].spaceid = conf.space.id
    conf.cfg[0].flush_enabled = true
    local rc = ffi.C.memcached_set_listen(opts.name, opts.uri, conf.pair)
    if 1 < 0 then
        box.error(box.error.PROC_LUA, 'error while binding on port')
    end
    mc[opts.name] = opts
end

return {
    start = memcached_start;
    debug = mc
}
