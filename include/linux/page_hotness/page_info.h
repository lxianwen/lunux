#ifndef __LINUX_PAGE_INFO_H
#define __LINUX_PAGE_INFO_H

#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched/sysctl.h>
#include <linux/types.h>
#include <linux/page_ext.h>

#define MY_MEM_NODES 4
typedef uint16_t counter_t;
#define MY_COUNTER_MAX 65535

struct page_info {
	spinlock_t pi_lock;

	// todo：这两个32bit的数其实可以合成一个64bit的数字存储，来实现原子性
	// todo：该字段后续可以放到page->flags中，参考tiering
	unsigned int pi_last_time; // unmap的时间，用于计算时间间隔

// #ifdef CONFIG_ACCESS_HISTOGRAM
	unsigned int access_interval_sec; // 用于直方图
// #endif

#ifdef CONFIG_CANDIDATE_PAGE
	// todo: 此部分其实可以使用集合或者直接使用page->lru来实现，来减少page_info开销
	struct page* page; // 为了遍历pi链表时，能够反向找到page
	struct list_head candidate_page_node; // pi链表
#endif
};

#ifdef CONFIG_PAGE_HOTNESS
extern struct page_ext_operations page_info_ops;

extern struct page_info *get_page_info(struct page_ext *page_ext);
extern struct page_info *get_page_info_from_page(struct page *page);
extern struct page_ext *get_page_ext(struct page_info *page_info);
extern void init_page_info(struct page *page, unsigned int order);

extern unsigned int __get_page_access_counter(struct page_info *pi, int cpu_id);
extern void mod_page_access_counter(struct page *page, unsigned int accessed, int cpu_id);
extern void clear_page_info(struct page *page);
extern void reset_page_info(struct page *page, unsigned int order);
extern void reset_page_last_access(struct page *page);

extern void copy_page_info(struct page *oldpage, struct page *newpage);

extern unsigned int get_numa_latency_ms(struct page *page, bool locked);
extern unsigned int xchg_page_access_time(struct page *page, unsigned int time);

extern bool in_page_unmap_state(struct page* page);
extern void set_page_unmap_state(struct page* page);

extern bool in_page_migrating_state(struct page* page);
extern void set_page_migrating_state(struct page* page);
extern void clear_page_migrating_state(struct page* page);

#else // !CONFIG_PAGE_HOTNESS
static inline struct page_info *get_page_info(struct page_ext *page_ext) { return NULL; }
static inline struct page_info *get_page_info_from_page(struct page *page) { return NULL; }
static inline struct page_ext *get_page_ext(struct page_info *page_info) { return NULL; }
static inline void init_page_info(struct page *page, unsigned int order);

static inline unsigned int __get_page_access_counter(struct page_info *pi, int cpu_id) { return 0; }
static inline void mod_page_access_counter(struct page *page, unsigned int accessed, int cpu_id) { return ; }
static inline void clear_page_info(struct page *page) { }
static inline void reset_page_info(struct page *page, unsigned int order) { }

static inline void reset_page_last_access(struct page *page) { }

static inline void copy_page_info(struct page *oldpage, struct page *newpage) { return ; }

static inline unsigned int get_numa_latency_ms(struct page *page) { return 0; }
static inline unsigned int get_and_update_numa_latency_ms(struct page *page, bool locked) { return 0; }
static inline unsigned int xchg_page_access_time(struct page *page, unsigned int time) { return 0; }

static inline bool in_page_unmap_state(struct page* page) { return false; }
static inline void set_page_unmap_state(struct page* page) {}

static inline bool in_page_migrating_state(struct page* page) { return false; }
static inline void set_page_migrating_state(struct page* page) { }
static inline void clear_page_migrating_state(struct page* page) { }
#endif // CONFIG_PAGE_HOTNESS

/************************** flags **********************************/
#ifdef CONFIG_PAGE_HOTNESS
extern int PageTracked(struct page_ext *page_ext);
extern void SetPageTracked(struct page_ext *page_ext);
extern void ClearPageTracked(struct page_ext *page_ext);

extern int PageUnmapLRU(struct page_ext *page_ext);
extern void SetPageUnmapLRU(struct page_ext *page_ext);
extern void ClearPageUnmapLRU(struct page_ext *page_ext);
extern bool TestAndClearPageUnmapLRU(struct page_ext *page_ext);

extern int PageRecentlyAccessed(struct page_ext *page_ext);
extern void SetPageRecentlyAccessed(struct page_ext *page_ext);
extern void ClearPageRecentlyAccessed(struct page_ext *page_ext);

extern int PageMigrating(struct page_ext *page_ext);
extern void SetPageMigrating(struct page_ext *page_ext);
extern void ClearPageMigrating(struct page_ext *page_ext);
extern bool TestAndSetPageMigrating(struct page_ext *page_ext);

// 对应access_interval_sec
extern int PageHistogram(struct page_ext *page_ext);
extern void SetPageHistogram(struct page_ext *page_ext);
extern void ClearPageHistogram(struct page_ext *page_ext);
extern bool TestAndSetPageHistogram(struct page_ext *page_ext);

// 对应pi_last_time
extern int PageUnmapTime(struct page_ext *page_ext);
extern void SetPageUnmapTime(struct page_ext *page_ext);
extern void ClearPageUnmapTime(struct page_ext *page_ext);
extern bool TestAndSetPageUnmapTime(struct page_ext *page_ext);

// 对应candidate_page_node
extern int PageCandidate(struct page_ext *page_ext);
extern void SetPageCandidate(struct page_ext *page_ext);
extern void ClearPageCandidate(struct page_ext *page_ext);
extern bool TestAndSetPageCandidate(struct page_ext *page_ext);

#else // !CONFIG_PAGE_HOTNESS
static inline int PageTracked(struct page_ext *page_ext) { return 0; }
static inline void SetPageTracked(struct page_ext *page_ext) { }
static inline void ClearPageTracked(struct page_ext *page_ext) { }

static inline int PageUnmapLRU(struct page_ext *page_ext) { return 0; }
static inline void SetPageUnmapLRU(struct page_ext *page_ext) {  }
static inline void ClearPageUnmapLRU(struct page_ext *page_ext) {  }
static inline bool TestAndClearPageUnmapLRU(struct page_ext *page_ext) { return false; }

static inline int PageRecentlyAccessed(struct page_ext *page_ext) { return 0; }
static inline void SetPageRecentlyAccessed(struct page_ext *page_ext) {  }
static inline void ClearPageRecentlyAccessed(struct page_ext *page_ext) {  }
static inline bool TestAndClearPageRecentlyAccessed(struct page_ext *page_ext) { return false; }

static inline int PageMigrating(struct page_ext *page_ext) { return 0; }
static inline void SetPageMigrating(struct page_ext *page_ext) { }
static inline void ClearPageMigrating(struct page_ext *page_ext) { }
static inline bool TestAndSetPageMigrating(struct page_ext *page_ext) { return false; }

static inline int PageHistogram(struct page_ext *page_ext) { return 0; }
static inline void SetPageHistogram(struct page_ext *page_ext) { }
static inline void ClearPageHistogram(struct page_ext *page_ext) { }
static inline bool TestAndSetPageHistogram(struct page_ext *page_ext) { return false; }

static inline int PageUnmapTime(struct page_ext *page_ext) { return 0; }
static inline void SetPageUnmapTime(struct page_ext *page_ext) { }
static inline void ClearPageUnmapTime(struct page_ext *page_ext) { }
static inline bool TestAndSetPageUnmapTime(struct page_ext *page_ext) { return false; }

static inline int PageCandidate(struct page_ext *page_ext) { return 0; }
static inline void SetPageCandidate(struct page_ext *page_ext) {}
static inline void ClearPageCandidate(struct page_ext *page_ext) {}
static inline bool TestAndSetPageCandidate(struct page_ext *page_ext) { return false; }
#endif // end of CONFIG_PAGE_HOTNESS

/************************** page_recently_accessed **********************************/
#ifdef CONFIG_PAGE_RECENTLY_ACCESSED_FLAG
extern bool page_recently_accessed(struct page* page);
#else // !CONFIG_PAGE_RECENTLY_ACCESSED_FLAG
static inline bool page_recently_accessed(struct page* page) { return false; }
#endif // end of CONFIG_PAGE_RECENTLY_ACCESSED_FLAG

/************************** page_exchange **********************************/
#ifdef CONFIG_PAGE_EXCHANGE
extern void exchange_page_info(struct page *from_page, struct page *to_page);
#else
static inline void exchange_page_info(struct page *from_page, struct page *to_page) {}
#endif


#endif /* __LINUX_PAGE_INFO_H */