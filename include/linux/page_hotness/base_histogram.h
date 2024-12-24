#ifndef __LINUX_PAGE_COMMON_BASE_HISTOGRAM_H
#define __LINUX_PAGE_COMMON_BASE_HISTOGRAM_H

#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/page_hotness/page_info.h>

#define BASE_HISTOGRAM_LENGTH 50

enum histogram_type {
    NORMAL_COUNT,
    ORDER_COUNT,
};

struct base_histogram {
    spinlock_t histogram_lock;
    int nid;
    enum histogram_type type;
    int len; // histogram数组的长度
    unsigned long sum; // histogram数组中所有cnt之和
    unsigned long histogram[BASE_HISTOGRAM_LENGTH];
};

#ifdef CONFIG_ACCESS_HISTOGRAM
extern void init_base_histogram(struct base_histogram *base_histogram, int len, enum histogram_type type, int nid);
extern void reset_base_histogram(struct base_histogram *base_histogram);
// extern bool base_histogram_update(struct base_histogram *base_histogram, unsigned long old_val, unsigned long new_val);
// extern bool base_histogram_insert(struct base_histogram *base_histogram, unsigned long new_val);
// extern void base_histogram_delete(struct base_histogram *base_histogram, unsigned long old_val);
extern unsigned long base_histogram_get_prefix_sum(struct base_histogram *base_histogram, unsigned long test_val);
extern void base_histogram_dump(const char *name, struct base_histogram *base_histogram, bool locked);
extern bool in_access_histogram(struct page* page);
extern int get_access_histogram_sum(struct page* page);
extern void access_histogram_insert_or_update(struct page* page, unsigned long latency_sec, bool locked);
extern void access_histogram_delete(struct page* page);
extern void access_histogram_copy(struct page* old_page, struct page* new_page);
extern void access_histogram_exchange(struct page* from_page, struct page* to_page);
#else // !CONFIG_ACCESS_HISTOGRAM
static inline void init_base_histogram(struct base_histogram *base_histogram, int len, enum histogram_type type, int nid) { }
static inline void reset_base_histogram(struct base_histogram *base_histogram) { }
// static inline bool base_histogram_update(struct base_histogram *base_histogram, unsigned long old_val, unsigned long new_val) { return false; }
// static inline bool base_histogram_insert(struct base_histogram *base_histogram, unsigned long new_val) { return false; }
// static inline void base_histogram_delete(struct base_histogram *base_histogram, unsigned long old_val) { return ; }
static inline unsigned long base_histogram_get_prefix_sum(struct base_histogram *base_histogram, unsigned long test_val) { return 0; }
static inline void base_histogram_dump(const char *name, struct base_histogram *base_histogram, bool locked) { }
static inline bool in_access_histogram(struct page* page) { return false; }
static inline int get_access_histogram_sum(struct page* page) { return 0; }
static inline void access_histogram_insert_or_update(struct page* page, unsigned long latency_sec, bool locked) { }
static inline void access_histogram_delete(struct page* page) { }
static inline void access_histogram_copy(struct page* old_page, struct page* new_page) { }
static inline void access_histogram_exchange(struct page* from_page, struct page* to_page) { }
#endif // CONFIG_ACCESS_HISTOGRAM

#endif // __LINUX_PAGE_COMMON_BASE_HISTOGRAM_H