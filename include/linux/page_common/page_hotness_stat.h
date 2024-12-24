#ifndef __LINUX_PAGE_HOTNESS_STAT_H
#define __LINUX_PAGE_HOTNESS_STAT_H

#include <linux/types.h>
#include <linux/page_hotness/page_info.h>

#define MYNODES 4
#define STAT_HISTOGRAM_LENGTH 50

/************* most_access_node ***************/
extern unsigned long node_hint_page_faults[MY_MEM_NODES];
extern unsigned long max_hint_page_faults;
extern int most_access_node;

/************* promote alloc fail stat ***************/
extern unsigned long global_promote_fail_nomem;
extern unsigned long global_promote_fail_nomem_threshold;

/************* promote stat ***************/
enum promote_event {
    TRY_PROMOTE,
    PROMOTE_SUCCESS,
    PROMOTE_FAIL,
    NR_PROMOTE_EVENT,
};
extern const char *promote_event_name[NR_PROMOTE_EVENT];

struct promote_histogram {
    // 每个cpu上的event数目
    unsigned long sum;

    // 各个event的sum
    unsigned long event_sums[NR_PROMOTE_EVENT];

    // 各个直方图的sum
    unsigned long histogram_sums[NR_PROMOTE_EVENT][MYNODES][MYNODES];

    // fixme: MYNODES is hard code
    // 此处为四维数组: [event_type][src_node][dst_node][latency]
    // 但由于最外层还有一个percpu，因此逻辑上其实是五维数组
    unsigned long histograms[NR_PROMOTE_EVENT][MYNODES][MYNODES][STAT_HISTOGRAM_LENGTH];
};
extern struct promote_histogram promote_histogram;

extern int get_stat_histogram_binary_order(int num);

extern void count_promote_event(enum promote_event type, int src_nid, int target_nid, int latency_sec);
extern unsigned long get_cpu_promote_count(int cpu, enum promote_event type, int src_nid, int target_nid, int latency_sec);
extern unsigned long get_cpu_promote_sum(int cpu);

void dump_histogram(unsigned long node_histograms[]);
void dump_promote_events(void);

/************* exchange stat ***************/
enum exchange_event {
    TRY_EXCHANGE,
    EXCHANGE_SUCCESS,
    EXCHANGE_ONLY_PROMOTE_SUCCESS,
    EXCHANGE_ONLY_DEMOTE_SUCCESS,
    EXCHANGE_FAIL,
    NR_EXCHANGE_EVENT,
};
extern const char *exchange_event_name[NR_EXCHANGE_EVENT];

struct exchange_histogram {
    // 每个cpu上的event数目
    unsigned long sum;

    // 各个event的sum
    unsigned long event_sums[NR_EXCHANGE_EVENT];

    // 各个直方图的sum
    unsigned long histogram_sums[NR_EXCHANGE_EVENT][MYNODES][MYNODES];

    // fixme: MYNODES is hard code
    // 此处为四维数组: [event_type][src_node][dst_node][latency]
    // 但由于最外层还有一个percpu，因此逻辑上其实是五维数组
    unsigned long upper_histograms[NR_EXCHANGE_EVENT][MYNODES][MYNODES][STAT_HISTOGRAM_LENGTH];
    unsigned long lower_histograms[NR_EXCHANGE_EVENT][MYNODES][MYNODES][STAT_HISTOGRAM_LENGTH];
};
extern struct promote_histogram promote_histogram;

extern void count_exchange_event(enum exchange_event type, int src_nid, int target_nid, int upper_tier_latency_sec, int lower_tier_latency_sec);
void dump_exchange_events(void);

/************* migrate stat ***************/
extern void dump_migrate_stat(void);

struct migrate_misplaced_page_stat {
    unsigned long all;
    unsigned long not_page_lru;
    unsigned long mapcount_one_and_file_lru;
    unsigned long dirty_and_file_lru;
    unsigned long isolate_fail;
    unsigned long migrate_fail;
    unsigned long migrate_success;
};
extern struct migrate_misplaced_page_stat migrate_misplaced_page_stat;

struct autonuma_migrate_page {
    unsigned long all;
    unsigned long migrate_success;
};
extern struct autonuma_migrate_page autonuma_migrate_page;

#endif /* __LINUX_PAGE_HOTNESS_STAT_H */