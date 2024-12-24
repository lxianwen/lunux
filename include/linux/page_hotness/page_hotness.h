/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_HOTNESS_H
#define __LINUX_PAGE_HOTNESS_H

#include <linux/types.h>
#include <linux/stacktrace.h>
#include <linux/stackdepot.h>
#include <linux/page_hotness/page_info.h>

// change_active_list
#ifdef CONFIG_PAGE_CHANGE_PROT
extern int my_next_demotion_node(int nid);
extern int my_second_demotion_node(int nid);
#else
static inline int my_next_demotion_node(int nid) { return -1; }
static inline int my_second_demotion_node(int nid) { return -1; }
#endif

#ifdef CONFIG_PAGE_CHANGE_PROT
extern int kunmapd_init(pid_t pid);
extern void kunmapd_exit(void);
#else
static inline int kunmapd_init(pid_t pid);
static inline void kunmapd_exit(void);
#endif

#endif /* __LINUX_PAGE_HOTNESS_H */
