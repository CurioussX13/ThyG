/*
 * drivers/staging/android/ion/ion_page_pool.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/mmzone.h>
#include "ion_priv.h"

static void *ion_page_pool_alloc_pages(struct ion_page_pool *pool)
{
	struct page *page;

	page = alloc_pages(pool->gfp_mask & ~__GFP_ZERO, pool->order);

	if (!page)
		return NULL;

	if (pool->gfp_mask & __GFP_ZERO)
		if (msm_ion_heap_high_order_page_zero(pool->dev, page,
						      pool->order))
			goto error_free_pages;

	ion_page_pool_alloc_set_cache_policy(pool, page);

	return page;
error_free_pages:
	__free_pages(page, pool->order);
	return NULL;
}

static void ion_page_pool_free_pages(struct ion_page_pool *pool,
				     struct page *page)
{
	ion_page_pool_free_set_cache_policy(pool, page);
	__free_pages(page, pool->order);
}

static int ion_page_pool_add(struct ion_page_pool *pool, struct page *page,
				bool prefetch)
{
	int page_count = 1 << pool->order;

	spin_lock(&pool->lock);
	if (PageHighMem(page)) {
		list_add_tail(&page->lru, &pool->high_items);
		pool->high_count++;
	} else {
		list_add_tail(&page->lru, &pool->low_items);
		pool->low_count++;
	}
	if (!prefetch)
		pool->nr_unreserved++;

	mod_zone_page_state(page_zone(page), NR_FILE_PAGES, page_count);
	mod_zone_page_state(page_zone(page), NR_INACTIVE_FILE, page_count);

	spin_unlock(&pool->lock);
	return 0;
}

static struct page *ion_page_pool_remove(struct ion_page_pool *pool, bool high,
					bool prefetch)
{
	struct page *page;
	int page_count = 1 << pool->order;

	if (high) {
		BUG_ON(!pool->high_count);
		page = list_first_entry(&pool->high_items, struct page, lru);
		pool->high_count--;
	} else {
		BUG_ON(!pool->low_count);
		page = list_first_entry(&pool->low_items, struct page, lru);
		pool->low_count--;
	}

	if (prefetch) {
		BUG_ON(!pool->nr_unreserved);
		pool->nr_unreserved--;
	}
	pool->nr_unreserved = min_t(int, pool->high_count + pool->low_count,
						pool->nr_unreserved);

	list_del(&page->lru);

	mod_zone_page_state(page_zone(page), NR_INACTIVE_FILE, -page_count);
	mod_zone_page_state(page_zone(page), NR_FILE_PAGES, -page_count);

	return page;
}

void *ion_page_pool_alloc(struct ion_page_pool *pool, bool *from_pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	*from_pool = true;

	if (spin_trylock(&pool->lock)) {
		if (pool->high_count)
			page = ion_page_pool_remove(pool, true, false);
		else if (pool->low_count)
			page = ion_page_pool_remove(pool, false, false);
		spin_unlock(&pool->lock);
	}
	if (!page) {
		page = ion_page_pool_alloc_pages(pool);
		*from_pool = false;
	}
	return page;
}

void *ion_page_pool_prefetch(struct ion_page_pool *pool, bool *from_pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	*from_pool = true;

	if (spin_trylock(&pool->lock)) {
		if (pool->high_count && pool->nr_unreserved > 0)
			page = ion_page_pool_remove(pool, true, true);
		else if (pool->low_count && pool->nr_unreserved > 0)
			page = ion_page_pool_remove(pool, false, true);
		spin_unlock(&pool->lock);
	}
	if (!page) {
		page = ion_page_pool_alloc_pages(pool);
		*from_pool = false;
	}
	return page;
}
/*
 * Tries to allocate from only the specified Pool and returns NULL otherwise
 */
void *ion_page_pool_alloc_pool_only(struct ion_page_pool *pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	if (spin_trylock(&pool->lock)) {
		if (pool->high_count)
			page = ion_page_pool_remove(pool, true, false);
		else if (pool->low_count)
			page = ion_page_pool_remove(pool, false, false);
		spin_unlock(&pool->lock);
	}

	return page;
}

void ion_page_pool_free(struct ion_page_pool *pool, struct page *page,
			bool prefetch)
{
	int ret;

	BUG_ON(pool->order != compound_order(page));

	ret = ion_page_pool_add(pool, page, prefetch);
	/* FIXME? For a secure page, not hyp unassigned in this err path */
	if (ret)
		ion_page_pool_free_pages(pool, page);
}

void ion_page_pool_free_immediate(struct ion_page_pool *pool, struct page *page)
{
	ion_page_pool_free_pages(pool, page);
}

int ion_page_pool_total(struct ion_page_pool *pool, bool high)
{
	int count = pool->low_count;

	if (high)
		count += pool->high_count;

	return count << pool->order;
}

int ion_page_pool_shrink(struct ion_page_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	int freed = 0;
	bool high;

	if (current_is_kswapd())
		high = true;
	else
		high = !!(gfp_mask & __GFP_HIGHMEM);

	if (nr_to_scan == 0)
		return ion_page_pool_total(pool, high);

	while (freed < nr_to_scan) {
		struct page *page;

		spin_lock(&pool->lock);
		if (pool->low_count) {
			page = ion_page_pool_remove(pool, false, false);
		} else if (high && pool->high_count) {
			page = ion_page_pool_remove(pool, true, false);
		} else {
			spin_unlock(&pool->lock);
			break;
		}
		spin_unlock(&pool->lock);
		ion_page_pool_free_pages(pool, page);
		freed += (1 << pool->order);
	}

	return freed;
}

struct ion_page_pool *ion_page_pool_create(struct device *dev, gfp_t gfp_mask,
					   unsigned int order)
{
	struct ion_page_pool *pool = kmalloc(sizeof(struct ion_page_pool),
					     GFP_KERNEL);
	if (!pool)
		return NULL;
	pool->dev = dev;
	pool->high_count = 0;
	pool->low_count = 0;
	pool->nr_unreserved = 0;
	INIT_LIST_HEAD(&pool->low_items);
	INIT_LIST_HEAD(&pool->high_items);
	pool->gfp_mask = gfp_mask | __GFP_COMP;
	pool->order = order;
	spin_lock_init(&pool->lock);
	plist_node_init(&pool->list, order);

	return pool;
}

void ion_page_pool_destroy(struct ion_page_pool *pool)
{
	kfree(pool);
}

static int __init ion_page_pool_init(void)
{
	return 0;
}

static void __exit ion_page_pool_exit(void)
{
}

module_init(ion_page_pool_init);
module_exit(ion_page_pool_exit);
