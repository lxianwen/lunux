#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/page_hotness/page_hotness.h>
#include <linux/page_hotness/page_info.h>
#include <linux/nodemask.h>
#include <linux/bug.h>
#include <linux/page_ext.h>
#include <linux/syscalls.h>

#include <linux/page_predict/page_predict.h>
#include <linux/page_common/page_hotness_stat.h>
#include <linux/page_common/page_hotness_sysctl.h>

/*********************** syscall entry  *****************************/
#if CONFIG_PAGE_HOTNESS
static void init_migrate_stat(void)
{
	memset(&autonuma_migrate_page, 0, sizeof(struct autonuma_migrate_page));
	memset(&migrate_misplaced_page_stat, 0, sizeof(struct migrate_misplaced_page_stat));
}

SYSCALL_DEFINE2(launch_program_start,
        pid_t, pid, int, node)
{
    ktime_t start, diff;
    start = ktime_get();

    kunmapd_init(pid);

    // 这部分统计信息并非某个线程独有的，因此放这里应该也可以
    // 但是不能保证原子性
    init_migrate_stat();

    diff = ktime_sub(ktime_get(), start);
    printk("launch_program_start syscall: %lldms", ktime_to_ms(diff));
    return 0;
}

SYSCALL_DEFINE1(launch_program_end,
		pid_t, pid)
{
    ktime_t start, diff;
    start = ktime_get();

    kunmapd_exit();
    dump_migrate_stat();

    diff = ktime_sub(ktime_get(), start);
    printk("launch_program_end syscall: %lldms", ktime_to_ms(diff));
    return 0;
}
#else // !CONFIG_PAGE_HOTNESS
SYSCALL_DEFINE2(launch_program_start,
		pid_t, pid, int, node)
{
    return 0;
}

SYSCALL_DEFINE1(launch_program_end,
		pid_t, pid)
{
    return 0;
}
#endif // end of CONFIG_PAGE_HOTNESS

/*********************** demote path *****************************/
#ifdef CONFIG_PAGE_HOTNESS
int my_next_demotion_node(int nid)
{
	WARN_ON(nid >= MY_MEM_NODES);
#if 1
	if (most_access_node == 0) {
		// 0->1->2->3->none
		int next_node[MY_MEM_NODES] = {1, 2, 3, NUMA_NO_NODE};
		return next_node[nid];
	} else {
		// 1->0->3->2->none
		/***************************** 0, 1, 2,            3*/
		int next_node[MY_MEM_NODES] = {3, 0, NUMA_NO_NODE, 2};
		return next_node[nid];
	}
#elif 0
	// todo: 这里没有考虑task的位置
	if (nid == 0) {
		return 1;
	} else if (nid == 1) {
		return 2;
	} else if (nid == 2) {
		return 3;
	} else if (nid == 3) {
		return NUMA_NO_NODE;
	} else {
		return NUMA_NO_NODE;
	}
#else
	if (nid == 0) {
		return 2;
	} else if (nid == 1) {
		return 3;
	} else if (nid == 2) {
		return NUMA_NO_NODE;
	} else if (nid == 3) {
		return NUMA_NO_NODE;
	} else {
		return NUMA_NO_NODE;
	}
#endif
}

int my_second_demotion_node(int nid)
{
	if (nid == 0) {
		return 2;
	} else if (nid == 1) {
		return 3;
	} else {
		return NUMA_NO_NODE;
	}
}

#if 1
// 参考tiering-0.8
// 参考migrate_balanced_pgdat函数
bool pgdat_free_space_enough(struct pglist_data *pgdat)
{
	int z;
	unsigned long enough_mark;

	// 100M
	// enough_mark = max(100UL * 1024 * 1024 >> PAGE_SHIFT,

	// 500M
	enough_mark = max(500UL * 1024 * 1024 >> PAGE_SHIFT,
			  pgdat->node_present_pages >> 4);
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		/* Avoid waking kswapd */
		if (zone_watermark_ok(zone, 0,
				      high_wmark_pages(zone) + enough_mark,
				      ZONE_MOVABLE, 0))
			return true;
	}
	return false;
}

bool lowertier_pgdat_free_space_enough(struct pglist_data *pgdat)
{
	int z;
	unsigned long enough_mark;

	// 100M
	// enough_mark = max(100UL * 1024 * 1024 >> PAGE_SHIFT,

	// 100M
	enough_mark = max(500UL * 1024 * 1024 >> PAGE_SHIFT,
			  pgdat->node_present_pages >> 4);
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		/* Avoid waking kswapd */
		if (zone_watermark_ok(zone, 0,
				      high_wmark_pages(zone) + enough_mark,
				      ZONE_MOVABLE, 0))
			return true;
	}
	return false;
}
#else // 基于zone state
bool pgdat_free_space_enough(struct pglist_data *pgdat)
{
	int z;
	unsigned long free_pages;
	unsigned long enough_mark;

	// todo: 有没有更好的判断方法？
	// 1GB
	enough_mark = max(1UL * 1024 * 1024 * 1024 >> PAGE_SHIFT,
			  pgdat->node_present_pages >> 4);

	free_pages = 0;
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}

	// printk("pgdat_free_space_enough nid=%d, free_pages=%lu", pgdat->node_id, free_pages);

	return free_pages >= enough_mark;
}
#endif

bool reach_static_hot_threshold(unsigned long latency_sec)
{
	if (latency_sec < sysctl_page_unmap_hot_threshold_sec) {
		return true;
	}

	return false;
}

bool reach_dynamic_hot_threshold(struct base_histogram *access_histogram, unsigned long latency_sec)
{
	unsigned long cur_sum;

	if (!access_histogram || !access_histogram->histogram) {
		return reach_static_hot_threshold(latency_sec);
	}

	// todo: 维护一个前缀和数组（空间换时间）
	// 统计比test_val更热的页面的数目
	cur_sum = base_histogram_get_prefix_sum(access_histogram, latency_sec);

	// 迁移条件： 当前页面热度 > 目标节点中1/8的页面
	// 等价于：目标节点中比当前页面更热的页面数目 占 目标节点中所有页面数目 的比例 < 1/8
	// 比"cur page"更热的页面的比例 < 1/8
	// 即：cur page热度超过7/8的页面
	// if (cur_sum/sum < 1/8)
	if (cur_sum * 8 < access_histogram->sum) {
		return true;
	}

	return false;
}

#endif // end of CONFIG_PAGE_HOTNESS