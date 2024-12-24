#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/uaccess.h>
#include <linux/jump_label.h>
#include <linux/memcontrol.h>
#include <linux/node.h>

// lmy
#include <linux/page_hotness/page_info.h>
#include <linux/page_owner.h>
#include <linux/page_ext.h>
#include <linux/page_hotness/page_hotness.h>
#include <linux/page_hotness/candidate_page.h>
#include <linux/page_common/page_hotness_stat.h>
#include <linux/page_common/page_hotness_sysctl.h>
#include <linux/printk.h>
#include <stdbool.h>

#include "../internal.h"

#ifdef CONFIG_PAGE_HOTNESS
struct page_info *get_page_info(struct page_ext *page_ext)
{
	return (void *)page_ext + page_info_ops.offset;
}

struct page_ext *get_page_ext(struct page_info *page_info)
{
	return (void *)page_info - page_info_ops.offset;
}

struct page_info *get_page_info_from_page(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (!page_ext)
		return NULL;
	return get_page_info(page_ext);
}

static inline void __init_page_info(struct page_ext *page_ext,
				unsigned int order)
{
	struct page_info *pi = NULL;
	int i;

	if (PageTracked(page_ext)) {
		// 说明这个page_info已经被设置过了
		return ;
	}

	for (i = 0; i < (1 << order); i++) {
		pi = get_page_info(page_ext);

		spin_lock_init(&pi->pi_lock);
		SetPageTracked(page_ext);

		ClearPageUnmapLRU(page_ext);
		ClearPageRecentlyAccessed(page_ext);
		ClearPageUnmapTime(page_ext); // pi_last_time
		ClearPageMigrating(page_ext);
		ClearPageHistogram(page_ext); // access_interval_sec

		pi->pi_last_time = 0;

#ifdef CONFIG_ACCESS_HISTOGRAM
		pi->access_interval_sec = 0;
#endif

#ifdef CONFIG_CANDIDATE_PAGE
		ClearPageCandidate(page_ext);
		INIT_LIST_HEAD(&pi->candidate_page_node);
#endif
	}

}

void init_page_info(struct page *page, unsigned int order)
{
	struct page_ext *page_ext = lookup_page_ext(page);

	if (unlikely(!page_ext))
		return;
	
	__init_page_info(page_ext, order);
}

static void init_pages_in_zone(pg_data_t *pgdat, struct zone *zone)
{
	unsigned long pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count = 0;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	for (; pfn < end_pfn; ) {
		unsigned long block_end_pfn;

		if (!pfn_valid(pfn)) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);
		block_end_pfn = min(block_end_pfn, end_pfn);

		for (; pfn < block_end_pfn; pfn++) {
			struct page *page = pfn_to_page(pfn);
			struct page_ext *page_ext;
			
			if (page_zone(page) != zone)
				continue;

			/*
			 * To avoid having to grab zone->lock, be a little
			 * careful when reading buddy page order. The only
			 * danger is that we skip too much and potentially miss
			 * some early allocated pages, which is better than
			 * heavy lock contention.
			 */
			if (PageBuddy(page)) {
				unsigned long order = buddy_order_unsafe(page);

				if (order > 0 && order < MAX_ORDER)
					pfn += (1UL << order) - 1;
				continue;
			}

			if (PageReserved(page))
				continue;

			// init page info
			page_ext = lookup_page_ext(page);
			if (page_ext) {
				__init_page_info(page_ext, 0);
			}

			count++;
		}
		cond_resched();
	}

	pr_info("Node %d, zone %8s: page owner found early allocated %lu pages\n",
		pgdat->node_id, zone->name, count);
}

static void init_zones_in_node(pg_data_t *pgdat)
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		init_pages_in_zone(pgdat, zone);
	}
}

static void init_early_allocated_pages(void)
{
	pg_data_t *pgdat;

	for_each_online_pgdat(pgdat)
		init_zones_in_node(pgdat);
}

static void init_page_balancing(void)
{
	init_early_allocated_pages();
	printk("init_page_balancing\n");
}

static bool need_page_balancing(void)
{
	return true;
}

struct page_ext_operations page_info_ops = {
	.size = sizeof(struct page_info),
	.need = need_page_balancing,
	.init = init_page_balancing,
};

void reset_single_page_info(struct page_ext *page_ext)
{
	struct page_info *pi = NULL;
	unsigned long irq_flags;

	pi = get_page_info(page_ext);

	if (unlikely(!PageTracked(page_ext))) {
		spin_lock_init(&pi->pi_lock);
		SetPageTracked(page_ext);
	}

#ifdef CONFIG_ACCESS_HISTOGRAM
	// 对应access_interval_sec
	if (PageHistogram(page_ext)) {
		access_histogram_delete(pi->page);
	}
#endif

#ifdef CONFIG_CANDIDATE_PAGE
	// 对应candidate_page_node
	if (PageCandidate(page_ext)) {
		del_candidate_page(pi->page);
	}
#endif

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	// PageTracked无需clear，因为PageTracked的含义是是否存在page_info, 后续都不会用到
	SetPageTracked(page_ext);
	
	// pi_last_time
	ClearPageUnmapLRU(page_ext);
	ClearPageRecentlyAccessed(page_ext);
	ClearPageUnmapTime(page_ext); // pi_last_time
	pi->pi_last_time = 0;

	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);
}

void reset_page_info(struct page *page, unsigned int order)
{
	int i;
	struct page_ext *page_ext = NULL;

	page_ext = lookup_page_ext(page);
	// 系统刚启动的时候，这里的page_ext可能为NULL，不进行判断的话，会导致系统无法正常启动
	if (unlikely(!page_ext))
		return;

	for (i = 0; i < (1 << order); i++) {
		reset_single_page_info(page_ext);
		page_ext = page_ext_next(page_ext);
	}
}

void copy_page_info(struct page *old_page, struct page *new_page)
{
	struct page_ext *old_ext, *new_ext;
	struct page_info *old_pi, *new_pi;
	struct page_info *first_pi, *second_pi;
	unsigned long irq_flags;

	old_ext = lookup_page_ext(old_page);
	new_ext = lookup_page_ext(new_page);
	if (unlikely(!old_ext || !new_ext))
		return ;

	if (!PageTracked(old_ext) || !PageTracked(new_ext)) {
		return ;
	}

	old_pi = get_page_info(old_ext);
	new_pi = get_page_info(new_ext);

#ifdef CONFIG_ACCESS_HISTOGRAM
	// 先拷贝直方图信息
	// access_interval_sec字段和PageHistogram flag
	access_histogram_copy(old_page, new_page);
#endif

#ifdef CONFIG_CANDIDATE_PAGE
	// 如果旧页位于candidate_page链表中，就将其移除
	if (in_candidate_pages(old_page))
		del_candidate_page(old_page);
#endif

	// 为了避免死锁，这里定义锁的顺序：根据指针地址，优先对低地址进行加锁
	if (old_pi < new_pi) {
		first_pi = old_pi;
		second_pi = new_pi;
	} else {
		first_pi = new_pi;
		second_pi = old_pi;
	}

	// 虽然这种情况一般不会出现，但是以防万一，不然后面会死锁
	if (first_pi == second_pi) {
		return ;
	}

	// 再处理其他信息
	spin_lock_irqsave_nested(&first_pi->pi_lock, irq_flags, 1);
	spin_lock_nested(&second_pi->pi_lock, 0);

	ClearPageMigrating(new_ext);

#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG
	if (PageUnmapLRU(old_ext) && PageRecentlyAccessed(old_ext)) {
		printk("lmy-warn: PageUnmapLRU and PageRecentlyAccessed are set in the same time");
	}
	new_pi->pi_last_time = old_pi->pi_last_time;
	if (PageUnmapLRU(old_ext))
		SetPageUnmapLRU(new_ext);
	if (PageRecentlyAccessed(old_ext))
		SetPageRecentlyAccessed(new_ext);
	if (PageUnmapTime(old_ext))
		SetPageUnmapTime(new_ext);

	ClearPageUnmapLRU(old_ext);
	ClearPageUnmapTime(old_ext);
	ClearPageRecentlyAccessed(old_ext);
	old_pi->pi_last_time = 0;
#else
	// pi_last_time字段
	// 重置old page的这些字段
	ClearPageUnmapLRU(old_ext);
	ClearPageUnmapTime(old_ext);
	old_pi->pi_last_time = 0;

	// 重置new page的这些字段（这些字段是unmap状态下才有意义）
	ClearPageUnmapLRU(new_ext);
	ClearPageUnmapTime(new_ext);
	new_pi->pi_last_time = 0;
#endif

	// histogram在前面处理过了
	// candidate_list在前面处理过了

	spin_unlock(&second_pi->pi_lock);
	spin_unlock_irqrestore(&first_pi->pi_lock, irq_flags);
}

#ifdef CONFIG_PAGE_EXCHANGE
void exchange_page_info(struct page *from_page, struct page *to_page)
{
	struct page_ext *from_ext, *to_ext;
	struct page_info *from_pi, *to_pi;
	struct page_info *first_pi, *second_pi;
	struct page_info tmp_pi;
	unsigned long irq_flags;

	from_ext = lookup_page_ext(from_page);
	to_ext = lookup_page_ext(to_page);
	if (unlikely(!from_ext || !to_ext))
		return;

	// todo: 后面放到迁移一开始的地方
	SetPageMigrating(from_ext);
	SetPageMigrating(to_ext);

	from_pi = get_page_info(from_ext);
	to_pi = get_page_info(to_ext);

	if (from_pi < second_pi) {
		first_pi = from_pi;
		second_pi = to_pi;
	} else {
		first_pi = to_pi;
		second_pi = from_pi;
	}

#ifdef CONFIG_ACCESS_HISTOGRAM
	// 先交换直方图信息
	access_histogram_exchange(from_page, to_page);
#endif

	spin_lock_irqsave_nested(&first_pi->pi_lock, irq_flags, 1);
	spin_lock_nested(&second_pi->pi_lock, 0);

	/*
	 * todo: PageRecentlyAccessed flag的exchange操作
	*/

	// 热度信息（直方图信息）
	ClearPageUnmapLRU(from_ext);
	ClearPageUnmapTime(from_ext);

	ClearPageUnmapLRU(to_ext);
	ClearPageUnmapTime(to_ext);

	from_pi->pi_last_time = 0;
	to_pi->pi_last_time = 0;

	// Migrating flag
	ClearPageMigrating(from_ext);
	ClearPageMigrating(to_ext);

	// cold_page_list
	// 在exchange之前就应该将to_page从candidate_page中移出来
	spin_unlock(&second_pi->pi_lock);
	spin_unlock_irqrestore(&first_pi->pi_lock, irq_flags);
	smp_mb__after_unlock_lock();
}
#endif // CONFIG_PAGE_EXCHANGE

const unsigned int INF = 0x3f3f3f3f;
// 仅计算时间间隔
unsigned int get_numa_latency_ms(struct page *page, bool locked)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	struct page_info *pi = get_page_info(page_ext);
	unsigned int time = jiffies_to_msecs(jiffies);
	unsigned int pi_last_time; // ms
	unsigned int ret_diff_time;
	unsigned long irq_flags;

	if (!PageTracked(page_ext))
		goto warn_out;

	if (!locked)
		spin_lock_irqsave(&pi->pi_lock, irq_flags);

	if (!PageUnmapTime(page_ext)) {
		printk("lmy-warn: get_numa_latency_ms: !PageUnmapTime(page_ext)");
		goto warn_out;
	}

	pi_last_time = pi->pi_last_time; // 这个字段应该在锁内部获取
	if (pi_last_time == 0) {
		printk("lmy-warn: get_numa_latency_ms: pi_last_time == 0");
		goto warn_out;
	}

	ret_diff_time = time - pi_last_time;

unlock:
	if (!locked)
		spin_unlock_irqrestore(&pi->pi_lock, irq_flags);
	return ret_diff_time;

warn_out:
	ret_diff_time = INF;
	goto unlock;
}

// 替换pi->last_time为time，并返回last_time
unsigned int xchg_page_access_time(struct page *page, unsigned int time)
{
	unsigned int pi_last_time;
	struct page_info *pi = get_page_info_from_page(page);
	WARN_ON(!pi);

	do {
		pi_last_time = pi->pi_last_time;
	} while (unlikely(cmpxchg(&pi->pi_last_time, pi_last_time, time) != pi_last_time));

	return pi_last_time;
}

bool in_page_unmap_state(struct page* page)
{
	struct page_ext *page_ext;
	struct page_info *pi;
	bool rc = false;
	unsigned long irq_flags;

	page_ext = lookup_page_ext(page);
	if (!PageTracked(page_ext)) {
		// printk("in_page_unmap_state: !PageTracked, nid=%d", page_to_nid(page));
		return false;
	}

	pi = get_page_info_from_page(page);
	WARN_ON(!pi);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	rc = PageUnmapLRU(page_ext)? true : false;
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

	return rc;
}

void set_page_unmap_state(struct page* page)
{
	struct page_info *pi;
	struct page_ext *page_ext;
	unsigned long irq_flags;

	page_ext = lookup_page_ext(page);
	if (!page_ext || !PageTracked(page_ext))
		return ;
	pi = get_page_info(page_ext);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	// 设置时间戳，以及两个flag
	// xchg_page_access_time(page,	jiffies_to_msecs(jiffies));
	pi->pi_last_time = jiffies_to_msecs(jiffies);
	barrier();
	SetPageUnmapTime(page_ext);
	SetPageUnmapLRU(page_ext);

#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG
	// reset RecentlyAccessed flag
	if (PageRecentlyAccessed(page_ext))
		ClearPageRecentlyAccessed(page_ext);
#endif

	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);
	smp_mb__after_unlock_lock();

	return ;
}

bool page_recently_accessed(struct page* page)
{
	struct page_ext *page_ext;
	struct page_info *pi;
	bool rc = false;
	unsigned long irq_flags;

	unsigned int latency_ms, latency_sec;

	page_ext = lookup_page_ext(page);
	if (!PageTracked(page_ext)) {
		// printk("page_recently_accessed: !PageTracked, nid=%d", page_to_nid(page));
		return false;
	}

	pi = get_page_info_from_page(page);
	WARN_ON(!pi);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);

	// 1. 检查flag
	if (!PageRecentlyAccessed(page_ext)) {
		goto unlock;
	}

	// 2. 检查时间间隔
	if (!PageUnmapTime(page_ext)) {
		printk("PageRecentlyAccessed is set, but PageUnmapTime is not set");
		goto unlock;
	}
	latency_ms = get_numa_latency_ms(page, true);
	latency_sec = latency_ms / 1000;
	if (latency_sec > sysctl_page_recently_accessed_threshold_sec) {
		// 超出阈值，即距离页面上次unmap已经过去很久了
		// 重置flag和时间戳
		ClearPageRecentlyAccessed(page_ext);
		ClearPageUnmapTime(page_ext);
		pi->pi_last_time = 0;
		goto unlock;
	}

	// 3. set true
	rc = true;

unlock:
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

	return rc;
}

bool in_page_migrating_state(struct page* page)
{
	struct page_ext *page_ext;
	bool rc = false;
	unsigned long irq_flags;
	struct page_info *pi;

	page_ext = lookup_page_ext(page);
	if (!page_ext || !PageTracked(page_ext))
		return rc;

	pi = get_page_info(page_ext);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	rc = PageMigrating(page_ext)? true : false;
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

	return rc;
}

void set_page_migrating_state(struct page* page)
{
	struct page_ext *page_ext;
	unsigned long irq_flags;
	struct page_info *pi;

	page_ext = lookup_page_ext(page);
	if (!page_ext || !PageTracked(page_ext))
		return ;
	pi = get_page_info(page_ext);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	SetPageMigrating(page_ext);
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);
	return ;
}

void clear_page_migrating_state(struct page* page)
{
	struct page_ext *page_ext;
	unsigned long irq_flags;
	struct page_info *pi;

	page_ext = lookup_page_ext(page);
	if (!page_ext || !PageTracked(page_ext))
		return ;
	pi = get_page_info(page_ext);

	spin_lock_irqsave(&pi->pi_lock, irq_flags);
	ClearPageMigrating(lookup_page_ext(page));
	spin_unlock_irqrestore(&pi->pi_lock, irq_flags);
	return ;
}


// clear_bit是原子操作，__clear_bit是非原子操作

// PAGE_EXT_TRACKED
inline int PageTracked(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

inline void SetPageTracked(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

inline void ClearPageTracked(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

// PAGE_EXT_UNMAP_LRU
inline int PageUnmapLRU(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_UNMAP_LRU, &page_ext->flags);
}

inline void SetPageUnmapLRU(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_UNMAP_LRU, &page_ext->flags);
}

inline void ClearPageUnmapLRU(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_UNMAP_LRU, &page_ext->flags);
}

inline bool TestAndClearPageUnmapLRU(struct page_ext *page_ext)
{
	return test_and_clear_bit(PAGE_EXT_UNMAP_LRU, &page_ext->flags);
}

// PAGE_EXT_RECENTLY_ACCESSED
inline int PageRecentlyAccessed(struct page_ext *page_ext)
{
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG	
	return test_bit(PAGE_EXT_RECENTLY_ACCESSED, &page_ext->flags);
#else
	return 0;
#endif
}

inline void SetPageRecentlyAccessed(struct page_ext *page_ext)
{
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG	
	return set_bit(PAGE_EXT_RECENTLY_ACCESSED, &page_ext->flags);
#else
	return ;
#endif
}

inline void ClearPageRecentlyAccessed(struct page_ext *page_ext)
{
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG	
	clear_bit(PAGE_EXT_RECENTLY_ACCESSED, &page_ext->flags);
#else
	return ;
#endif
}

inline bool TestAndClearPageRecentlyAccessed(struct page_ext *page_ext)
{
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG	
	return test_and_clear_bit(PAGE_EXT_RECENTLY_ACCESSED, &page_ext->flags);
#else
	return ;
#endif
}

// PAGE_EXT_MIGRATING
inline int PageMigrating(struct page_ext *page_ext)
{
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG	
	return test_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
#else
	return ;
#endif
}

inline void SetPageMigrating(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}

inline void ClearPageMigrating(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}

inline bool TestAndSetPageMigrating(struct page_ext *page_ext)
{
	return test_and_set_bit(PAGE_EXT_MIGRATING, &page_ext->flags);
}

// PAGE_EXT_HISTOGRAM
inline int PageHistogram(struct page_ext *page_ext)
{
#ifdef CONFIG_ACCESS_HISTOGRAM
	return test_bit(PAGE_EXT_HISTOGRAM, &page_ext->flags);
#else
	return 0;
#endif
}

inline void SetPageHistogram(struct page_ext *page_ext)
{
#ifdef CONFIG_ACCESS_HISTOGRAM
	return set_bit(PAGE_EXT_HISTOGRAM, &page_ext->flags);
#else
	return ;
#endif
}

inline void ClearPageHistogram(struct page_ext *page_ext)
{
#ifdef CONFIG_ACCESS_HISTOGRAM
	clear_bit(PAGE_EXT_HISTOGRAM, &page_ext->flags);
#else
	return ;
#endif
}

inline bool TestAndSetPageHistogram(struct page_ext *page_ext)
{
#ifdef CONFIG_ACCESS_HISTOGRAM
	return test_and_set_bit(PAGE_EXT_HISTOGRAM, &page_ext->flags);
#else
	return false;
#endif
}

// PAGE_EXT_UNMAPTIME
inline int PageUnmapTime(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_UNMAPTIME, &page_ext->flags);
}

inline void SetPageUnmapTime(struct page_ext *page_ext)
{
	return set_bit(PAGE_EXT_UNMAPTIME, &page_ext->flags);
}

inline void ClearPageUnmapTime(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_UNMAPTIME, &page_ext->flags);
}

inline bool TestAndSetPageUnmapTime(struct page_ext *page_ext)
{
	return test_and_set_bit(PAGE_EXT_UNMAPTIME, &page_ext->flags);
}

// PAGE_EXT_CANDIDATE
inline int PageCandidate(struct page_ext *page_ext)
{
#ifdef CONFIG_CANDIDATE_PAGE
	return test_bit(PAGE_EXT_CANDIDATE, &page_ext->flags);
#else
	return 0;
#endif
}

inline void SetPageCandidate(struct page_ext *page_ext)
{
#ifdef CONFIG_CANDIDATE_PAGE
	return set_bit(PAGE_EXT_CANDIDATE, &page_ext->flags);
#else
	return ;
#endif
}

inline void ClearPageCandidate(struct page_ext *page_ext)
{
#ifdef CONFIG_CANDIDATE_PAGE
	clear_bit(PAGE_EXT_CANDIDATE, &page_ext->flags);
#else
	return ;
#endif
}

inline bool TestAndSetPageCandidate(struct page_ext *page_ext)
{
#ifdef CONFIG_CANDIDATE_PAGE
	return test_and_set_bit(PAGE_EXT_CANDIDATE, &page_ext->flags);
#else
	return false;
#endif
}

#endif