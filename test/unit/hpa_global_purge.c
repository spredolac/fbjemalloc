#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/nstime.h"

#define SHARD_IND 111

#define ALLOC_MAX (HUGEPAGE)

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
	 * Must be the first member -- we convert back and forth between the
	 * test_data_t and the hpa_shard_t;
	 */
	hpa_shard_t shard;
	hpa_central_t central;
	base_t *base;
	edata_cache_t shard_edata_cache;

	emap_t emap;
};

static void
test_init_shard_stats(hpa_purge_analytics_t *dest, size_t npending,
		      bool hug_blocked, size_t ndirty, size_t nactive) {
	dest->shard = NULL;
	dest->npending_purge = npending;
	dest->hugify_blocked_by_dirty = hug_blocked;
	dest->stats.merged.ndirty = ndirty;
	dest->stats.merged.nactive = nactive;
	dest->stats.merged.npageslabs = 0;
	
}

static void
test_init_shard_stats_full(hpa_purge_analytics_t *dest, void *fake_shard,
			   bool hug_blocked, size_t ndirty,
			   fb_group_t *purge_bitmap) {
	test_init_shard_stats(dest, 0, hug_blocked, ndirty, 0);
	dest->shard = fake_shard;
	const size_t BMP_SZ =
	    sizeof(fb_group_t) * FB_NGROUPS(PSSET_NPURGE_LISTS);
	memcpy(dest->purge_bitmap, purge_bitmap, BMP_SZ);
}

TEST_BEGIN(test_max_over_period) {
	test_skip_if(!hpa_supported() || !config_stats);

	opt_global_purge_period_secs = 10;

	hpa_global_purge_t hpa_purge;
	hpa_global_purge_init(&hpa_purge);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	const size_t NSHARDS = 2;
	hpa_global_purge_realloc(tsdn, &hpa_purge, NSHARDS);
	expect_zu_eq(hpa_purge.analytics_capacity, NSHARDS, "");

	/* We are testing the max logic now, so just setup fake stats */
	hpa_purge_analytics_t *analytics = hpa_purge.analytics;
	test_init_shard_stats(&analytics[0], 10, false, 20, 100);
	test_init_shard_stats(&analytics[1], 0, false, 30, 200);

	nstime_t start;
	nstime_init2(&start, 100, 0);
	nstime_copy(&hpa_purge.period_start, &start);

	nstime_t after_sec;
	nstime_init2(&after_sec, 101, 0);
	nstime_t after_period_sec;
	nstime_init2(&after_period_sec, 100 + opt_global_purge_period_secs, 0);
	nstime_t after_2period_sec;
	nstime_init2(&after_2period_sec, 100 + 2* opt_global_purge_period_secs,
		     0);

	/* Advance one second and update the stats */
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &after_sec);
	expect_zu_eq(hpa_purge.prior_active_max, 0, "");
	expect_zu_eq(hpa_purge.current_active_max, 300, "");

	test_init_shard_stats(&analytics[1], 60, false, 40, 300);
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &after_sec);
	expect_zu_eq(hpa_purge.prior_active_max, 0, "");
	expect_zu_eq(hpa_purge.current_active_max, 400, "");

	test_init_shard_stats(&analytics[1], 60, false, 40, 100);
	/* Advance one second and update the stats */
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &after_period_sec);

	expect_zu_eq(hpa_purge.prior_active_max, 400, "");
	expect_zu_eq(hpa_purge.current_active_max, 200, "");

	test_init_shard_stats(&analytics[1], 60, false, 0, 10);
	/* Advance one second and update the stats */
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &after_2period_sec);
	expect_zu_eq(hpa_purge.prior_active_max, 200, "");
	expect_zu_eq(hpa_purge.current_active_max, 110, "");
}
TEST_END

TEST_BEGIN(test_calc_target) {
	test_skip_if(!hpa_supported() || !config_stats);

	opt_global_purge_period_secs = 10;

	hpa_global_purge_t hpa_purge;
	hpa_global_purge_init(&hpa_purge);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	const size_t NSHARDS = 2;
	hpa_global_purge_realloc(tsdn, &hpa_purge, NSHARDS);
	expect_zu_eq(hpa_purge.analytics_capacity, NSHARDS, "");

	/* Have 300 active pages, and 50 touched */
	hpa_purge_analytics_t *analytics = hpa_purge.analytics;
	test_init_shard_stats(&analytics[0], 0, false, 20, 100);
	test_init_shard_stats(&analytics[1], 0, false, 30, 200);

	nstime_t start;
	nstime_t period1;
	nstime_t period2;
        nstime_init2(&start, 100, 0);
        nstime_init2(&period1, 100 + opt_global_purge_period_secs, 0);
	nstime_init2(&period2, 100 + 2 * opt_global_purge_period_secs, 0);

	nstime_copy(&hpa_purge.period_start, &start);
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &start);
	expect_zu_eq(hpa_purge.prior_active_max, 0, "");
	expect_zu_eq(hpa_purge.current_active_max, 300, "");

	/*
	 * Have 200 active pages, and 160 touched. Prior max becomes 300,
	 * and new max is 200.  Our number of dirty pages allowed should be
	 * 300 - 200 = 100, thus, our target is 160 - 100 = 60.
	 */
	test_init_shard_stats(&analytics[1], 0, false, 140, 100);
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &period1);
	expect_zu_eq(hpa_purge.prior_active_max, 300, "");
	expect_zu_eq(hpa_purge.current_active_max, 200, "");
	expect_zu_eq(hpa_purge.ntarget, 60,"Start the purge after 1st period");

	/*
	 * Have current active 110, prior active max is 200, and 120 dirty.
	 * Target should be 120 - 90 = 30.  Then we change the number of pending
	 * to 30 and target should be 0 if none of the other conditions changed.
	 */
        test_init_shard_stats(&analytics[1], 0, false, 100, 10);
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &period2);
        expect_zu_eq(hpa_purge.prior_active_max, 200, "");
	expect_zu_eq(hpa_purge.current_active_max, 110, "");
        expect_zu_eq(hpa_purge.ntarget, 30, "");

        test_init_shard_stats(&analytics[1], 30, false, 100, 10);
	hpa_global_purge_tick(&hpa_purge, NSHARDS, &period2);
        expect_zu_eq(hpa_purge.prior_active_max, 200, "");
	expect_zu_eq(hpa_purge.current_active_max, 110, "");	
        expect_zu_eq(hpa_purge.ntarget, 0, "Pending not taken into account");
}
TEST_END

extern size_t (*hpa_try_purge_then_hugify_hook)(tsdn_t *, hpa_shard_t *, size_t);
extern size_t (*hpa_try_hugify_all_hook)(tsdn_t *, hpa_shard_t *);

#define HPA_GLOBAL_PURGE_TEST_MAX_CALLS 256
static size_t nshards_purge;
static size_t purge_return_value;
static void *shards_purge[HPA_GLOBAL_PURGE_TEST_MAX_CALLS];

static size_t
test_purge_then_hugify(tsdn_t *tsdn, hpa_shard_t *shard, size_t ntarget) {
	(void) tsdn;
	(void) ntarget;
	if (nshards_purge < HPA_GLOBAL_PURGE_TEST_MAX_CALLS) {
		shards_purge[nshards_purge++] = (void *)shard;
	}
	return purge_return_value;
}

static size_t nshards_hugify;
static void *shards_hugify[HPA_GLOBAL_PURGE_TEST_MAX_CALLS];

static size_t
test_hugify_all(tsdn_t *tsdn, hpa_shard_t *shard) {
	(void) tsdn;
	if (nshards_hugify < HPA_GLOBAL_PURGE_TEST_MAX_CALLS) {
		shards_hugify[nshards_hugify++] = (void *) shard;
	}
	return 0;
}

static void
set_hooks(size_t retval) {
	hpa_try_purge_then_hugify_hook = test_purge_then_hugify;
	hpa_try_hugify_all_hook = test_hugify_all;
	nshards_hugify = 0;
	nshards_purge = 0;
	purge_return_value = retval;
}

static void
clear_hooks(void) {
	hpa_try_purge_then_hugify_hook = NULL;
        hpa_try_hugify_all_hook = NULL;
	purge_return_value = 0;
}

TEST_BEGIN(test_global_purge) {
	test_skip_if(!hpa_supported() || !config_stats);
	opt_global_purge_period_secs = 10;

        hpa_global_purge_t hpa_purge;
	hpa_global_purge_init(&hpa_purge);

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	const size_t NSHARDS = 4;
	hpa_global_purge_realloc(tsdn, &hpa_purge, NSHARDS);
	expect_zu_eq(hpa_purge.analytics_capacity, NSHARDS, "");

	size_t target = 100; /* simulate deletion of 100 pages */
	set_hooks(target / 2); /* simulate shards cleaning 50 */

	fb_group_t pbmp_low[FB_NGROUPS(PSSET_NPURGE_LISTS)] = {
		0xf, 0x5
	};

	fb_group_t pbmp_high[FB_NGROUPS(PSSET_NPURGE_LISTS)] = {
		0xe, 0x7
	};

	void *shard1 = (void *)0x1;
	void *shard2 = (void *)0x2;
	void *shard3 = (void *)0x3;
	void *shard4 = (void *)0x4;

	hpa_purge_analytics_t *analytics = hpa_purge.analytics;
	test_init_shard_stats_full(&analytics[0], shard2, true, 20, pbmp_low);
	test_init_shard_stats_full(&analytics[1], shard4, false, 30, pbmp_low);
	test_init_shard_stats_full(&analytics[2], shard3, false, 50, pbmp_low);
        test_init_shard_stats_full(&analytics[3], shard1, true, 20, pbmp_high);

	hpa_global_purge(tsdn, &hpa_purge, NSHARDS, target);

	/* We expect two calls as our hook returns ntarget - 1 */
	expect_zu_eq(nshards_purge, 2, "Expected 2 calls until target filled");
	expect_ptr_eq(shards_purge[0], shard1, "blocks hugify and high bitmap");
	expect_ptr_eq(shards_purge[1], shard2, "blocks hugify and high bitmap");

	expect_zu_eq(nshards_hugify, 2,
		     "Hugify should be called for non-purged as well");
	expect_ptr_eq(shards_hugify[0], shard3, "more dirty than shard4");
	expect_ptr_eq(shards_hugify[1], shard4, "missing last call");

	clear_hooks();
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
		test_max_over_period,
		test_calc_target,
		test_global_purge);
}
