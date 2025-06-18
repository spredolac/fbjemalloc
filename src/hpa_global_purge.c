#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

size_t opt_global_purge_period_secs = 10;

ph_gen(,hpa_gpheap, hpa_purge_analytics_t, heap_link, hpa_gpheap_cmp);

void
hpa_global_purge_init(hpa_global_purge_t *hpa_purge) {
	hpa_purge->analytics = NULL;
	hpa_purge->analytics_capacity = 0;
	hpa_purge->current_active_max = 0;
	hpa_purge->prior_active_max = 0;
	hpa_purge->ntarget = 0;
	hpa_purge->ndirty = 0;
	hpa_purge->npending = 0;
	nstime_init_update(&hpa_purge->period_start);
}

#define BILLION UINT64_C(1000000000)

void
hpa_global_purge_realloc(
    tsdn_t *tsdn, hpa_global_purge_t *ppurge, size_t nslots) {
	if (opt_global_purge_period_secs == 0) {
		return;
	}

	if (ppurge->analytics_capacity >= nslots) {
		return;
	}
	if (ppurge->analytics != NULL) {
		idalloctm(tsdn, ppurge->analytics, NULL, NULL, true, true);
	}
	size_t stat_size = sz_sa2u(
	    sizeof(hpa_purge_analytics_t) * nslots, PAGE);
	ppurge->analytics = ipallocztm(
	    tsdn, stat_size, PAGE, true, NULL, true, NULL);
	if (ppurge->analytics) {
		ppurge->analytics_capacity = nslots;
	} else {
		malloc_printf("Cannot allocate memory for global purging\n");
	}
}

#define MAX_ANALYTICS_FAILED_TRYLOCKS 256

static size_t
read_for_failed_locks(tsdn_t *tsdn, hpa_global_purge_t *hpa_purge,
    hpa_shard_t **failed, size_t nfailed, size_t dest_idx) {
	for (size_t i = 0; i < nfailed; ++i) {
		assert(dest_idx < hpa_purge->analytics_capacity);
		hpa_purge_analytics_t *dest = &hpa_purge->analytics[dest_idx];
		hpa_purge_analytics_read(tsdn, failed[i], false,
					 &dest->npending_purge, &dest->stats,
					 &dest->purge_bitmap[0],
					 &dest->hugify_blocked_by_dirty);
		dest->shard = failed[i];
		dest_idx++;
	}
	return dest_idx;
}

size_t
hpa_global_purge_read(tsdn_t *tsdn, hpa_global_purge_t *hpa_purge,
    hpa_shard_t **shards, size_t nshards) {
	assert(opt_global_purge_period_secs != 0);

	hpa_shard_t *failed_locks[MAX_ANALYTICS_FAILED_TRYLOCKS];
	size_t nfailed_locks = 0;

	if (hpa_purge->analytics == NULL) {
		return 0;
	}

	size_t dest_idx = 0;
	for (size_t i = 0; i < nshards; ++i) {
		assert(dest_idx < hpa_purge->analytics_capacity);
		hpa_purge_analytics_t *dest = &hpa_purge->analytics[dest_idx];
		bool err = hpa_purge_analytics_read(
			tsdn, shards[i], true, &dest->npending_purge,
			&dest->stats, &dest->purge_bitmap[0],
			&dest->hugify_blocked_by_dirty);
		if (!err) {
			dest->shard = shards[i];
			dest_idx++;
		} else {
			failed_locks[nfailed_locks] = shards[i];
			nfailed_locks++;
			if (nfailed_locks == MAX_ANALYTICS_FAILED_TRYLOCKS) {
				dest_idx = read_for_failed_locks(tsdn,
				    hpa_purge, failed_locks, nfailed_locks,
				    dest_idx);
				nfailed_locks = 0;
			}
		}
	}
	dest_idx = read_for_failed_locks(
	    tsdn, hpa_purge, failed_locks, nfailed_locks, dest_idx);
	return dest_idx;
}

static inline void
hpa_purge_period_advance(hpa_global_purge_t *hpa_purge, nstime_t *now,
    size_t nactive, size_t ndirty, size_t npending) {
	nstime_copy(&hpa_purge->period_start, now);
	hpa_purge->prior_active_max = hpa_purge->current_active_max;
	hpa_purge->current_active_max = nactive;
}

static inline size_t
hpa_purge_set_target(hpa_global_purge_t *hpa_purge, nstime_t *now,
    size_t ndirty, size_t npending) {
	/* todo decay */
	size_t prior_max = hpa_purge->prior_active_max;
	size_t curr_max = hpa_purge->current_active_max;
	size_t ntarget = 0;
	if (prior_max >= curr_max) {
		size_t dirtymax = prior_max - curr_max;
		if (ndirty > npending && ndirty - npending > dirtymax) {
			ntarget = ndirty - npending - dirtymax;
		}
	}
	hpa_purge->ntarget = ntarget;
	return hpa_purge->ntarget;
}

size_t
hpa_global_purge_tick(
    hpa_global_purge_t *hpa_purge, size_t nshards, nstime_t *now) {
	assert(nshards <= hpa_purge->analytics_capacity);
	size_t nactive = 0;
	size_t ndirty = 0;
	size_t npending = 0;
	for (size_t i = 0; i < nshards; ++i) {
		nactive += hpa_purge->analytics[i].stats.merged.nactive;
		ndirty += hpa_purge->analytics[i].stats.merged.ndirty;
		npending += hpa_purge->analytics[i].npending_purge;
	}

	if (unlikely(!nstime_monotonic()
	        && nstime_compare(&hpa_purge->period_start, now) > 0)) {
		/* Time moved backwards, we reset the plan */
		hpa_purge_period_advance(
		    hpa_purge, now, nactive, ndirty, npending);
		return hpa_purge_set_target(hpa_purge, now, ndirty, npending);
	}

	nstime_t period_ago;
	nstime_copy(&period_ago, now);
	const uint64_t PERIOD_IN_NS = opt_global_purge_period_secs * BILLION;
	nstime_isubtract(&period_ago, PERIOD_IN_NS);
	if (nstime_compare(&hpa_purge->period_start, &period_ago) <= 0) {
		hpa_purge_period_advance(
		    hpa_purge, now, nactive, ndirty, npending);
		return hpa_purge_set_target(hpa_purge, now, ndirty, npending);
	}

	if (nactive > hpa_purge->current_active_max) {
		hpa_purge->current_active_max = nactive;
	}
	return hpa_purge_set_target(hpa_purge, now, ndirty, npending);
}

size_t
hpa_global_purge(tsdn_t *tsdn, hpa_global_purge_t *hpa_purge, size_t nshards,
    size_t npurge) {
	assert(npurge > 0);

	hpa_gpheap_t pheap;
	hpa_gpheap_new(&pheap);

	for(size_t i = 0; i<nshards; ++i) {
		hpa_gpheap_insert(&pheap, &hpa_purge->analytics[i]);
	}
	size_t purged = 0;
	while (purged < npurge && !hpa_gpheap_empty(&pheap)) {
		size_t ntarget = npurge - purged;
		hpa_purge_analytics_t *cur = hpa_gpheap_remove_first(&pheap);
		size_t ndirty = hpa_try_purge_then_hugify(tsdn, cur->shard,
							  ntarget);
		purged += ndirty;
	}
	size_t nhugified = 0;
	while (!hpa_gpheap_empty(&pheap)) {
                hpa_purge_analytics_t *cur = hpa_gpheap_remove_first(&pheap);
		nhugified += hpa_try_hugify_all(tsdn, cur->shard);
        }
	/* Will use it for USDT */
	(void) nhugified;
	return purged;
}

int hpa_gpheap_cmp(const hpa_purge_analytics_t *a,
                   const hpa_purge_analytics_t *b) {
	if (a->hugify_blocked_by_dirty) {
		if (!b->hugify_blocked_by_dirty) {
			return -1;
		}
	} else {
		if (b->hugify_blocked_by_dirty) {
			return 1;
		}
	}

	assert(a->hugify_blocked_by_dirty == b->hugify_blocked_by_dirty);
	/*
	 * Just go in the order of prefered purge.  When we want to change that
	 * order, we should just change the way those lists are created. For
	 * example, we may want to put all non-huge pages at the end and order
	 * them based on longest free range as a proxy for number of
	 * allocations, or use size-based routing. We can also replace lists
	 * with age heaps if needed. In any, case the logic here will not need to
	 * change much (meaning we will use purge_bitmap)
	 * TODO: experiment with utilization, size-route, age, LFR
	 */
	
	/*
	 * Purge_bitmap has at index 2*i HUGE for that number of dirty,
	 * at 2*i + 1 nh for that size.  Special case is the last one
	 * where N-1 is huge and N-2 is nh
	 */
	for (size_t i = 0; i < FB_NGROUPS(PSSET_NPURGE_LISTS); i++) {
		size_t ind = FB_NGROUPS(PSSET_NPURGE_LISTS) - 1 - i;
		if (a->purge_bitmap[ind] < b->purge_bitmap[ind]) {
			return 1; /* Pick b first */
		} else if (a->purge_bitmap[ind] > b->purge_bitmap[ind]) {
			return -1; /* Pick a first */
		}
	}

        const psset_stats_t *as = &a->stats;
	const psset_stats_t *bs = &b->stats;

	/* Look at overall number of dirty now. TODO: nh dirty first */
	if (as->merged.ndirty < bs->merged.ndirty) {
		return 1;
	} else if (as->merged.ndirty > bs->merged.ndirty) {
		return -1;
	}
	/*TODO add age of the shard here*/
	assert(a->shard != b->shard);
 
	return (uintptr_t) a->shard < (uintptr_t) b->shard ? -1 : 1;
}
