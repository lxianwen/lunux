#include <linux/syscalls.h>
#include <linux/migrate.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/hugetlb.h>
#include <linux/mm_inline.h>
#include <linux/page_idle.h>
#include <linux/page-flags.h>
#include <linux/ksm.h>
#include <linux/memcontrol.h>
#include <linux/balloon_compaction.h>
#include <linux/buffer_head.h>
#include <linux/fs.h> /* buffer_migrate_page  */
#include <linux/backing-dev.h>
#include <linux/sched/mm.h>

#include <linux/page_hotness/candidate_page.h>
#include <linux/page_hotness/page_exchange.h>
#include <linux/page_common/page_hotness_stat.h>
#include "../internal.h"

#ifdef CONFIG_PAGE_EXCHANGE

struct page_flags {
	unsigned int page_error :1;
	unsigned int page_referenced:1;
	unsigned int page_uptodate:1;
	unsigned int page_active:1;
	unsigned int page_workingset:1; // nimble中没有这个flag
	unsigned int page_unevictable:1;
	unsigned int page_checked:1;
	unsigned int page_mappedtodisk:1;
	unsigned int page_dirty:1;
	unsigned int page_is_young:1;
	unsigned int page_is_idle:1;
	unsigned int page_swapcache:1;
	unsigned int page_writeback:1;
	unsigned int page_private:1;
	unsigned int page_doublemap:1;
	unsigned int __pad:2;
};


static bool can_be_exchanged(struct page *from, struct page *to)
{
	if (PageCompound(from) != PageCompound(to))
		return false;
	
	if (PageHuge(from) != PageHuge(to))
		return false;

	if (PageHuge(from) || PageHuge(to))
		return false;

	if (compound_order(from) != compound_order(to))
		return false;

	return true;
}

static void exchange_page(char *to, char *from)
{
	u64 tmp;
	int i;

	for (i = 0; i < PAGE_SIZE; i += sizeof(tmp)) {
		tmp = *((u64*)(from + i));
		*((u64*)(from + i)) = *((u64*)(to + i));
		*((u64*)(to + i)) = tmp;
	}
}

static inline void exchange_highpage(struct page *to, struct page *from)
{
	char *vfrom, *vto;

	vfrom = kmap_atomic(from);
	vto = kmap_atomic(to);
	exchange_page(vto, vfrom);
	kunmap_atomic(vto);
	kunmap_atomic(vfrom);
}

static void __exchange_gigantic_page(struct page *dst, struct page *src,
				int nr_pages)
{
	int i;
	struct page *dst_base = dst;
	struct page *src_base = src;

	for (i = 0; i < nr_pages; ) {
		cond_resched();
		exchange_highpage(dst, src);

		i++;
		dst = mem_map_next(dst, dst_base, i);
		src = mem_map_next(src, src_base, i);
	}
}

static void exchange_huge_page(struct page *dst, struct page *src)
{
	int i;
	int nr_pages;

	if (PageHuge(src)) {
		/* hugetlbfs page */
		struct hstate *h = page_hstate(src);
		nr_pages = pages_per_huge_page(h);

		if (unlikely(nr_pages > MAX_ORDER_NR_PAGES)) {
			__exchange_gigantic_page(dst, src, nr_pages);
			return;
		}
	} else {
		/* thp page */
		BUG_ON(!PageTransHuge(src));
		// lmy: 没有hpage_nr_pages，我替换成了thp_nr_pages，不清楚对不对
		nr_pages = thp_nr_pages(src);
	}

	for (i = 0; i < nr_pages; i++) {
		exchange_highpage(dst + i, src + i);
	}
}


/*
 * Copy the page to its new location without polluting cache
 */
static void exchange_page_flags(struct page *to_page, struct page *from_page)
{
	int from_cpupid, to_cpupid;
	struct page_flags from_page_flags = {0}, to_page_flags = {0};
	struct mem_cgroup *to_memcg = page_memcg(to_page),
					  *from_memcg = page_memcg(from_page);

	from_cpupid = page_cpupid_xchg_last(from_page, -1);

	// PageError
	from_page_flags.page_error = PageError(from_page);
	if (from_page_flags.page_error)
		ClearPageError(from_page);

	// PageReferenced
	from_page_flags.page_referenced = TestClearPageReferenced(from_page);

	// PageUptodate
	from_page_flags.page_uptodate = PageUptodate(from_page);
	ClearPageUptodate(from_page);

	// TestClearPageActive
	from_page_flags.page_active = TestClearPageActive(from_page);

	// TestClearPageUnevictable
	from_page_flags.page_unevictable = TestClearPageUnevictable(from_page);

	// nimble中没有这个flag，但是5.15.114有这个flag
	// PageWorkingset
	// from_page_flags.page_workingset = PageWorkingset(from_page);

	// PageChecked
	from_page_flags.page_checked = PageChecked(from_page);
	if (from_page_flags.page_checked)
		ClearPageChecked(from_page);

	// PageMappedToDisk
	from_page_flags.page_mappedtodisk = PageMappedToDisk(from_page);
	ClearPageMappedToDisk(from_page);

	// PageDirty
	from_page_flags.page_dirty = PageDirty(from_page);
	ClearPageDirty(from_page);

	// page_is_young
	from_page_flags.page_is_young = test_and_clear_page_young(from_page);

	// page_is_idle
	from_page_flags.page_is_idle = page_is_idle(from_page);
	clear_page_idle(from_page);

	// PageSwapCache
	from_page_flags.page_swapcache = PageSwapCache(from_page);

	/*from_page_flags.page_private = PagePrivate(from_page);*/
	/*ClearPagePrivate(from_page);*/

	// PageWriteback
	from_page_flags.page_writeback = test_clear_page_writeback(from_page);

	// migrate_page_states中没有这个flag的代码，但是nimble中添加了
	from_page_flags.page_doublemap = PageDoubleMap(from_page); // 这个是什么？

	// cpupid
	to_cpupid = page_cpupid_xchg_last(to_page, -1);

	to_page_flags.page_error = PageError(to_page);
	if (to_page_flags.page_error)
		ClearPageError(to_page);
	to_page_flags.page_referenced = TestClearPageReferenced(to_page);
	to_page_flags.page_uptodate = PageUptodate(to_page);
	ClearPageUptodate(to_page);
	to_page_flags.page_active = TestClearPageActive(to_page);
	to_page_flags.page_unevictable = TestClearPageUnevictable(to_page);
	// PageWorkingset
	// to_page_flags.page_workingset = PageWorkingset(to_page);
	to_page_flags.page_checked = PageChecked(to_page);
	if (to_page_flags.page_checked)
		ClearPageChecked(to_page);
	to_page_flags.page_mappedtodisk = PageMappedToDisk(to_page);
	ClearPageMappedToDisk(to_page);
	to_page_flags.page_dirty = PageDirty(to_page);
	ClearPageDirty(to_page);
	to_page_flags.page_is_young = test_and_clear_page_young(to_page);
	to_page_flags.page_is_idle = page_is_idle(to_page);
	clear_page_idle(to_page);
	to_page_flags.page_swapcache = PageSwapCache(to_page);
	/*to_page_flags.page_private = PagePrivate(to_page);*/
	/*ClearPagePrivate(to_page);*/
	to_page_flags.page_writeback = test_clear_page_writeback(to_page);
	to_page_flags.page_doublemap = PageDoubleMap(to_page);

	/* set to_page */
	if (from_page_flags.page_error)
		SetPageError(to_page);
	if (from_page_flags.page_referenced)
		SetPageReferenced(to_page);
	if (from_page_flags.page_uptodate)
		SetPageUptodate(to_page);
	if (from_page_flags.page_active) {
		VM_BUG_ON_PAGE(from_page_flags.page_unevictable, from_page);
		SetPageActive(to_page);
	} else if (from_page_flags.page_unevictable)
		SetPageUnevictable(to_page);
	// workingset
#if 0
	if (from_page_flags.page_workingset)
		SetPageWorkingset(to_page);
	else
		ClearPageWorkingset(to_page);
#else
	// ClearPageWorkingset(from_page);
#endif
	if (from_page_flags.page_checked)
		SetPageChecked(to_page);
	if (from_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(to_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (from_page_flags.page_dirty)
		SetPageDirty(to_page);

	if (from_page_flags.page_is_young)
		set_page_young(to_page);
	if (from_page_flags.page_is_idle)
		set_page_idle(to_page);
	if (from_page_flags.page_doublemap)
		SetPageDoubleMap(to_page);

	/* set from_page */
	if (to_page_flags.page_error)
		SetPageError(from_page);
	if (to_page_flags.page_referenced)
		SetPageReferenced(from_page);
	if (to_page_flags.page_uptodate)
		SetPageUptodate(from_page);
	if (to_page_flags.page_active) {
		VM_BUG_ON_PAGE(to_page_flags.page_unevictable, from_page);
		SetPageActive(from_page);
	} else if (to_page_flags.page_unevictable)
		SetPageUnevictable(from_page);
	if (to_page_flags.page_checked)
		SetPageChecked(from_page);
#if 0
	if (to_page_flags.page_workingset)
		SetPageWorkingset(from_page);
	else
		ClearPageWorkingset(from_page);
#else
	// ClearPageWorkingset(from_page);
#endif
	if (to_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(from_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (to_page_flags.page_dirty)
		SetPageDirty(from_page);

	if (to_page_flags.page_is_young)
		set_page_young(from_page);
	if (to_page_flags.page_is_idle)
		set_page_idle(from_page);
	if (to_page_flags.page_doublemap)
		SetPageDoubleMap(from_page);

	/*
	 * Copy NUMA information to the new page, to prevent over-eager
	 * future migrations of this same page.
	 */
	page_cpupid_xchg_last(to_page, from_cpupid);
	page_cpupid_xchg_last(from_page, to_cpupid);

	ksm_exchange_page(to_page, from_page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(to_page);
	ClearPageSwapCache(from_page);
	if (from_page_flags.page_swapcache)
		SetPageSwapCache(to_page);
	if (to_page_flags.page_swapcache)
		SetPageSwapCache(from_page);


#ifdef CONFIG_PAGE_OWNER
	/* exchange page owner  */
	BUG();
#endif

#ifdef CONFIG_MEMCG
	// 不清楚这里有没有问题
	/* exchange mem cgroup  */
	to_page->memcg_data = (unsigned long)from_memcg;
	from_page->memcg_data = (unsigned long)to_memcg;
#endif

}

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */
// migrate_page_move_mapping
static int exchange_page_move_mapping(struct address_space *to_mapping,
			struct address_space *from_mapping,
			struct page *to_page, struct page *from_page,
			struct buffer_head *to_head, struct buffer_head *from_head,
			enum migrate_mode mode,
			int to_extra_count, int from_extra_count)
{
	int from_expected_count = 1 + from_extra_count;
	int to_expected_count = 1 + to_extra_count;
	unsigned long from_page_index = from_page->index;
	unsigned long to_page_index = to_page->index;
	int from_swapbacked = PageSwapBacked(from_page);
	int to_swapbacked = PageSwapBacked(to_page);
	struct address_space *from_mapping_value = from_page->mapping;
	struct address_space *to_mapping_value = to_page->mapping;

	VM_BUG_ON_PAGE(to_mapping != page_mapping(to_page), to_page);
	VM_BUG_ON_PAGE(from_mapping != page_mapping(from_page), from_page);
	VM_BUG_ON(PageCompound(from_page) != PageCompound(to_page));

	if (!to_mapping) {
		/* Anonymous page without mapping */
		if (page_count(to_page) != to_expected_count)
			return -EAGAIN;
	}

	if (!from_mapping) {
		/* Anonymous page without mapping */
		if (page_count(from_page) != from_expected_count)
			return -EAGAIN;
	}

	if (!from_mapping && !to_mapping) {
		// debug: 无bug
		// return -EAGAIN;

		/* 情况1 */
		/* both are anonymous pages without mapping */
		/* from_page  */
		from_page->index = to_page_index;
		from_page->mapping = to_mapping_value;
		if (to_swapbacked)
			SetPageSwapBacked(from_page);
		else
			ClearPageSwapBacked(from_page);

		/* to_page  */
		to_page->index = from_page_index;
		to_page->mapping = from_mapping_value;
		if (from_swapbacked)
			SetPageSwapBacked(to_page);
		else
			ClearPageSwapBacked(to_page);

		// nimble没有WorkingSet相关代码
		// ClearPageWorkingset(from_page);
		// ClearPageWorkingset(to_page);
	} else if (!from_mapping && to_mapping) {
		/* 情况2 */
		/* from is anonymous, to is file-backed  */
		struct zone *from_zone, *to_zone;
		void **to_pslot;
		int dirty;

		printk("exchange_page_move_mapping: skip !from_mapping && to_mapping");

		from_zone = page_zone(from_page);
		to_zone = page_zone(to_page);

		spin_lock_irq(&to_mapping->i_pages.xa_lock);

		to_pslot = radix_tree_lookup_slot(&to_mapping->i_pages, page_index(to_page));

		to_expected_count += 1 + page_has_private(to_page);
		if (page_count(to_page) != to_expected_count ||
			radix_tree_deref_slot_protected(to_pslot, &to_mapping->i_pages.xa_lock)
			!= to_page) {
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);
			return -EAGAIN;
		}

		if (!page_ref_freeze(to_page, to_expected_count)) {
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);
			pr_debug("cannot freeze page count\n");
			return -EAGAIN;
		}

		if (((mode & MIGRATETYPE_MASK) == MIGRATE_ASYNC) && to_head &&
				!buffer_migrate_lock_buffers(to_head, mode)) {
			page_ref_unfreeze(to_page, to_expected_count);
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);

			pr_debug("cannot lock buffer head\n");
			return -EAGAIN;
		}

		/*
		 * Now we know that no one else is looking at the page:
		 * no turning back from here.
		 */
		ClearPageSwapBacked(from_page);
		ClearPageSwapBacked(to_page);

		/* from_page  */
		from_page->index = to_page_index;
		from_page->mapping = to_mapping_value;
		/* to_page  */
		to_page->index = from_page_index;
		to_page->mapping = from_mapping_value;

		get_page(from_page); /* add cache reference  */
		if (to_swapbacked)
			__SetPageSwapBacked(from_page);
		else
			VM_BUG_ON_PAGE(PageSwapCache(to_page), to_page);

		if (from_swapbacked)
			__SetPageSwapBacked(to_page);
		else
			VM_BUG_ON_PAGE(PageSwapCache(from_page), from_page);

		dirty = PageDirty(to_page);

		radix_tree_replace_slot(&to_mapping->i_pages, to_pslot, from_page);

		/* drop cache reference */
		page_ref_unfreeze(to_page, to_expected_count - 1);

		spin_unlock(&to_mapping->i_pages.xa_lock);

		/*
		 * If moved to a different zone then also account
		 * the page for that zone. Other VM counters will be
		 * taken care of when we establish references to the
		 * new page and drop references to the old page.
		 *
		 * Note that anonymous pages are accounted for
		 * via NR_FILE_PAGES and NR_ANON_MAPPED if they
		 * are mapped to swap space.
		 */
		if (to_zone != from_zone) {
			__dec_node_state(to_zone->zone_pgdat, NR_FILE_PAGES);
			__inc_node_state(from_zone->zone_pgdat, NR_FILE_PAGES);
			if (PageSwapBacked(to_page) && !PageSwapCache(to_page)) {
				__dec_node_state(to_zone->zone_pgdat, NR_SHMEM);
				__inc_node_state(from_zone->zone_pgdat, NR_SHMEM);
			}
			// nimble中是mapping_cap_account_dirty，5.15.114应该是mapping_can_writeback
			if (dirty && mapping_can_writeback(to_mapping)) {
				__dec_node_state(to_zone->zone_pgdat, NR_FILE_DIRTY);
				__dec_zone_state(to_zone, NR_ZONE_WRITE_PENDING);
				__inc_node_state(from_zone->zone_pgdat, NR_FILE_DIRTY);
				__inc_zone_state(from_zone, NR_ZONE_WRITE_PENDING);
			}
		}
		local_irq_enable();

	} else {
		/* from is file-backed to is anonymous: fold this to the case above */
		/* both are file-backed  */
		BUG();
	}

	return MIGRATEPAGE_SUCCESS;
}

// move_to_new_page
static int exchange_to_new_page(struct page *to_page, struct page *from_page,
				enum migrate_mode mode)
{
	int rc = -EBUSY;
	struct address_space *to_page_mapping, *from_page_mapping;
	struct buffer_head *to_head = NULL, *to_bh = NULL;

	VM_BUG_ON_PAGE(!PageLocked(from_page), from_page);
	VM_BUG_ON_PAGE(!PageLocked(to_page), to_page);

	/* copy page->mapping not use page_mapping()  */
	to_page_mapping = page_mapping(to_page);
	from_page_mapping = page_mapping(from_page);

	// 为什么必须是匿名页？？
	/* from_page has to be anonymous page  */
	BUG_ON(from_page_mapping);
	BUG_ON(PageWriteback(from_page));
	/* writeback has to finish */
	BUG_ON(PageWriteback(to_page));

	/* to_page is anonymous  */
	if (!to_page_mapping) {
exchange_mappings:
		/* actual page mapping exchange */
		rc = exchange_page_move_mapping(to_page_mapping, from_page_mapping,
							to_page, from_page, NULL, NULL, mode, 0, 0);
	} else {
		if (to_page_mapping->a_ops->migratepage == buffer_migrate_page) {
			// pr_dump_page(to_page, "exchange has migratepage: to ");

			if (!page_has_buffers(to_page))
				goto exchange_mappings;

			to_head = page_buffers(to_page);

			rc = exchange_page_move_mapping(to_page_mapping,
					from_page_mapping, to_page, from_page,
					to_head, NULL, mode, 0, 0);

			if (rc != MIGRATEPAGE_SUCCESS)
				return rc;

			/*
			 * In the async case, migrate_page_move_mapping locked the buffers
			 * with an IRQ-safe spinlock held. In the sync case, the buffers
			 * need to be locked now
			 */
			if (mode != MIGRATE_ASYNC)
				BUG_ON(!buffer_migrate_lock_buffers(to_head, mode));

			ClearPagePrivate(to_page);
			set_page_private(from_page, page_private(to_page));
			set_page_private(to_page, 0);
			/* transfer private page count  */
			put_page(to_page);
			get_page(from_page);

			to_bh = to_head;
			do {
				set_bh_page(to_bh, from_page, bh_offset(to_bh));
				to_bh = to_bh->b_this_page;

			} while (to_bh != to_head);

			SetPagePrivate(from_page);

			to_bh = to_head;
		} else if (!to_page_mapping->a_ops->migratepage) {
			/* fallback_migrate_page  */
			// pr_dump_page(to_page, "exchange no migratepage: to ");

			if (PageDirty(to_page)) {
				if (mode != MIGRATE_SYNC)
					return -EBUSY;
				return writeout(to_page_mapping, to_page);
			}
			if (page_has_private(to_page) &&
				!try_to_release_page(to_page, GFP_KERNEL))
				return -EAGAIN;

			goto exchange_mappings;
		}
	}

debug:
	/* actual page data exchange  */
	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	rc = -EFAULT;

	// if (mode & MIGRATE_MT)
	// 	rc = exchange_page_mthread(to_page, from_page,
	// 			thp_nr_pages(from_page));
	if (rc) {
		if (PageHuge(from_page) || PageTransHuge(from_page)) {
			// printk("exchange_to_new_page PageHuge(from_page)");
			WARN_ON(1);
			// exchange_huge_page(to_page, from_page);
		} else
			exchange_highpage(to_page, from_page);
		rc = 0;
	}

	/*
	 * 1. buffer_migrate_page:
	 *   private flag should be transferred from to_page to from_page
	 *
	 * 2. anon<->anon, fallback_migrate_page:
	 *   both have none private flags or to_page's is cleared.
	 * */
	VM_BUG_ON(!((page_has_private(from_page) && !page_has_private(to_page)) ||
				(!page_has_private(from_page) && !page_has_private(to_page))));

	exchange_page_flags(to_page, from_page);
	// 去掉exchange_page_info就没有死锁了
	exchange_page_info(from_page, to_page);

	// pr_dump_page(from_page, "after exchange: from ");
	// pr_dump_page(to_page, "after exchange: to ");

	if (to_bh) {
		VM_BUG_ON(to_bh != to_head);
		do {
			unlock_buffer(to_bh);
			put_bh(to_bh);
			to_bh = to_bh->b_this_page;

		} while (to_bh != to_head);
	}

	return rc;
}

static int unmap_and_exchange(struct page *from_page,
		struct page *to_page, int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	struct anon_vma *from_anon_vma = NULL;
	struct anon_vma *to_anon_vma = NULL;
	/*bool is_from_lru = !__PageMovable(from_page);*/
	/*bool is_to_lru = !__PageMovable(to_page);*/
	int from_page_was_mapped = 0;
	int to_page_was_mapped = 0;
	int from_page_count = 0, to_page_count = 0;
	int from_map_count = 0, to_map_count = 0;
	unsigned long from_flags, to_flags;
	pgoff_t from_index, to_index;
	struct address_space *from_mapping, *to_mapping;

	// printk("try lock page, from_page=%px, to_page=%px\n", from_page, to_page);
	if (!trylock_page(from_page)) {
		if (!force || mode == MIGRATE_ASYNC) {
			goto out;
		}

		lock_page(from_page);
	}

	if (!trylock_page(to_page)) {
		if (mode == MIGRATE_ASYNC) {
			// nimble与autotiering没有解锁from_page，应该是有bug的吧
			// 原版直接迁移，不需要两个页面都加锁，所以没问题
			// nimble与autotiering之所以没出bug，应该是因为只使用了MIGRATE_SYNC模式
			unlock_page(from_page);
			goto out;
		}
		lock_page(to_page);
	}

	// 为什么nimble中假设from page为匿名页
	/* from_page is supposed to be an anonymous page */
	VM_BUG_ON_PAGE(PageWriteback(from_page), from_page);

	if (PageWriteback(to_page)) {
		// printk("PageWriteback(to_page), to_page=%px\n", to_page);
		/* 
		 * 参考__unmap_and_move注释
		 */
		switch (mode) {
		case MIGRATE_SYNC:
		case MIGRATE_SYNC_NO_COPY:
			break;
		default:
			rc = -EBUSY;
			goto out_unlock;
		}
		if (!force)
			goto out_unlock;
		wait_on_page_writeback(to_page);
	}

	/* 
	* 参考__unmap_and_move注释
	*/
	// printk("record anon_vma\n");
	if (PageAnon(from_page) && !PageKsm(from_page))
		from_anon_vma = page_get_anon_vma(from_page);

	if (PageAnon(to_page) && !PageKsm(to_page))
		to_anon_vma = page_get_anon_vma(to_page);

	from_page_count = page_count(from_page);
	from_map_count = page_mapcount(from_page);
	to_page_count = page_count(to_page);
	to_map_count = page_mapcount(to_page);
	from_flags = from_page->flags;
	to_flags = to_page->flags;
	from_mapping = from_page->mapping;
	to_mapping = to_page->mapping;
	from_index = from_page->index;
	to_index = to_page->index;

	/* 
	* 参考__unmap_and_move注释
	*/
	// printk("check from_page->mapping\n");
	if (!from_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(from_page), from_page);
		if (page_has_private(from_page)) {
			try_to_free_buffers(from_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(from_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(from_page) && !PageKsm(from_page) &&
					   !from_anon_vma, from_page);
		// 此处nimble使用的是try_to_unmap，但是5.15.114使用的是try_to_migrate
		try_to_migrate(from_page, 0);
		from_page_was_mapped = 1;
	}

	if (!to_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(to_page), to_page);
		if (page_has_private(to_page)) {
			try_to_free_buffers(to_page);
			goto out_unlock_both_remove_from_migration_pte;
		}
	} else if (page_mapped(to_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(to_page) && !PageKsm(to_page) &&
						!to_anon_vma, to_page);
		try_to_migrate(to_page, 0);
		to_page_was_mapped = 1;
	}

	if (!page_mapped(from_page) && !page_mapped(to_page)) {
		// exchange_to_new_page不需要处理refcount
		// refcount由调用者exchange_pages函数处理
		rc = exchange_to_new_page(to_page, from_page, mode);
		// printk("exchange_to_new_page rc=%d\n", rc);
		// printk("exchange_to_new_page from: %px, to %px: %d\n", from_page, to_page, rc);
	}

	if (to_page_was_mapped) {
		if (rc == MIGRATEPAGE_SUCCESS)
			swap(to_page->index, to_index);

		remove_migration_ptes(to_page,
			rc == MIGRATEPAGE_SUCCESS ? from_page : to_page, false);

		if (rc == MIGRATEPAGE_SUCCESS)
			swap(to_page->index, to_index);
	}


out_unlock_both_remove_from_migration_pte:
	if (from_page_was_mapped) {
		if (rc == MIGRATEPAGE_SUCCESS)
			swap(from_page->index, from_index);

		remove_migration_ptes(from_page,
			rc == MIGRATEPAGE_SUCCESS ? to_page : from_page, false);

		if (rc == MIGRATEPAGE_SUCCESS)
			swap(from_page->index, from_index);
	}

out_unlock_both:
	/* Drop an anon_vma reference if we took one */
	if (to_anon_vma)
		put_anon_vma(to_anon_vma);
	unlock_page(to_page);
out_unlock:

	/* Drop an anon_vma reference if we took one */
	if (from_anon_vma)
		put_anon_vma(from_anon_vma);
	unlock_page(from_page);
out:
	return rc;
}

bool exchange_pages(struct page *from_page, struct page *to_page)
{
	int rc= 1111;

	if (page_count(from_page) == 1) {
		/* page was freed from under us. So we are done  */
		ClearPageActive(from_page);
		ClearPageUnevictable(from_page);

		put_page(from_page);
		dec_node_page_state(from_page, NR_ISOLATED_ANON +
					page_is_file_lru(from_page));

		if (page_count(to_page) == 1) {
			ClearPageActive(to_page);
			ClearPageUnevictable(to_page);
			put_page(to_page);
		} else {
			// printk("page_count(from_page) == 1, goto putback_to_page");
			goto putback_to_page;
		}

		return false;
	}

	if (page_count(to_page) == 1) {
		/* page was freed from under us. So we are done  */
		ClearPageActive(to_page);
		ClearPageUnevictable(to_page);

		put_page(to_page);

		dec_node_page_state(to_page, NR_ISOLATED_ANON +
				page_is_file_lru(to_page));

		dec_node_page_state(from_page, NR_ISOLATED_ANON +
				page_is_file_lru(from_page));
		putback_lru_page(from_page);
		return false;
	}

	/* TODO: compound page not supported */
	if (!can_be_exchanged(from_page, to_page)
		 || page_mapping(from_page)
		/* allow to_page to be file-backed page  */
		/*|| page_mapping(to_page)*/
		) {
		// printk("!can_be_exchanged || page_mapping(from_page), goto putback");
		goto putback;
	}

	// debug : 没有出现bug
	// goto putback;

	rc = unmap_and_exchange(from_page, to_page, 0, MIGRATE_SYNC);
	// printk("return from unmap_and_exchange, rc=%d", rc);
	// rc = -EAGAIN;

	// 不管exchange成功与否，都应该将他们放回lru中?
putback:
	dec_node_page_state(from_page, NR_ISOLATED_ANON +
			page_is_file_lru(from_page));
	putback_lru_page(from_page);

putback_to_page:
	dec_node_page_state(to_page, NR_ISOLATED_ANON +
			page_is_file_lru(to_page));
	putback_lru_page(to_page);

	if (rc != MIGRATEPAGE_SUCCESS) {
		// printk("fail rc=%d", rc);
		return false;
	}

	return true;
}

static bool should_exchange(struct page *page)
{
	if (page_zonenum(page) == ZONE_DMA)
	{
		goto skip;
	}
	if (PageCompound(page) || PageHead(page) || PageTail(page)) {
		goto skip;
	}

	if (!page || PageKsm(page)) {
		goto skip;
	}

	if (PageUnevictable(page)) {
		goto skip;
	}

	// 参考migrate_misplaced_page
	if (page_is_file_lru(page) && PageDirty(page))
		goto skip;

	return true;

skip:
	return false;
}

// 调用该函数之前, from_page与to_page均执行过get_page
// 无论成功与否，put_page工作由该函数完成
bool __try_exchang_page(struct page *from_page, struct page *to_page)
{
	bool success_status = false;

	if (isolate_lru_page(from_page) != 0) {
		// printk("isolate from_page fail");
		goto out;
	}

	if (isolate_lru_page(to_page) != 0) {
		// printk("isolate to_page fail");
		putback_lru_page(from_page); // from_page: isolate_lru_page
		goto out;
	}

	// 退回isolate产生的reference
	put_page(from_page);
	put_page(to_page);

	// 直到两个都成功isolate才修改统计信息，省得频繁回退
	inc_node_page_state(from_page,
		NR_ISOLATED_ANON + page_is_file_lru(from_page));
	inc_node_page_state(to_page,
		NR_ISOLATED_ANON + page_is_file_lru(to_page));

	// 此时只剩下调用者的reference，exchange_pages函数可以安心地迁移
	success_status = exchange_pages(from_page, to_page);
	if (success_status)
		count_vm_event(LMY_EXCHANGE_PAGE_SUCCESS);
	else
		count_vm_event(LMY_EXCHANGE_PAGE_FAIL);

	// 迁移失败的情况由exchange_pages函数处理了

	return success_status;

out:
	// 此处释放的是调用者的reference(numa_migrate_prep和try_exchange_page函数)
	put_page(from_page);
	put_page(to_page);
	count_vm_event(LMY_EXCHANGE_PAGE_FAIL);
	return false;
}

// 调用之前对from_page执行过一次get_page
bool try_exchange_page(struct page *from_page, int target_id)
{
	struct pglist_data *target_pgdat = NODE_DATA(target_id);
	struct page *to_page = NULL;
	bool is_success;

	unsigned int from_latency_sec;
	unsigned int to_latency_sec;

	count_vm_event(LMY_EXCHANGE_PAGE_ALL);

	// 如果from页面在candidate page list中，先将其摘出来
	if (in_candidate_pages(from_page)) {
		del_candidate_page(from_page);
	}

	// 获取to_page
	to_page = dequeue_candidate_page(target_pgdat);
	if (!to_page) {
		count_vm_event(LMY_EXCHANGE_PAGE_FAIL_NO_CANDIDATE_PAGE);
		goto out;
	}

	// check from_page and to_page
	if (!should_exchange(from_page) || !should_exchange(to_page)) {
		if (!should_exchange(from_page))
			printk("!should_exchange(from_page)");
		if (!should_exchange(to_page))
			printk("!should_exchange(to_page)");
		goto out;
	}

	// filter workingset
	if (PageWorkingset(from_page) || PageWorkingset(to_page)) {
		printk("from_page=%px, PageWorkingset=%d, PageAnon(page)=%d", from_page, PageWorkingset(from_page), PageAnon(from_page));
		printk("to_page=%px, PageWorkingset=%d, PageAnon(page)=%d", to_page, PageWorkingset(to_page), PageAnon(to_page));
		goto out;
	}

	// 开始exchange
	from_latency_sec = get_page_info_from_page(from_page)->access_interval_sec;
	to_latency_sec = get_page_info_from_page(to_page)->access_interval_sec;
	// printk("before get to_page, from_page.ref_count=%d, to_page.ref_count=%d", page_count(from_page), page_count(to_page));

	// get to_page
	// to_page.ref = x + 1
	if (!get_page_unless_zero(to_page)) {
		printk("!get_page_unless_zero(to_page)");
		goto out;
	}

	// printk("after get to_page, from_page.ref_count=%d, to_page.ref_count=%d", page_count(from_page), page_count(to_page));

	// promote过程中，to_page是上层, from_page是下层
	count_exchange_event(TRY_EXCHANGE, page_to_nid(from_page), page_to_nid(to_page),
					to_latency_sec, from_latency_sec);
	// exchange pages
	is_success = __try_exchang_page(from_page, to_page);
	if (is_success) {
		count_exchange_event(EXCHANGE_SUCCESS, page_to_nid(from_page), page_to_nid(to_page),
					to_latency_sec, from_latency_sec);
	} else {
		count_exchange_event(EXCHANGE_FAIL, page_to_nid(from_page), page_to_nid(to_page),
					to_latency_sec, from_latency_sec);
	}
	return is_success;

out:
	// 此处释放的是调用者的get_page(numa_migrate_prep函数中)
	put_page(from_page);
	BUG_ON(in_atomic());
	return false;
}

#endif // CONFIG_PAGE_EXCHANGE