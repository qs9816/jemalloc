#define JEMALLOC_TCACHE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

bool	opt_tcache = true;
ssize_t	opt_lg_tcache_max = LG_TCACHE_MAXCLASS_DEFAULT;

tcache_bin_info_t	*tcache_bin_info;
static unsigned		stack_nelms; /* Total stack elms per tcache. */

unsigned		nhbins;
size_t			tcache_maxclass;

tcaches_t		*tcaches;

/* Index of first element within tcaches that has never been used. */
static unsigned		tcaches_past;

/* Head of singly linked list tracking available tcaches elements. */
static tcaches_t	*tcaches_avail;

/* Protects tcaches{,_past,_avail}. */
static malloc_mutex_t	tcaches_mtx;

/******************************************************************************/

size_t
tcache_salloc(tsdn_t *tsdn, const void *ptr) {
	return arena_salloc(tsdn, ptr);
}

void
tcache_event_hard(tsd_t *tsd, tcache_t *tcache) {
	szind_t binind = tcache->next_gc_bin;
	tcache_bin_t *tbin = &tcache->tbins[binind];
	tcache_bin_info_t *tbin_info = &tcache_bin_info[binind];

	if (tbin->low_water > 0) {
		/*
		 * Flush (ceiling) 3/4 of the objects below the low water mark.
		 */
		if (binind < NBINS) {
			tcache_bin_flush_small(tsd, tcache, tbin, binind,
			    tbin->ncached - tbin->low_water + (tbin->low_water
			    >> 2));
		} else {
			tcache_bin_flush_large(tsd, tbin, binind, tbin->ncached
			    - tbin->low_water + (tbin->low_water >> 2), tcache);
		}
		/*
		 * Reduce fill count by 2X.  Limit lg_fill_div such that the
		 * fill count is always at least 1.
		 */
		if ((tbin_info->ncached_max >> (tbin->lg_fill_div+1)) >= 1) {
			tbin->lg_fill_div++;
		}
	} else if (tbin->low_water < 0) {
		/*
		 * Increase fill count by 2X.  Make sure lg_fill_div stays
		 * greater than 0.
		 */
		if (tbin->lg_fill_div > 1) {
			tbin->lg_fill_div--;
		}
	}
	tbin->low_water = tbin->ncached;

	tcache->next_gc_bin++;
	if (tcache->next_gc_bin == nhbins) {
		tcache->next_gc_bin = 0;
	}
}

void *
tcache_alloc_small_hard(tsdn_t *tsdn, arena_t *arena, tcache_t *tcache,
    tcache_bin_t *tbin, szind_t binind, bool *tcache_success) {
	void *ret;

	arena_tcache_fill_small(tsdn, arena, tbin, binind, config_prof ?
	    tcache->prof_accumbytes : 0);
	if (config_prof) {
		tcache->prof_accumbytes = 0;
	}
	ret = tcache_alloc_easy(tbin, tcache_success);

	return ret;
}

void
tcache_bin_flush_small(tsd_t *tsd, tcache_t *tcache, tcache_bin_t *tbin,
    szind_t binind, unsigned rem) {
	bool merged_stats = false;

	assert(binind < NBINS);
	assert(rem <= tbin->ncached);

	arena_t *arena = tcache->arena;
	assert(arena != NULL);
	unsigned nflush = tbin->ncached - rem;
	VARIABLE_ARRAY(extent_t *, item_extent, nflush);
	/* Look up extent once per item. */
	for (unsigned i = 0 ; i < nflush; i++) {
		item_extent[i] = iealloc(tsd_tsdn(tsd), *(tbin->avail - 1 - i));
	}

	while (nflush > 0) {
		/* Lock the arena bin associated with the first object. */
		extent_t *extent = item_extent[0];
		arena_t *bin_arena = extent_arena_get(extent);
		arena_bin_t *bin = &bin_arena->bins[binind];

		if (config_prof && bin_arena == arena) {
			if (arena_prof_accum(tsd_tsdn(tsd), arena,
			    tcache->prof_accumbytes)) {
				prof_idump(tsd_tsdn(tsd));
			}
			tcache->prof_accumbytes = 0;
		}

		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		if (config_stats && bin_arena == arena) {
			assert(!merged_stats);
			merged_stats = true;
			bin->stats.nflushes++;
			bin->stats.nrequests += tbin->tstats.nrequests;
			tbin->tstats.nrequests = 0;
		}
		unsigned ndeferred = 0;
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = *(tbin->avail - 1 - i);
			extent = item_extent[i];
			assert(ptr != NULL && extent != NULL);

			if (extent_arena_get(extent) == bin_arena) {
				arena_dalloc_bin_junked_locked(tsd_tsdn(tsd),
				    bin_arena, extent, ptr);
			} else {
				/*
				 * This object was allocated via a different
				 * arena bin than the one that is currently
				 * locked.  Stash the object, so that it can be
				 * handled in a future pass.
				 */
				*(tbin->avail - 1 - ndeferred) = ptr;
				item_extent[ndeferred] = extent;
				ndeferred++;
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
		arena_decay_ticks(tsd_tsdn(tsd), bin_arena, nflush - ndeferred);
		nflush = ndeferred;
	}
	if (config_stats && !merged_stats) {
		/*
		 * The flush loop didn't happen to flush to this thread's
		 * arena, so the stats didn't get merged.  Manually do so now.
		 */
		arena_bin_t *bin = &arena->bins[binind];
		malloc_mutex_lock(tsd_tsdn(tsd), &bin->lock);
		bin->stats.nflushes++;
		bin->stats.nrequests += tbin->tstats.nrequests;
		tbin->tstats.nrequests = 0;
		malloc_mutex_unlock(tsd_tsdn(tsd), &bin->lock);
	}

	memmove(tbin->avail - rem, tbin->avail - tbin->ncached, rem *
	    sizeof(void *));
	tbin->ncached = rem;
	if ((int)tbin->ncached < tbin->low_water) {
		tbin->low_water = tbin->ncached;
	}
}

void
tcache_bin_flush_large(tsd_t *tsd, tcache_bin_t *tbin, szind_t binind,
    unsigned rem, tcache_t *tcache) {
	bool merged_stats = false;

	assert(binind < nhbins);
	assert(rem <= tbin->ncached);

	arena_t *arena = tcache->arena;
	assert(arena != NULL);
	unsigned nflush = tbin->ncached - rem;
	VARIABLE_ARRAY(extent_t *, item_extent, nflush);
	/* Look up extent once per item. */
	for (unsigned i = 0 ; i < nflush; i++) {
		item_extent[i] = iealloc(tsd_tsdn(tsd), *(tbin->avail - 1 - i));
	}

	while (nflush > 0) {
		/* Lock the arena associated with the first object. */
		extent_t *extent = item_extent[0];
		arena_t *locked_arena = extent_arena_get(extent);
		UNUSED bool idump;

		if (config_prof) {
			idump = false;
		}

		malloc_mutex_lock(tsd_tsdn(tsd), &locked_arena->large_mtx);
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = *(tbin->avail - 1 - i);
			assert(ptr != NULL);
			extent = iealloc(tsd_tsdn(tsd), ptr);
			if (extent_arena_get(extent) == locked_arena) {
				large_dalloc_prep_junked_locked(tsd_tsdn(tsd),
				    extent);
			}
		}
		if ((config_prof || config_stats) && locked_arena == arena) {
			if (config_prof) {
				idump = arena_prof_accum(tsd_tsdn(tsd), arena,
				    tcache->prof_accumbytes);
				tcache->prof_accumbytes = 0;
			}
			if (config_stats) {
				merged_stats = true;
				arena_stats_large_nrequests_add(tsd_tsdn(tsd),
				    &arena->stats, binind,
				    tbin->tstats.nrequests);
				tbin->tstats.nrequests = 0;
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &locked_arena->large_mtx);

		unsigned ndeferred = 0;
		for (unsigned i = 0; i < nflush; i++) {
			void *ptr = *(tbin->avail - 1 - i);
			extent = item_extent[i];
			assert(ptr != NULL && extent != NULL);

			if (extent_arena_get(extent) == locked_arena) {
				large_dalloc_finish(tsd_tsdn(tsd), extent);
			} else {
				/*
				 * This object was allocated via a different
				 * arena than the one that is currently locked.
				 * Stash the object, so that it can be handled
				 * in a future pass.
				 */
				*(tbin->avail - 1 - ndeferred) = ptr;
				item_extent[ndeferred] = extent;
				ndeferred++;
			}
		}
		if (config_prof && idump) {
			prof_idump(tsd_tsdn(tsd));
		}
		arena_decay_ticks(tsd_tsdn(tsd), locked_arena, nflush -
		    ndeferred);
		nflush = ndeferred;
	}
	if (config_stats && !merged_stats) {
		/*
		 * The flush loop didn't happen to flush to this thread's
		 * arena, so the stats didn't get merged.  Manually do so now.
		 */
		arena_stats_large_nrequests_add(tsd_tsdn(tsd), &arena->stats,
		    binind, tbin->tstats.nrequests);
		tbin->tstats.nrequests = 0;
	}

	memmove(tbin->avail - rem, tbin->avail - tbin->ncached, rem *
	    sizeof(void *));
	tbin->ncached = rem;
	if ((int)tbin->ncached < tbin->low_water) {
		tbin->low_water = tbin->ncached;
	}
}

static void
tcache_arena_associate(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	tcache->arena = arena;
	if (config_stats) {
		/* Link into list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		ql_elm_new(tcache, link);
		ql_tail_insert(&arena->tcache_ql, tcache, link);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

static void
tcache_arena_dissociate(tsdn_t *tsdn, tcache_t *tcache) {
	arena_t *arena = tcache->arena;
	if (config_stats) {
		/* Unlink from list of extant tcaches. */
		malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
		if (config_debug) {
			bool in_ql = false;
			tcache_t *iter;
			ql_foreach(iter, &arena->tcache_ql, link) {
				if (iter == tcache) {
					in_ql = true;
					break;
				}
			}
			assert(in_ql);
		}
		ql_remove(&arena->tcache_ql, tcache, link);
		tcache_stats_merge(tsdn, tcache, arena);
		malloc_mutex_unlock(tsdn, &arena->tcache_ql_mtx);
	}
}

void
tcache_arena_reassociate(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	tcache_arena_dissociate(tsdn, tcache);
	tcache_arena_associate(tsdn, tcache, arena);
}

tcache_t *
tcache_get_hard(tsd_t *tsd) {
	arena_t *arena;

	if (!tcache_enabled_get()) {
		if (tsd_nominal(tsd)) {
			tcache_enabled_set(false); /* Memoize. */
		}
		return NULL;
	}
	arena = arena_choose(tsd, NULL);
	if (unlikely(arena == NULL)) {
		return NULL;
	}
	return tcache_create(tsd_tsdn(tsd), arena);
}

tcache_t *
tcache_create(tsdn_t *tsdn, arena_t *arena) {
	tcache_t *tcache;
	size_t size, stack_offset;
	unsigned i;

	size = offsetof(tcache_t, tbins) + (sizeof(tcache_bin_t) * nhbins);
	/* Naturally align the pointer stacks. */
	size = PTR_CEILING(size);
	stack_offset = size;
	size += stack_nelms * sizeof(void *);
	/* Avoid false cacheline sharing. */
	size = sa2u(size, CACHELINE);

	tcache = ipallocztm(tsdn, size, CACHELINE, true, NULL, true,
	    arena_get(TSDN_NULL, 0, true));
	if (tcache == NULL) {
		return NULL;
	}

	tcache_arena_associate(tsdn, tcache, arena);

	ticker_init(&tcache->gc_ticker, TCACHE_GC_INCR);

	assert((TCACHE_NSLOTS_SMALL_MAX & 1U) == 0);
	for (i = 0; i < nhbins; i++) {
		tcache->tbins[i].lg_fill_div = 1;
		stack_offset += tcache_bin_info[i].ncached_max * sizeof(void *);
		/*
		 * avail points past the available space.  Allocations will
		 * access the slots toward higher addresses (for the benefit of
		 * prefetch).
		 */
		tcache->tbins[i].avail = (void **)((uintptr_t)tcache +
		    (uintptr_t)stack_offset);
	}

	return tcache;
}

static void
tcache_destroy(tsd_t *tsd, tcache_t *tcache) {
	unsigned i;

	for (i = 0; i < NBINS; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_small(tsd, tcache, tbin, i, 0);

		if (config_stats) {
			assert(tbin->tstats.nrequests == 0);
		}
	}

	for (; i < nhbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		tcache_bin_flush_large(tsd, tbin, i, 0, tcache);

		if (config_stats) {
			assert(tbin->tstats.nrequests == 0);
		}
	}

	/*
	 * Get arena after flushing -- when using percpu arena, the associated
	 * arena could change during flush.
	 */
	arena_t *arena = arena_choose(tsd, NULL);
	tcache_arena_dissociate(tsd_tsdn(tsd), tcache);

	if (config_prof && tcache->prof_accumbytes > 0 &&
	    arena_prof_accum(tsd_tsdn(tsd), arena, tcache->prof_accumbytes)) {
		prof_idump(tsd_tsdn(tsd));
	}

	idalloctm(tsd_tsdn(tsd), tcache, NULL, true, true);
}

void
tcache_cleanup(tsd_t *tsd) {
	tcache_t *tcache;

	if (!config_tcache) {
		return;
	}

	if ((tcache = tsd_tcache_get(tsd)) != NULL) {
		tcache_destroy(tsd, tcache);
		tsd_tcache_set(tsd, NULL);
	}
}

void
tcache_stats_merge(tsdn_t *tsdn, tcache_t *tcache, arena_t *arena) {
	unsigned i;

	cassert(config_stats);

	/* Merge and reset tcache stats. */
	for (i = 0; i < NBINS; i++) {
		arena_bin_t *bin = &arena->bins[i];
		tcache_bin_t *tbin = &tcache->tbins[i];
		malloc_mutex_lock(tsdn, &bin->lock);
		bin->stats.nrequests += tbin->tstats.nrequests;
		malloc_mutex_unlock(tsdn, &bin->lock);
		tbin->tstats.nrequests = 0;
	}

	for (; i < nhbins; i++) {
		tcache_bin_t *tbin = &tcache->tbins[i];
		arena_stats_large_nrequests_add(tsdn, &arena->stats, i,
		    tbin->tstats.nrequests);
		tbin->tstats.nrequests = 0;
	}
}

static bool
tcaches_create_prep(tsd_t *tsd) {
	bool err;

	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);

	if (tcaches == NULL) {
		tcaches = base_alloc(tsd_tsdn(tsd), b0get(), sizeof(tcache_t *)
		    * (MALLOCX_TCACHE_MAX+1), CACHELINE);
		if (tcaches == NULL) {
			err = true;
			goto label_return;
		}
	}

	if (tcaches_avail == NULL && tcaches_past > MALLOCX_TCACHE_MAX) {
		err = true;
		goto label_return;
	}

	err = false;
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	return err;
}

bool
tcaches_create(tsd_t *tsd, unsigned *r_ind) {
	witness_assert_depth(tsd_tsdn(tsd), 0);

	bool err;

	if (tcaches_create_prep(tsd)) {
		err = true;
		goto label_return;
	}

	arena_t *arena = arena_ichoose(tsd, NULL);
	if (unlikely(arena == NULL)) {
		err = true;
		goto label_return;
	}
	tcache_t *tcache = tcache_create(tsd_tsdn(tsd), arena);
	if (tcache == NULL) {
		err = true;
		goto label_return;
	}

	tcaches_t *elm;
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcaches_avail != NULL) {
		elm = tcaches_avail;
		tcaches_avail = tcaches_avail->next;
		elm->tcache = tcache;
		*r_ind = (unsigned)(elm - tcaches);
	} else {
		elm = &tcaches[tcaches_past];
		elm->tcache = tcache;
		*r_ind = tcaches_past;
		tcaches_past++;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);

	err = false;
label_return:
	witness_assert_depth(tsd_tsdn(tsd), 0);
	return err;
}

static tcache_t *
tcaches_elm_remove(tsd_t *tsd, tcaches_t *elm) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &tcaches_mtx);

	if (elm->tcache == NULL) {
		return NULL;
	}
	tcache_t *tcache = elm->tcache;
	elm->tcache = NULL;
	return tcache;
}

void
tcaches_flush(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcache_t *tcache = tcaches_elm_remove(tsd, &tcaches[ind]);
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache);
	}
}

void
tcaches_destroy(tsd_t *tsd, unsigned ind) {
	malloc_mutex_lock(tsd_tsdn(tsd), &tcaches_mtx);
	tcaches_t *elm = &tcaches[ind];
	tcache_t *tcache = tcaches_elm_remove(tsd, elm);
	elm->next = tcaches_avail;
	tcaches_avail = elm;
	malloc_mutex_unlock(tsd_tsdn(tsd), &tcaches_mtx);
	if (tcache != NULL) {
		tcache_destroy(tsd, tcache);
	}
}

bool
tcache_boot(tsdn_t *tsdn) {
	cassert(config_tcache);

	unsigned i;

	/* If necessary, clamp opt_lg_tcache_max. */
	if (opt_lg_tcache_max < 0 || (ZU(1) << opt_lg_tcache_max) <
	    SMALL_MAXCLASS) {
		tcache_maxclass = SMALL_MAXCLASS;
	} else {
		tcache_maxclass = (ZU(1) << opt_lg_tcache_max);
	}

	if (malloc_mutex_init(&tcaches_mtx, "tcaches", WITNESS_RANK_TCACHES)) {
		return true;
	}

	nhbins = size2index(tcache_maxclass) + 1;

	/* Initialize tcache_bin_info. */
	tcache_bin_info = (tcache_bin_info_t *)base_alloc(tsdn, b0get(), nhbins
	    * sizeof(tcache_bin_info_t), CACHELINE);
	if (tcache_bin_info == NULL) {
		return true;
	}
	stack_nelms = 0;
	for (i = 0; i < NBINS; i++) {
		if ((arena_bin_info[i].nregs << 1) <= TCACHE_NSLOTS_SMALL_MIN) {
			tcache_bin_info[i].ncached_max =
			    TCACHE_NSLOTS_SMALL_MIN;
		} else if ((arena_bin_info[i].nregs << 1) <=
		    TCACHE_NSLOTS_SMALL_MAX) {
			tcache_bin_info[i].ncached_max =
			    (arena_bin_info[i].nregs << 1);
		} else {
			tcache_bin_info[i].ncached_max =
			    TCACHE_NSLOTS_SMALL_MAX;
		}
		stack_nelms += tcache_bin_info[i].ncached_max;
	}
	for (; i < nhbins; i++) {
		tcache_bin_info[i].ncached_max = TCACHE_NSLOTS_LARGE;
		stack_nelms += tcache_bin_info[i].ncached_max;
	}

	return false;
}

void
tcache_prefork(tsdn_t *tsdn) {
	if (!config_prof && opt_tcache) {
		malloc_mutex_prefork(tsdn, &tcaches_mtx);
	}
}

void
tcache_postfork_parent(tsdn_t *tsdn) {
	if (!config_prof && opt_tcache) {
		malloc_mutex_postfork_parent(tsdn, &tcaches_mtx);
	}
}

void
tcache_postfork_child(tsdn_t *tsdn) {
	if (!config_prof && opt_tcache) {
		malloc_mutex_postfork_child(tsdn, &tcaches_mtx);
	}
}
