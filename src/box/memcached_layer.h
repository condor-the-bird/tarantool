#ifndef   TARANTOOL_BOX_MEMCACHED_LAYER_H_INCLUDED
#define   TARANTOOL_BOX_MEMCACHED_LAYER_H_INCLUDED

void mc_process_set     (struct memcached_connection *con);
void mc_process_get     (struct memcached_connection *con);
void mc_process_del     (struct memcached_connection *con);
void mc_process_nop     (struct memcached_connection *con);
void mc_process_flush   (struct memcached_connection *con);
void mc_process_gat     (struct memcached_connection *con);
void mc_process_version (struct memcached_connection *con);
void mc_process_delta   (struct memcached_connection *con);
void mc_process_pend    (struct memcached_connection *con);
void mc_process_quit    (struct memcached_connection *con);
void mc_process_stats   (struct memcached_connection *con);

#endif /* TARANTOOL_BOX_MEMCACHED_LAYER_H_INCLUDED */
