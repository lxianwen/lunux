/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_HOTNESS_API_H
#define __LINUX_PAGE_HOTNESS_API_H

#include <linux/types.h>
#include <linux/stacktrace.h>
#include <linux/stackdepot.h>
#include <linux/page_hotness/page_info.h>

#ifdef CONFIG_PAGE_HOTNESS
extern bool pgdat_free_space_enough(struct pglist_data *pgdat);
extern bool lowertier_pgdat_free_space_enough(struct pglist_data *pgdat);
extern bool reach_static_hot_threshold(unsigned long latency_sec);
extern bool reach_dynamic_hot_threshold(struct base_histogram *access_histogram, unsigned long latency_sec);
#else // !CONFIG_PAGE_HOTNESS
static inline bool pgdat_free_space_enough(struct pglist_data *pgdat) { return true; }
static inline bool lowertier_pgdat_free_space_enough(struct pglist_data *pgdat) { return true; }
static inline bool reach_static_hot_threshold(unsigned long latency_sec) { return false; }
static inline bool reach_dynamic_hot_threshold(struct base_histogram *access_histogram, unsigned long latency_sec) { return false; }
#endif // CONFIG_PAGE_HOTNESS

#endif /* __LINUX_PAGE_HOTNESS_API_H */
