
#include "wt_internal.h"

/*��ȡevict LRU cache������*/
static int __cache_config_local(WT_SESSION_IMPL* session, int shared, const char* cfg[])
{
	WT_CACHE* cache;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	uint32_t evict_workers_max, evict_workers_min;

	conn = S2C(session);
	cache = conn->cache;

	/*��ȡcache size*/
	if (!shared){
		WT_RET(__wt_config_gets(session, cfg, "cache_size", &cval));
		conn->cache_size = (uint64_t)cval.val;
	}

	WT_RET(__wt_config_gets(session, cfg, "cache_overhead", &cval));
	cache->overhead_pct = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_target", &cval));
	cache->eviction_target = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
	cache->eviction_trigger = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_target", &cval));
	cache->eviction_dirty_target = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
	WT_ASSERT(session, cval.val > 0);
	evict_workers_max = (uint32_t)cval.val - 1;

	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_min", &cval));
	WT_ASSERT(session, cval.val > 0);
	evict_workers_min = (uint32_t)cval.val - 1;

	/*�Ϸ���У��*/
	if (evict_workers_min > evict_workers_max)
		WT_RET_MSG(session, EINVAL, "eviction=(threads_min) cannot be greater than eviction=(threads_max)");
	conn->evict_workers_max = evict_workers_max;
	conn->evict_workers_min = evict_workers_min;

	return 0;
}

/*��ȡ������Ϣ����������Ϣ���õ���ǰconn��cache��Ϣ*/
int __wt_cache_config(WT_SESSION_IMPL *session, int reconfigure, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	int now_shared, was_shared;

	conn = S2C(session);

	WT_ASSERT(session, conn->cache != NULL);

	WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
	now_shared = cval.len != 0;
	was_shared = F_ISSET(conn, WT_CONN_CACHE_POOL);

	/* Cleanup if reconfiguring */
	if (reconfigure && was_shared && !now_shared)
		/* Remove ourselves from the pool if necessary */
		WT_RET(__wt_conn_cache_pool_destroy(session)); /*���ԭ����cache pool����connection cache,���ڵ��������óɶ�����cache��������ô��cache pool��ɾ��������ϵ*/
	else if (reconfigure && !was_shared && now_shared)
		/*
		* Cache size will now be managed by the cache pool - the
		* start size always needs to be zero to allow the pool to
		* manage how much memory is in-use.
		*/
		conn->cache_size = 0;

	/*����connection��cache*/
	WT_RET(__cache_config_local(session, now_shared, cfg));
	if (now_shared) {
		WT_RET(__wt_cache_pool_config(session, cfg)); /*��cache pool�����ø���*/
		WT_ASSERT(session, F_ISSET(conn, WT_CONN_CACHE_POOL));
		if (!was_shared)
			WT_RET(__wt_conn_cache_pool_open(session)); /*��connection cache���뵽cache pool���н��й���*/
	}

	return 0;
}

/*����һ��connection evict cache*/
int __wt_cache_create(WT_SESSION_IMPL* session, const char* cfg[])
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	WT_RET(__wt_calloc_one(session, &conn->cache));
	cache = conn->cache;

	/*��cache��������*/
	WT_RET(__wt_cache_config(session, 0, cfg));

	if (cache->eviction_target >= cache->eviction_trigger)
		WT_ERR_MSG(session, EINVAL, "eviction target must be lower than the eviction trigger");

	/*����evict cond�ź���*/
	WT_ERR(__wt_cond_alloc(session, "cache eviction server", 0, &cache->evict_cond));
	WT_ERR(__wt_cond_alloc(session, "eviction waiters", 0, &cache->evict_waiter_cond));
	WT_ERR(__wt_spin_init(session, &cache->evict_lock, "cache eviction"));
	WT_ERR(__wt_spin_init(session, &cache->evict_walk_lock, "cache walk"));

	/* Allocate the LRU eviction queue. */
	cache->evict_slots = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	WT_ERR(__wt_calloc_def(session, cache->evict_slots, &cache->evict));

	/*��ʼ��cache statͳ��ģ��*/
	__wt_cache_stats_update(session);
	return 0;

err:
	WT_RET(__wt_cache_destroy(session));
	return ret;
}

/*����cache��״̬����cache��ͳ����Ϣ*/
void __wt_cache_stats_update(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS *stats;

	conn = S2C(session);
	cache = conn->cache;
	stats = &conn->stats;

	WT_STAT_SET(stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));

	WT_STAT_SET(stats, cache_overhead, cache->overhead_pct);
	WT_STAT_SET(stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
	WT_STAT_SET(stats, cache_bytes_dirty, __wt_cache_dirty_inuse(cache));
	WT_STAT_SET(stats, cache_eviction_maximum_page_size, cache->evict_max_page_size);
	WT_STAT_SET(stats, cache_pages_dirty, cache->pages_dirty);

	/* Figure out internal, leaf and overflow stats */
	WT_STAT_SET(stats, cache_bytes_internal, cache->bytes_internal);
	WT_STAT_SET(stats, cache_bytes_leaf, conn->cache_size - (cache->bytes_internal + cache->bytes_overflow));
	WT_STAT_SET(stats, cache_bytes_overflow, cache->bytes_overflow);
}

/*����һ��connection evict cache����*/
int __wt_cache_destroy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	cache = conn->cache;

	if (cache == NULL)
		return 0;

	WT_TRET(__wt_cond_destroy(session, &cache->evict_cond));
	WT_TRET(__wt_cond_destroy(session, &cache->evict_waiter_cond));
	__wt_spin_destroy(session, &cache->evict_lock);
	__wt_spin_destroy(session, &cache->evict_walk_lock);

	__wt_free(session, cache->evict);
	__wt_free(session, conn->cache);
	return ret;
}