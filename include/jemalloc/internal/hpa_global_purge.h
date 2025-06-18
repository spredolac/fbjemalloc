#ifndef JEMALLOC_INTERNAL_HPA_GLOBAL_PURGE_H
#define JEMALLOC_INTERNAL_HPA_GLOBAL_PURGE_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/ph.h"
#include "jemalloc/internal/nstime.h"

/* Background thread will use this to apply purging policy */
typedef struct hpa_purge_analytics_s hpa_purge_analytics_t;

/* Priority queue for shards with variety of pages inside */
#define HAP_GPHEAP_ENUMERATE_MAX_NUM 32 /* Check if this matters at all and point to comment to edata.h */
ph_structs(hpa_gpheap, hpa_purge_analytics_t, HAP_GPHEAP_ENUMERATE_MAX_NUM);
ph_proto(, hpa_gpheap, hpa_purge_analytics_t);
int
hpa_gpheap_cmp(const hpa_purge_analytics_t *a, const hpa_purge_analytics_t *b);

struct hpa_purge_analytics_s {
	psset_bin_stats_t stats;
	hpa_shard_t *shard;
	hpa_gpheap_link_t heap_link;
	bool hugify_blocked_by_dirty;
	bool should_purge;
};

typedef struct hpa_global_purge_s hpa_global_purge_t;
struct hpa_global_purge_s {
	/* Will be dynamically allocated by background thread */
	hpa_purge_analytics_t *analytics;
	size_t analytics_capacity;

	/* Start of the current period */
	nstime_t period_start;
	/* Max number of regular active pages in prior period */
	size_t prior_active_max;
	/* Max number of regular active pages at last tick */
	size_t current_active_max;
	/* Number of regular dirty pages at last tick */
	size_t ndirty;
	size_t tick_counter;
};

void hpa_global_purge_init(hpa_global_purge_t *hpa_purge);
void hpa_global_purge_realloc(
    tsdn_t *tsdn, hpa_global_purge_t *ppurge, size_t nslots);
size_t hpa_global_purge_read(tsdn_t *tsdn, hpa_global_purge_t *hpa_purge,
    hpa_shard_t **shards, size_t nshards);
size_t hpa_global_purge_tick(
    hpa_global_purge_t *hpa_purge, size_t nshards, nstime_t *now);
size_t hpa_global_purge(
	tsdn_t *tsdn, hpa_global_purge_t *hpa_purge, size_t nshards, size_t npurge, bool to_hugify_only);

#endif /* JEMALLOC_INTERNAL_HPA_GLOBAL_PURGE_H */
