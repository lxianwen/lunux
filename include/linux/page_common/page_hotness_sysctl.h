#ifndef __LINUX_PAGE_HOTNESS_SYSCTL_H
#define __LINUX_PAGE_HOTNESS_SYSCTL_H

#include <linux/sysctl.h>
#include <linux/printk.h>

extern struct static_key_false static_key_autonuma_state;
extern struct static_key_true static_key_kunmapd_state;
extern struct static_key_true static_key_kdemote_state;
extern struct static_key_false static_key_gups_hot_enable_state;

extern int sysctl_page_hotness_threshold;
extern int sysctl_page_hotness_debug;

// extern unsigned long dram_scan_interval;

extern unsigned int sysctl_page_unmap_hot_threshold_sec; // s
extern unsigned int sysctl_page_unmap_cold_threshold_sec; // s
extern unsigned int sysctl_page_lantency_histogram_max_sec; // s

extern unsigned int sysctl_page_recently_accessed_threshold_sec;
extern int sysctl_page_hotness_init(void);
extern void sysctl_page_hotness_exit(void);

#endif /* __LINUX_PAGE_HOTNESS_SYSCTL_H */