#include <linux/pagewalk.h>
#include <linux/hugetlb.h>
#include <linux/shm.h>
#include <linux/mman.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/security.h>
#include <linux/mempolicy.h>
#include <linux/personality.h>
#include <linux/syscalls.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/ksm.h>
#include <linux/uaccess.h>
#include <linux/mm_inline.h>
#include <linux/pgtable.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>

#include "../internal.h"

#include <linux/page_hotness/page_info.h>
#include <linux/page_hotness/page_hotness.h>
#include <linux/page_hotness/candidate_page.h>
#include <linux/page_common/page_hotness_stat.h>
#include <linux/page_common/page_hotness_sysctl.h>
#include <linux/page_hotness/base_histogram.h>
#include <linux/page_hotness/page_hotness_api.h>
#include <linux/page_hotness/page_gups_test.h>

#define MY_UNMAP_PAGE_NUM 1000
// #define MY_DEMOTE_PAGE_NUM 10000 // 约40MB
#define MY_DEMOTE_PAGE_NUM 50000 // 约200MB
// #define MY_DEMOTE_PAGE_NUM 0 // 

// unmap lru的过滤统计（仅kunmapd线程使用）
struct unmap_filter_stat {
	// unmap scan stat
	unsigned long nr_scan_time;
	unsigned long nr_all_pages;
	unsigned long nr_already_unmap;
	unsigned long nr_recently_accessed;
	unsigned long nr_already_migrating;
	unsigned long nr_skip_page;
	unsigned long nr_invalid_pfn;
	unsigned long nr_pebs_skip;
	unsigned long nr_not_access;
	unsigned long nr_fill_page;

	// unmap stat
	unsigned long nr_try_unmap;
	unsigned long nr_active_unmap;
	unsigned long nr_inactive_unmap;

	// coldness node stat
	unsigned long nr_bottom_tier; // 位于最底层，没有可以做demote的节点
	unsigned long nr_upper_enough_free_space; // 由于上层的容量充足，“理论上”不需要进行demote
	unsigned long nr_frequent_promote_nomem; // 即使上层容量充足，但是实际有许多个promote请求因为没有空闲空间失败
	unsigned long nr_next_no_free_space; // 由于下层的容量不够，无法进行demote
	unsigned long nr_second_no_free_space; // 由于下层的容量不够，无法进行demote
	unsigned long nr_normal_hot_threshold; // 由于热度分布不适合，不建议进行demote
	unsigned long nr_should_demote;

	// coldness page stat
	unsigned long coldness_nr_all_pages;
	unsigned long coldness_nr_not_in_unmap_state;
	unsigned long nr_coldness_all_pages;

	// demote stat
	unsigned long nr_try_demote;
	unsigned long nr_isolate_page;
	unsigned long nr_active_demote;
	unsigned long nr_inactive_demote;

	// demote histogram
	unsigned long fill_demote_histogram[STAT_HISTOGRAM_LENGTH]; // demote的页面热度分布情况

	// candidate page
	unsigned long nr_add_candidate_pages;

	// overhead stat
	unsigned long fill_time_ms;
	unsigned long unmap_time_ms;
	unsigned long scan_coldness_time_ms;
	unsigned long demote_time_ms;
};
struct unmap_filter_stat unmap_filter_stats[MYNODES] = {0};

unsigned long global_demote;
unsigned long global_demote_last_val;

// unmap lru的操作统计（kunmapd与ksampled线程均会使用）
struct local_unmap_stat {
	unsigned long nr_all;
	unsigned long nr_try_unmap;
	unsigned long nr_unmap;
};

enum demote_cond_flags {
	DEMODE_BOTTOM_TIER = 1 << 0, // 位于最下层节点时，不进行demote操作
	DEMODE_HIGHER_TIER_ENOUGH_CAPATITY = 1 << 1, // 上层容量充足时，"理论上"不需要进行demote（demote的目的是腾出空间）
	DEMODE_FREQUENT_PROMOTE_FAIL_NOMEM = 1 << 2, // 上层容量充足时，但是出现大量promote请求失败
	DEMODE_MEMORY_THRASHING = 1 << 3, // 内存颠簸/节点热度条件不合适时，不进行demote
	DEMODE_NEXT_NODE_NO_CAPATITY = 1 << 4, // 如果next node没有空间时，就无法demote到这里
	DEMODE_SECOND_NODE_NO_CAPATITY = 1 << 5, // 如果second node没有空间时，就无法demote到这里，不过可以exchange
};

#ifdef CONFIG_PAGE_CHANGE_PROT

// invalid return true
// valid return false
static bool my_invalid_vma(struct vm_area_struct *vma, void *arg)
{
	// check vma (the code is copied from task_numa_work)
	if (!vma_migratable(vma) ||
		is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
		goto out_invalid;
	}
    
	if (!vma->vm_mm ||
		(vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
		goto out_invalid;

	if (!vma_is_accessible(vma))
		goto out_invalid;

	return false;

out_invalid:
	return true;
}

static bool should_change_prot(struct page *page)
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

	// 理论上不需要判断文件
	// if (page_is_file_lru(page) && PageDirty(page)) {
	// 	goto skip;
	// }

	// change_pte_range本身在prot_numa情况下,需要跳过一些paga，这里提前过滤

	// if (fault_in_kernel_space(page_address(page))) {
	// 	prot_stat.nr_kernel_page++;
	// 	res = false;
	// 	// return false;
	// }

	// if (!access_ok(page, PAGE_SIZE)) {
	// 	prot_stat.nr_access_not_ok ++;
	// 	return false;
	// }


	// if (PageReported(page)) {
	// 	return false;
	// }

	// 是否要过滤掉内核页
	// if (page_to_pfn())
	return true;

skip:
	return false;
}

// 参考 try_to_migrate_one
static bool try_to_unmap_one_lru_page(struct page *page, struct vm_area_struct *vma,
		     unsigned long address, void *arg)
{
	struct mm_struct *mm = vma->vm_mm;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pte_t pteval;
	struct page *subpage;
	bool ret = true;
	struct mmu_notifier_range range;
	enum ttu_flags flags = 0;
	// printk("try_to_unmap_one_lru_page: page=%px", page);
	struct local_unmap_stat *local_unmap_stat = (struct local_unmap_stat*)arg;

	local_unmap_stat->nr_all ++;

	if (PageHuge(page) || PageTransHuge(page)) {
        return false;
    }

    if (is_zone_device_page(page) || PageHWPoison(page)) {
        printk("try to unmap device page or HWPoison page, false");
        return false;
    }

	if (!vma_migratable(vma) ||
        is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
		printk("invalid vma: type 1");
        return false;
    }

    if (!vma->vm_mm || (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ))) {
		printk("invalid vma: type 2");
        return false;
	}

    if (!vma_is_accessible(vma)) {
		printk("invalid vma: !vma_is_accessible(vma)");
        return false;
	}

	local_unmap_stat->nr_try_unmap ++;
	// atomic_inc(&unmap_stat.nr_try_unmap);

	/*
	 * When racing against e.g. zap_pte_range() on another cpu,
	 * in between its ptep_get_and_clear_full() and page_remove_rmap(),
	 * try_to_migrate() may return before page_mapped() has become false,
	 * if page table locking is skipped: use TTU_SYNC to wait for that.
	 */
	if (flags & TTU_SYNC)
		pvmw.flags = PVMW_SYNC;

	/*
	 * unmap_page() in mm/huge_memory.c is the only user of migration with
	 * TTU_SPLIT_HUGE_PMD and it wants to freeze.
	 */
	if (flags & TTU_SPLIT_HUGE_PMD)
		split_huge_pmd_address(vma, address, true, page);

	// range.end = PageKsm(page) ?
	// 		address + PAGE_SIZE : vma_address_end(page, vma);
	range.end = address + PAGE_SIZE;
	// MMU_NOTIFY_PROTECTION_VMA参考change_pmd_range函数
	mmu_notifier_range_init(&range, MMU_NOTIFY_PROTECTION_VMA, 0, vma, vma->vm_mm,
				address, range.end);
	mmu_notifier_invalidate_range_start(&range);

	while (page_vma_mapped_walk(&pvmw)) {
		/* PMD-mapped THP migration entry */
		if (!pvmw.pte) {
			continue;
		}

		/* Unexpected PMD-mapped THP? */
		VM_BUG_ON_PAGE(!pvmw.pte, page);

		subpage = page - page_to_pfn(page) + pte_pfn(*pvmw.pte);
		address = pvmw.address;

		if (PageHuge(page) && !PageAnon(page)) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}

#if 0
		flush_tlb_batched_pending(vma->vm_mm);
		arch_enter_lazy_mmu_mode();
#else
		/* Nuke the page table entry. */
		flush_cache_page(vma, address, pte_pfn(*pvmw.pte));
		pteval = ptep_clear_flush(vma, address, pvmw.pte);
#endif
		/* Move the dirty bit to the page. Now the pte is gone. */
		if (pte_dirty(pteval))
			set_page_dirty(page);

		/* Update high watermark before we lower rss */
		update_hiwater_rss(mm);

		if (is_zone_device_page(page)) {
			printk(KERN_ALERT "is_zone_device_page(page)");
			continue;	
		} else if (pte_unused(pteval) && !userfaultfd_armed(vma)) {
			printk(KERN_ALERT "pte_unused(pteval) && !userfaultfd_armed(vma)");
			continue;
		}  else if (PageHWPoison(page)) {
			printk(KERN_ALERT "PageHWPoison(page)");
			continue;	
		} else {
			pte_t ptent;
#if 0
			pte_t oldpte;
			oldpte = ptep_modify_prot_start(vma, address, pvmw.pte);
			ptent = pte_modify(oldpte, PAGE_NONE);
			ptep_modify_prot_commit(vma, address, pvmw.pte, oldpte, ptent);
#else
			ptent = pte_modify(pteval, PAGE_NONE);

			// if (pte_write(pteval))
			// 	ptent = pte_mk_savedwrite(ptent);
			// if (pte_uffd_wp(pteval))
			// 	ptent = pte_mkuffd_wp(ptent);
			// if (pte_soft_dirty(pteval))
			// 	ptent = pte_mkwrite(ptent);

		// 	pr_alert("BUG: Bad page map in process %s  pte:%08llx pmd:%08llx\n",
		//  current->comm,
		//  (long long)pte_val(pte), (long long)pmd_val(*pmd));

			// printk("pteval=%lx, ptent=%lx", pteval.pte, ptent.pte);
			set_pte_at(mm, address, pvmw.pte, ptent);
#endif
			// atomic_inc(&unmap_stat.unmap);
			local_unmap_stat->nr_unmap ++;

			set_page_unmap_state(page);
		}
	}

#if 0
	arch_leave_lazy_mmu_mode();
#else
	mmu_notifier_invalidate_range_end(&range);
#endif
	return ret;
}

static int page_not_mapped(struct page *page)
{
	return !page_mapped(page);
}

static int fill_lru_pages(struct list_head *head, struct page *pages[], struct unmap_filter_stat *stat)
{
	int len = 0;
	struct page* page = NULL;

	stat->nr_scan_time ++;

	list_for_each_entry(page, head, lru) {
		if (len == MY_UNMAP_PAGE_NUM) {
			break;
		}
		stat->nr_all_pages ++;

		// if (!PageAnon(page)) {
		// 	continue;
		// }

		if (in_page_unmap_state(page)) {
			stat->nr_already_unmap ++;
			continue;
		}

		if (PageMigrating(lookup_page_ext(page))) {
			stat->nr_already_migrating ++;
			continue;
		}

#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG
		if (page_recently_accessed(page)) {
			stat->nr_recently_accessed ++;
			continue;
		}
#endif

		if (!should_change_prot(page)) {
			stat->nr_skip_page ++;
			continue;
		}

		// if (!pfn_valid(page_to_pfn(page))) {
		// 	stat->nr_invalid_pfn ++;
		// 	continue;
		// }

		pages[len++] = page;
	}

	stat->nr_fill_page += len;

	return len;
}

int unmap_lru_pages(struct page* pages[], int len, struct unmap_filter_stat *stat)
{
	int i = 0;

	// 局部统计变量 rwc.arg
	struct local_unmap_stat local_unmap_stat = {0};
	struct rmap_walk_control rwc = {
		.rmap_one = try_to_unmap_one_lru_page,
		.arg = &local_unmap_stat,
		// .anon_lock = page_lock_anon_vma_read,
		.done = page_not_mapped,
		.invalid_vma = my_invalid_vma,
	};

	for (i = 0; i < len; i++) {
		struct page* page = pages[i];

		if (unlikely(!page)) {
			printk("unmap_lru_pages: page is null");
			continue;
		}
		if (unlikely(!pfn_valid(page_to_pfn(page)))) {
			printk("unmap_lru_pages: pfn(%lx) is invalid", page_to_pfn(page));
			continue;
		}

		// 参考page_referenced中的过滤
		if (!page_rmapping(page))
			continue;

		// if (!PageAnon(page) || PageKsm(page))
		// 	continue;

		if (PageKsm(page))
			continue;

		if (PageWriteback(page))
			continue;

		// get page
		if (!get_page_unless_zero(page)) {
			continue;
		}

		// lock page
		if (!trylock_page(page)) {
			put_page(page);
			continue;
		}

		
		// printk("unmap_lru_pages: page=%px, xchg_page_access_time=%ums", page, jiffies_to_msecs(jiffies));

		// printk("unmap_lru_pages before: page=%px, ref_count=%d, mapcount=%d", page, page_count(page), page_mapcount(page));
		WARN_ON(!(PageSlab(page) || page_has_type(page)) && page_mapcount(page) < 0);
		WARN_ON(!(PageSlab(page) || page_has_type(page)) && page_count(page) < 0);
		rmap_walk(page, &rwc);
		// rmap_walk(page, &rwc);
		// printk("unmap_lru_pages after: page=%px, ref_count=%d, mapcount=%d", page, page_count(page), page_mapcount(page));
		WARN_ON(!(PageSlab(page) || page_has_type(page)) && page_mapcount(page) < 0);
		WARN_ON(!(PageSlab(page) || page_has_type(page)) && page_count(page) < 0);

		unlock_page(page);
		put_page(page);
	}

	// printk("local_unmap_stat: nr_all=%d, nr_try_unmap = %d, nr_unmap=%d", local_unmap_stat.nr_all, local_unmap_stat.nr_try_unmap,
	// 		local_unmap_stat.nr_unmap);
	stat->nr_try_unmap += local_unmap_stat.nr_try_unmap;

	return local_unmap_stat.nr_unmap;
}

static struct page *my_alloc_demote_page(struct page *page, unsigned long node)
{
	struct migration_target_control mtc = {
		/*
		 * Allocate from 'node', or fail quickly and quietly.
		 * When this happens, 'page' will likely just be discarded
		 * instead of migrated.
		 */
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			    __GFP_THISNODE  | __GFP_NOWARN |
			    __GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = node
	};

	return alloc_migration_target(page, (unsigned long)&mtc);
}

static unsigned int my_demote_page_list(struct list_head *demote_pages,
				     int demote_target_nid)
{
	unsigned int nr_succeeded;
	int err;

	if (list_empty(demote_pages))
		return 0;

	if (demote_target_nid == NUMA_NO_NODE)
		return 0;

	/* Demotion ignores all cpuset and mempolicy settings */
	err = migrate_pages(demote_pages, my_alloc_demote_page, NULL,
			    demote_target_nid, MIGRATE_ASYNC, MR_DEMOTION,
			    &nr_succeeded);

	if (err)
		putback_movable_pages(demote_pages);

	return nr_succeeded;
}

static struct page* active_pages[MY_UNMAP_PAGE_NUM];
static struct page* inactive_pages[MY_UNMAP_PAGE_NUM];

void unmap_lruvec_pages(struct lruvec *lruvec)
{
	int active_change_cnt = 0;
	int inactive_change_cnt = 0;
	int active_pages_len = 0;
	int inactive_pages_len = 0;

	int nid = lruvec_pgdat(lruvec)->node_id;

	ktime_t start, diff;

	// 1. fill page
	start = ktime_get();
	// lru_add_drain();
	if (!spin_trylock_irq(&lruvec->lru_lock)) {
		return ;
	}
	// spin_lock_irq(&lruvec->lru_lock);
	active_pages_len = fill_lru_pages(&lruvec->lists[LRU_ACTIVE_ANON], active_pages, &unmap_filter_stats[nid]);
	inactive_pages_len = fill_lru_pages(&lruvec->lists[LRU_INACTIVE_ANON], inactive_pages, &unmap_filter_stats[nid]);
	spin_unlock_irq(&lruvec->lru_lock);
	diff = ktime_sub(ktime_get(), start);
	unmap_filter_stats[nid].fill_time_ms += ktime_to_ms(diff);

	// 2. unmap page
	start = ktime_get();
	active_change_cnt = unmap_lru_pages(active_pages, active_pages_len, &unmap_filter_stats[nid]);
	// printk("active_change_cnt=%d\n", active_change_cnt);

	inactive_change_cnt = unmap_lru_pages(inactive_pages, inactive_pages_len, &unmap_filter_stats[nid]);
	// printk("inactive_change_cnt=%d\n", inactive_change_cnt);
	diff = ktime_sub(ktime_get(), start);
	unmap_filter_stats[nid].unmap_time_ms += ktime_to_ms(diff);

	// 3. update stat
	unmap_filter_stats[nid].nr_active_unmap += active_change_cnt;
	unmap_filter_stats[nid].nr_inactive_unmap += inactive_change_cnt;
}

static int node_scan_freq[MY_MEM_NODES];
void change_memcg_page_prot(struct mem_cgroup *target_memcg)
{
	int nid;

	// 参考age_active_anon函数
	for_each_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		struct mem_cgroup *memcg = NULL;
		struct lruvec *lruvec = NULL;
		int lruvec_cnt = 0;

		if (!pgdat) {
			continue;
		}

		if (nid == 0 || nid == 1) {
			if (node_scan_freq[nid] < 4) { // 降低dram层扫描频率
				node_scan_freq[nid] ++;
				continue;
			} else {
				node_scan_freq[nid] = 0;
			}
		}

		// printk("nid=%d", nid);
		memcg = target_memcg;
		if (!memcg) {
			printk("change_memcg_page_prot: memcg is null");
			continue;
		}

		do {
			lruvec_cnt ++;
			lruvec = mem_cgroup_lruvec(memcg, pgdat);
			// change
			unmap_lruvec_pages(lruvec);
			
			memcg = mem_cgroup_iter(target_memcg, memcg, NULL);
		} while (memcg);
		// printk("lruvec_cnt = %d", lruvec_cnt);
	}
}

bool reach_cold_threshold(unsigned long latency_s, unsigned long cold_threshold)
{
	return latency_s >= cold_threshold;
}

bool reach_hot_threshold(unsigned long latency_s, unsigned long hot_threshold)
{
	return latency_s <= hot_threshold;
}

int demote_lru_pages(struct lruvec *lruvec, enum lru_list lru, struct page* pages[], int len, int demote_target_nid)
{
	int i;
	int nr_isolate = 0;
	int nr_reclaimed = 0; // 成功demote的页面数目
	LIST_HEAD(page_list);
	int nid = lruvec_pgdat(lruvec)->node_id;
	WARN_ON(nid >= MYNODES);

#if 0
	spin_lock_irq(&lruvec->lru_lock);
	isolate_lru_page_arr(lruvec, lru, pages, len, &page_list);
	spin_unlock_irq(&lruvec->lru_lock);
#else
	for (i = 0; i < len; i++) {
		struct page *page = pages[i];
		int nr_pages = compound_nr(page);

		if (nr_pages > 1)
			continue;

		// isolate_lru_page好像只针对LRU page，参考isolate_movable_page代码注释
		if (!PageLRU(page))
			continue;

		if (!get_page_unless_zero(page))
			continue;

		if (isolate_lru_page(page) != 0) {
			put_page(page);
			continue;
		}

		put_page(page);
		nr_isolate ++;
		list_add(&page->lru, &page_list);
		mod_node_page_state(page_pgdat(page),
			NR_ISOLATED_ANON + page_is_file_lru(page), nr_pages);
	}

	unmap_filter_stats[nid].nr_isolate_page += nr_isolate;
#endif

	nr_reclaimed = my_demote_page_list(&page_list, demote_target_nid);

	if (!list_empty(&page_list))
		putback_movable_pages(&page_list);
	WARN_ON(!list_empty(&page_list));

	global_demote += nr_reclaimed;

	return nr_reclaimed;
}

// 返回值：更新成功与否
// ret_latency_s：返回时间间隔
// 注意：进入这个函数时，外层有一个lruvec的lock
static bool update_page_coldness(struct page* page, unsigned int *ret_latency_s)
{
	unsigned int latency_ms, latency_s;
	struct page_ext *page_ext;
	struct page_info *pi;
	bool rc = false;
	unsigned long irq_flags;

	page_ext = lookup_page_ext(page);
	if (!page_ext || !PageTracked(page_ext))
		return false;
	pi = get_page_info(page_ext);

	// 先更新page info时间间隔信息

    spin_lock_irqsave(&pi->pi_lock, irq_flags);
	// 1. 在锁内再次检查unmap lru flag
	if (!PageUnmapLRU(lookup_page_ext(page))) {
		// 只有处于unmap状态的页面有时间间隔信息
		goto unlock;
	}
	// 2. 获取时间间隔，即 当前时刻 - unmap时刻
	if (!PageUnmapTime(page_ext)) {
		// 检查该分支，理论上PageUnmapLRU为true时，一定有时刻信息
		printk("try_fill_demote_pages: !PageUnmapTime(lookup_page_ext(page))");
		goto unlock;
	}
	latency_ms = get_numa_latency_ms(page, true); // ms
	latency_s = latency_ms / 1000;
	*ret_latency_s = latency_s;
	rc = true;

unlock:
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

#ifdef CONFIG_ACCESS_HISTOGRAM
	// 4. 更新直方图信息
	if (rc)
		access_histogram_insert_or_update(page, latency_s, false);
#endif

	return rc;
}

// #define DEBUG_SCAN_LRU_COLDNESS

#ifdef DEBUG_SCAN_LRU_COLDNESS
#define count_scan_event(x) do{\
	++(x);\
} while(0)
#else // !DEBUG_SCAN_LRU_COLDNESS
	#define count_scan_event(x) do{} while(0)
#endif // end of DEBUG_SCAN_LRU_COLDNESS

static int try_fill_demote_pages(struct list_head *head,
		int demote_flags, unsigned long cold_threshold,
		struct page *pages[], int *len, struct unmap_filter_stat *stat)
{
#ifdef DEBUG_SCAN_LRU_COLDNESS
	int nr_not_should_change_prot = 0;
	int nr_not_PageUnmapLRU = 0;
	int nr_not_update_page_coldness = 0;
	int nr_not_should_demote = 0;
	int nr_higher_tier_enough_capacity = 0;
	int nr_frequent_promote_fail_nomem = 0;
	int nr_skip_Migrating = 0;
	int nr_not_reach_cold_threshold = 0;
	int nr_reach_cold_threshold = 0;
	int nr_next_node_no_capatity = 0;
	int nr_second_node_no_capatity = 0;
	int nr_buffer_overflow = 0;
#endif
	struct page* page = NULL;
	unsigned int latency_s;

	int nr_try_demote = 0;
	int nr_add_candidate_pages = 0;
	bool should_demote_or_exchange = true;

	// 位于最下层节点时，不进行demote/exchange操作
	if (demote_flags & DEMODE_BOTTOM_TIER) {
		should_demote_or_exchange = false;
	}

	// 内存颠簸/节点热度条件不合适时，不进行demote/exchange
	// 注意此处是整体的热度条件，后面循环内部判断的是单个page的热度情况
	if (demote_flags & DEMODE_MEMORY_THRASHING) {
		should_demote_or_exchange = false;
	}

	list_for_each_entry(page, head, lru) {
		// int nr_pages = compound_nr(page);

		if (!should_change_prot(page)) {
			count_scan_event(nr_not_should_change_prot);
			continue;
		}

		// if (!pfn_valid(page_to_pfn(page))) {
		// 	continue;
		// }

		// 注意：此处无需关注页面是否最近是否访问过
		// 如果page最近被访问过，那么我们获得的延时就不是准确的访问间隔，而是unmap至今的间隔
		// 如果page最近未被访问过，那么使用PageUnmapLRU函数来做判断就足够了
		
		// 对于这类page，我们没有明确的coldness信息
		if (!PageUnmapLRU(lookup_page_ext(page))) {
			count_scan_event(nr_not_PageUnmapLRU);
			continue;
		}

		if (!update_page_coldness(page, &latency_s)) {
			count_scan_event(nr_not_update_page_coldness);
			continue;
		}

		// 不进行demote/exchange操作
		if (!should_demote_or_exchange) {
			count_scan_event(nr_not_should_demote);
			continue;
		}

		// 上层节点容量充足，选择不做demote/exchange操作
		if (demote_flags & DEMODE_HIGHER_TIER_ENOUGH_CAPATITY) {
			count_scan_event(nr_higher_tier_enough_capacity);
			if (demote_flags & DEMODE_FREQUENT_PROMOTE_FAIL_NOMEM) {
				// 需要demote操作
				count_scan_event(nr_frequent_promote_fail_nomem);
				continue;
			} else {
				// 无需demote操作
				continue;
			}
		}

		if (PageMigrating(lookup_page_ext(page))) {
			count_scan_event(nr_skip_Migrating);
			continue;
		}

		// 跳过较热页面, todo: hard code
		if (latency_s <= 5) {
			continue;
		}

		// cold page
		if (!reach_cold_threshold(latency_s, cold_threshold)) {
			count_scan_event(nr_not_reach_cold_threshold);
			// 该页面不符合cold page的阈值，不进行demote/exchange
			// 如果它位于candidate page list中，就摘出来
#ifdef CONFIG_CANDIDATE_PAGE
			if (in_candidate_pages(page)) {
				del_candidate_page(page);
				// WARN_ON(!del_rc);
			}
#endif
			continue;
		}
		count_scan_event(nr_reach_cold_threshold);

		// 2. 下层节点没有空闲空间，就无法再做demote操作，但可以放入candidate page list
		// 3. 本次扫描过程中设置的demote页面数目已经达到上限，选择放入candidate page list
		if ((demote_flags & DEMODE_NEXT_NODE_NO_CAPATITY && demote_flags & DEMODE_SECOND_NODE_NO_CAPATITY)
		 	|| *len >= MY_DEMOTE_PAGE_NUM) {

			if (demote_flags & DEMODE_NEXT_NODE_NO_CAPATITY) {
				count_scan_event(nr_next_node_no_capatity);
			}
			if (demote_flags & DEMODE_SECOND_NODE_NO_CAPATITY) {
				count_scan_event(nr_second_node_no_capatity);
			}
			if (*len >= MY_DEMOTE_PAGE_NUM) {
				count_scan_event(nr_buffer_overflow);
			}

#ifdef CONFIG_CANDIDATE_PAGE
			// 已经被放入candidate page list，跳过
			if (in_candidate_pages(page)) {
				continue;
			}

			enqueue_candidate_page(page);
			nr_add_candidate_pages ++;
#endif
			continue;
		}

		if (*len < MY_DEMOTE_PAGE_NUM) {
#ifdef CONFIG_CANDIDATE_PAGE
			// 已经被放入candidate page list，先删除再进行demote
			if (in_candidate_pages(page)) {
				del_candidate_page(page);
			}
#endif
			// 扫描到的前MY_DEMOTE_PAGE_NUM个页面，在可以进行demote的情况下，先进行demote操作
			// 通过静态数组缓存将页面直接返回，减少lru lock持有时间
			// printk("pages[%d]=%px", *len, page);
			pages[*len] = page;
			(*len) ++;

			// todo: 此处假设我们是以2的幂来统计直方图
			stat->fill_demote_histogram[get_stat_histogram_binary_order(latency_s)] ++;
		}

		nr_try_demote ++;
	}

	stat->nr_add_candidate_pages += nr_add_candidate_pages;
#ifdef DEBUG_SCAN_LRU_COLDNESS
	printk("cold_threshold=%lu", cold_threshold);
	printk("nr_not_should_change_prot=%d", nr_not_should_change_prot);
	printk("nr_not_PageUnmapLRU=%d", nr_not_PageUnmapLRU);
	printk("nr_not_update_page_coldness=%d", nr_not_update_page_coldness);
	printk("nr_not_should_demote=%d", nr_not_should_demote);
	printk("nr_higher_tier_enough_capacity=%d", nr_higher_tier_enough_capacity);
	printk("nr_frequent_promote_fail_nomem=%d", nr_frequent_promote_fail_nomem);
	printk("nr_skip_Migrating=%d", nr_skip_Migrating);
	printk("nr_not_reach_cold_threshold=%d", nr_not_reach_cold_threshold);
	printk("nr_reach_cold_threshold=%d", nr_reach_cold_threshold);
	printk("nr_next_node_no_capatity=%d", nr_next_node_no_capatity);
	printk("nr_second_node_no_capatity=%d", nr_second_node_no_capatity);
	printk("nr_buffer_overflow=%d", nr_buffer_overflow);
#endif

	return nr_try_demote;
}

int get_histogram_topx_time(struct base_histogram *access_histogram, unsigned long topx)
{
	int i;
	unsigned long sum = 0;
	

	if (!access_histogram || !access_histogram->histogram)
		return (int)0x3f3f3f3f;

	for (i = 0;  i < access_histogram->len; i++) {
		sum += access_histogram->histogram[i];
		if (sum >= topx) {
			return i;
		}
	}

	return i - 1;
}

// 返回值表示当前是否适合demote操作
// 如果适合demote，返回一个cold_threshold作为冷页面阈值
bool calc_cold_threshold(struct pglist_data *pgdat, struct base_histogram *access_histogram, unsigned long *cold_threshold)
{
	int nid = pgdat->node_id;
	int max_time;
	unsigned long max_time_cnt;
	int min_bound = 1;

	int next_nid = my_next_demotion_node(nid);
	struct base_histogram *next_demote_access_histogram;
	int next_topx_time;

	if (!access_histogram || !access_histogram->histogram)
		return false;

	// find the max time of access_histogram
	// 找到上层节点的直方图中最冷的那类页面
	for (max_time = access_histogram->len - 1; max_time >= min_bound; --max_time) {
		if (access_histogram->histogram[max_time] != 0)
			break;
	}

	// 如果最大的时间时间也很短，就不demote了
	if (max_time < min_bound) {
		return false;
	}

	// 最冷的那类页面的数目
	max_time_cnt = access_histogram->histogram[max_time];

	// todo
	next_demote_access_histogram = &NODE_DATA(next_nid)->access_histogram;
	if (!next_demote_access_histogram || !next_demote_access_histogram->histogram)
		return false;

	// 计算下层节点的第max_time_cnt页面的热度
	next_topx_time = get_histogram_topx_time(next_demote_access_histogram, max_time_cnt);
	// 如果下层topx的时间间隔 > 上层的时间间隔，说明下层更冷，那就不进行demote操作了
	if (next_topx_time > max_time_cnt) {
		return false;
	}

	// 执行demote操作，demote 时间间隔为max_time的页面，也就是demote上层最冷的那些页面
	// 前面的判断只是为了判断 上层最冷的那些页面 是否 比下层最热的页面更冷
	if (access_histogram->type == NORMAL_COUNT) {
		*cold_threshold = max_time;
	} else if (access_histogram->type == ORDER_COUNT) {
		*cold_threshold = 1 << (unsigned long)max_time;
	}
	return true;
}

int get_pgdat_demote_flags(struct pglist_data *pgdat, unsigned long *cold_threshold, int *demote_target_nid)
{
	int flags = 0;
	int nid = pgdat->node_id;
	int next_nid = my_next_demotion_node(nid);
	int second_nid = my_second_demotion_node(nid);
	*demote_target_nid = next_nid; // 先假设目标节点为next_nid

	// 位于最下层节点时，不进行demote操作
	if (next_nid == NUMA_NO_NODE && second_nid == NUMA_NO_NODE) {
		unmap_filter_stats[nid].nr_bottom_tier ++;
		flags |= DEMODE_BOTTOM_TIER;
		return flags;
	}

	// 容量充足时，不需要进行demote（demote的目的是腾出空间）
	if (pgdat_free_space_enough(pgdat)) {
		unmap_filter_stats[nid].nr_upper_enough_free_space ++;
		flags |= DEMODE_HIGHER_TIER_ENOUGH_CAPATITY;

		// promote失败数目如果未达到阈值就不进行demote操作
		if (global_promote_fail_nomem < global_promote_fail_nomem_threshold) {
			return flags;
		} else {
			unmap_filter_stats[nid].nr_frequent_promote_nomem ++;
			// 如果达到了阈值，就需要进行demote操作
			flags |= DEMODE_FREQUENT_PROMOTE_FAIL_NOMEM;
			// 重置计数器
			global_promote_fail_nomem = 0;
		}
	}

#ifdef CONFIG_STATIC_THRESHOLD
	*cold_threshold = sysctl_page_unmap_cold_threshold_sec;
#else
	// 内存颠簸/节点热度条件不合适时，不进行demote
	if (!calc_cold_threshold(pgdat, &pgdat->access_histogram, cold_threshold)) {
		unmap_filter_stats[nid].nr_normal_hot_threshold ++;
		flags |= DEMODE_MEMORY_THRASHING;
		return flags;
	}
#endif

	// 如果下层没有空间时，就无法进行demote，但可以进行exchange
	if (next_nid != NUMA_NO_NODE && !lowertier_pgdat_free_space_enough(NODE_DATA(next_nid))) {
		unmap_filter_stats[nid].nr_next_no_free_space ++;
		flags |= DEMODE_NEXT_NODE_NO_CAPATITY;
		*demote_target_nid = second_nid; // 再假设目标节点为second_nid

		if (second_nid != NUMA_NO_NODE && !lowertier_pgdat_free_space_enough(NODE_DATA(second_nid))) {
			unmap_filter_stats[nid].nr_second_no_free_space ++;
			flags |= DEMODE_SECOND_NODE_NO_CAPATITY;
			*demote_target_nid = NUMA_NO_NODE; // 均无法demote
			return flags;
		}
	}

	unmap_filter_stats[nid].nr_should_demote ++;
	// printk("cold_threshold = %lu", *cold_threshold);
	return flags;
}

// todo: 根据直方图信息，好像也能动态获得最大数组长度
static struct page* active_cold_pages[MY_DEMOTE_PAGE_NUM];
static struct page* inactive_cold_pages[MY_DEMOTE_PAGE_NUM];
static void scan_lruvec_coldness(struct lruvec *lruvec, struct base_histogram *access_histogram)
{
	int active_cold_pages_len = 0; // 从active list中得到的冷页面数目
	int inactive_cold_pages_len = 0; // 从inactive list中得到的冷页面数目

	int nr_active_try_demote = 0; // 尝试demote的页面数目
	int nr_inactive_try_demote = 0; // 尝试demote的页面数目
	int nr_active_reclaimed = 0; // 成功demote的页面数目
	int nr_inactive_reclaimed = 0; // 成功demote的页面数目

	ktime_t start, diff;

	unsigned long cold_threshold = 10000; // 初始化一个较大的数，实际的阈值由get_pgdat_demote_flags决定
	int demote_target_nid = NUMA_NO_NODE;
	struct pglist_data *pgdat;
	int nid;
	int demote_flags = 0;
	
	pgdat = lruvec_pgdat(lruvec);
	BUG_ON(!pgdat);
	nid = pgdat->node_id;
	demote_flags = get_pgdat_demote_flags(pgdat, &cold_threshold, &demote_target_nid);
	// 1. 扫描lru，更新page时间间隔，填充demote page，填充candidate page list
	start = ktime_get();
	if (!spin_trylock_irq(&lruvec->lru_lock))
		return ;
	if (demote_flags & DEMODE_BOTTOM_TIER) {
		// 后续实现candidate page的填充
		;
	} else {
		#ifdef DEBUG_SCAN_LRU_COLDNESS
			printk("nid=%d, demote_target_nid=%d, active, try_fill_demote_pages", nid, demote_target_nid);
		#endif
		nr_active_try_demote = try_fill_demote_pages(&lruvec->lists[LRU_ACTIVE_ANON], demote_flags, cold_threshold,
													active_cold_pages, &active_cold_pages_len, &unmap_filter_stats[nid]);
		#ifdef DEBUG_SCAN_LRU_COLDNESS
			printk("nid=%d, demote_target_nid=%d, inactive, try_fill_demote_pages", nid, demote_target_nid);
		#endif
		nr_inactive_try_demote = try_fill_demote_pages(&lruvec->lists[LRU_INACTIVE_ANON], demote_flags, cold_threshold,
													inactive_cold_pages, &inactive_cold_pages_len, &unmap_filter_stats[nid]);
	}
	spin_unlock_irq(&lruvec->lru_lock);
	diff = ktime_sub(ktime_get(), start);
	unmap_filter_stats[nid].scan_coldness_time_ms += ktime_to_ms(diff);

	// 2. demote页面
	start = ktime_get();
	nr_active_reclaimed = demote_lru_pages(lruvec, LRU_ACTIVE_ANON, active_cold_pages, active_cold_pages_len, demote_target_nid);
	nr_inactive_reclaimed = demote_lru_pages(lruvec, LRU_INACTIVE_ANON, inactive_cold_pages, inactive_cold_pages_len, demote_target_nid);
	diff = ktime_sub(ktime_get(), start);
	unmap_filter_stats[nid].demote_time_ms += ktime_to_ms(diff);

	WARN_ON(nid >= MYNODES);
	// 尝试demote的数目
	unmap_filter_stats[nid].nr_try_demote += nr_active_try_demote + nr_inactive_try_demote;
	// 实际demote成功的数目
	unmap_filter_stats[nid].nr_active_demote += nr_active_reclaimed;
	unmap_filter_stats[nid].nr_inactive_demote += nr_inactive_reclaimed;
	__count_vm_events(LMY_DEMOTE_PAGE, nr_active_reclaimed + nr_inactive_reclaimed);
}

void dump_all_node_histogram(void)
{
	int nid = 0;
	char buf[50];

	printk("node_histogram:");
	for_each_node(nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);

		if (!pgdat) {
			continue;
		}
		snprintf(buf, 50, "nid=%d", nid);
		base_histogram_dump(buf, &pgdat->access_histogram, false);
	}
}

void scan_memcg_page_coldness(struct mem_cgroup *target_memcg)
{
	int nid;
	int node_len = 0;

	for_each_node(nid) {
		++ node_len;
	}

	// 参考age_active_anon函数
	// for_each_node(nid) {
	// todo: 此处希望能够逆序遍历，先demote下面的节点
	for (nid = node_len-1; nid >= 0; --nid) {
		struct pglist_data *pgdat = NODE_DATA(nid);
		struct mem_cgroup *memcg = NULL;
		struct lruvec *lruvec = NULL;
		int lruvec_cnt = 0;
		struct base_histogram *access_histogram;

		if (!pgdat) {
			continue;
		}

		// printk("nid=%d", nid);
		// BUG_ON(nid > MY_MEM_NODES);
		access_histogram = &pgdat->access_histogram;

		memcg = target_memcg;
		if (!memcg) {
			printk("scan_memcg_page_coldness: memcg is null");
			continue;
		}

		do {
			lruvec_cnt ++;
			lruvec = mem_cgroup_lruvec(memcg, pgdat);
			// scan
			scan_lruvec_coldness(lruvec, access_histogram);
			
			memcg = mem_cgroup_iter(target_memcg, memcg, NULL);
		} while (memcg);

		// printk("lruvec_cnt = %d", lruvec_cnt);
	}
}

/******************* kunmapd *********************/
struct task_struct *unmap_task = NULL;


static struct task_struct *
find_lively_task_by_vpid(pid_t vpid)
{
	struct task_struct *task;

	rcu_read_lock();
	if (!vpid)
		task = current;
	else
		task = find_task_by_vpid(vpid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	if (!task)
		return ERR_PTR(-ESRCH);

	return task;
}

// 根据 PID 获取对应的 mem_cgroup
static struct mem_cgroup *get_mem_cgroup_from_pid(pid_t pid) {
    struct task_struct *task = NULL;
    struct mem_cgroup *memcg = NULL;
	int err;

	task = find_lively_task_by_vpid(pid);
	printk("get_mem_cgroup_from_pid pid=%d, task=%px\n", pid, task);
	if (IS_ERR(task)) {
		printk("IS_ERR(task)\n");
		err = PTR_ERR(task);
		return NULL;
	}

    if (task != NULL) {
        // 获取进程所在的 mem_cgroup
		memcg = get_mem_cgroup_from_mm(task->mm);
		if (memcg == root_mem_cgroup) {
			printk("memcg == root_mem_cgroup");
		} else if (memcg == NULL) {
			printk("memcg == NULL");
		}
    }

    return memcg;
}

void dump_demote_histogram(void)
{
	int nid;
	printk("fill_demote_histogram (unit: 2^n second):\n");
	for (nid = 0; nid < MY_MEM_NODES; nid++) {
		printk("nid=%d: ", nid);
		dump_histogram(unmap_filter_stats[nid].fill_demote_histogram);
	}
}

// todo：此处暂时为硬编码
#define DUMP_NODE_STAT(item)\
	printk(#item"=%lu, %lu, %lu, %lu",\
		unmap_filter_stats[0].item,\
		unmap_filter_stats[1].item,\
		unmap_filter_stats[2].item,\
		unmap_filter_stats[3].item);

static void dump_node_stat(void)
{
	printk("-------------unmap_filter_stats begin------------\n");
	printk(">>> overhead stat:\n");
	DUMP_NODE_STAT(fill_time_ms);
	DUMP_NODE_STAT(unmap_time_ms);
	DUMP_NODE_STAT(scan_coldness_time_ms);
	DUMP_NODE_STAT(demote_time_ms);

	printk(">>> unmap scan stat:\n");
	DUMP_NODE_STAT(nr_scan_time);
	DUMP_NODE_STAT(nr_all_pages);
	DUMP_NODE_STAT(nr_already_unmap);
	DUMP_NODE_STAT(nr_recently_accessed);
	DUMP_NODE_STAT(nr_already_migrating);
	DUMP_NODE_STAT(nr_skip_page);
	DUMP_NODE_STAT(nr_invalid_pfn);
	DUMP_NODE_STAT(nr_pebs_skip);
	DUMP_NODE_STAT(nr_not_access);
	DUMP_NODE_STAT(nr_fill_page);

	printk(">>> unmap stat:\n");
	DUMP_NODE_STAT(nr_try_unmap);
	DUMP_NODE_STAT(nr_active_unmap);
	DUMP_NODE_STAT(nr_inactive_unmap);

	printk(">>> demote scan stat:\n");
	DUMP_NODE_STAT(nr_bottom_tier);
	DUMP_NODE_STAT(nr_upper_enough_free_space);
	DUMP_NODE_STAT(nr_frequent_promote_nomem);
	DUMP_NODE_STAT(nr_next_no_free_space);
	DUMP_NODE_STAT(nr_second_no_free_space);
	DUMP_NODE_STAT(nr_normal_hot_threshold);
	DUMP_NODE_STAT(nr_should_demote);

	printk(">>> demote stat:\n");
	DUMP_NODE_STAT(nr_try_demote);
	DUMP_NODE_STAT(nr_isolate_page);
	DUMP_NODE_STAT(nr_active_demote);
	DUMP_NODE_STAT(nr_inactive_demote);

	printk(">>> candidate page stat:\n");
	DUMP_NODE_STAT(nr_add_candidate_pages);

	dump_demote_histogram();
	dump_promote_events();
	dump_exchange_events();

	printk("-------------unmap_filter_stats end------------\n");
}

pid_t global_pid;
static int kunmapd(void *_pid)
{
	pid_t pid = *(pid_t*)_pid;
	// unsigned long sleep_timeout = usecs_to_jiffies(2000); // 2ms
	// unsigned long sleep_timeout = msecs_to_jiffies(1000); // 1s
	unsigned long sleep_timeout = msecs_to_jiffies(100); // 100ms
	// unsigned long sleep_timeout = msecs_to_jiffies(1000); // 1s
	struct mem_cgroup *target_memcg;
	unsigned long kunmapd_overhead_ms = 0;
	int scan_coldness_cnt = 0;
	int dump_coldness_cnt = 0;
	int i;

	/************* most_access_node ***************/
	memset(node_hint_page_faults, 0, MY_MEM_NODES * sizeof(unsigned long));
	max_hint_page_faults = 0;
	most_access_node = 0;

	/************* promote alloc fail stat ***************/
	global_promote_fail_nomem = 0;

	// 为什么有时候传入的pid不太对，是我传参用法不对吗
	printk("kunmapd pid=%d, global_pid=%d\n", pid, global_pid);
	// target_memcg = get_mem_cgroup_from_pid(pid);
	target_memcg = get_mem_cgroup_from_pid(global_pid);

	memset(&unmap_filter_stats, 0, MYNODES*sizeof(struct unmap_filter_stat));
	// global_demote_last_val = global_demote = 0;
	memset(node_scan_freq, 0, sizeof(node_scan_freq));

	printk("sysctl_page_unmap_cold_threshold_sec = %ds", sysctl_page_unmap_cold_threshold_sec);
	printk("sysctl_page_unmap_hot_threshold_sec = %ds", sysctl_page_unmap_hot_threshold_sec);

	printk("sysctl_page_recently_accessed_threshold_sec = %ds", sysctl_page_recently_accessed_threshold_sec);

	for (i = 0; i < 4; i++) {
		printk("nid=%d, my_next_demotion_node[%d]=%d", i, i, my_next_demotion_node(i));
	}

	while (!kthread_should_stop()) {
		ktime_t start, diff;

		if (static_branch_likely(&static_key_kdemote_state)) {
			// if (scan_coldness_cnt++ == 20) { // 2s
			if (scan_coldness_cnt++ == 100) { // 10s
				scan_memcg_page_coldness(target_memcg);
				scan_coldness_cnt = 0;
			}

			if (dump_coldness_cnt++ == 400) {
				printk("------------begin------------");
				dump_all_node_histogram();
				dump_coldness_cnt = 0;
				// dump_whole_memory_layout(); // 该函数非常低效和耗时，只在探究gups程序内存分布情况时才调用

				dump_demote_histogram();
				dump_promote_events();
				dump_exchange_events();
				dump_node_stat();
				dump_migrate_stat();
				printk("------------end------------");
			}
		}

        start = ktime_get();
		change_memcg_page_prot(target_memcg);

		diff = ktime_sub(ktime_get(), start);
		kunmapd_overhead_ms += ktime_to_ms(diff);

		// if (global_demote - global_demote_last_val < 1000) {

		// }
		schedule_timeout_interruptible(sleep_timeout);
	}

	printk("kunmapd_overhead_ms=%ldms", kunmapd_overhead_ms);
	dump_node_stat();
	return 0;
}

static int kunmapd_run(pid_t pid)
{
    int err = 0;
	int cpu = 1;
	global_pid = pid;

    printk("kunmapd_run: pid=%d, global_pid=%d\n", pid, global_pid);
	// unmap_task = kthread_create_on_cpu(kunmapd, &pid, cpu, "kunmapd");
	unmap_task = kthread_create_on_cpu(kunmapd, &global_pid, cpu, "kunmapd");
	wake_up_process(unmap_task);

	if (IS_ERR(unmap_task)) {
		err = PTR_ERR(unmap_task);
		unmap_task = NULL;
		printk(KERN_ALERT "kunmapd_run: kthread create failed\n");
	}
    
    return err;
}

// 作为一个模块插入，可能会出现lru内部无页面？
// 暂时基于系统调用
// static int __init kunmapd_init(void)
int kunmapd_init(pid_t pid)
{
	if (!static_branch_likely(&static_key_kunmapd_state)) {
		printk("kunmapd_init: skip (disabled state)");
		return 0;
	}

	printk("kunmapd_init: ready to entery kunmapd_run\n");
	kunmapd_run(pid);
	printk("kunmapd_init: return\n");
    return 0;
}
// module_init(kunmapd_init)

void kunmapd_exit(void)
{
	if (!static_branch_likely(&static_key_kunmapd_state)) {
		printk("kunmapd_exit: skip (disabled state)");
		return ;
	}

    if (unmap_task) {
		printk("kunmapd_exit: ready to stop unmap_task: %px\n", unmap_task);
        kthread_stop(unmap_task);
        unmap_task = NULL;
    } else {
		printk("kunmapd_exit: skip (unmap_task is null)\n");
	}
	printk("kunmapd_exit: return\n");
}

#endif // CONFIG_PAGE_CHANGE_PROT