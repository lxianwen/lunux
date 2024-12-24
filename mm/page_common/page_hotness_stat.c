#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/page_hotness/page_hotness.h>
#include <linux/page_hotness/page_info.h>
#include <linux/nodemask.h>
#include <linux/bug.h>
#include <linux/page_ext.h>

#include <linux/seq_file.h>
#include <linux/page_common/page_hotness_stat.h>
#include <linux/page_common/page_hotness_sysctl.h>

/************* most_access_node ***************/
// 为了减少开销，且因为其准确度不需要很及时地体现，
// 因此使用简单的全局变量，没有做同步控制，也没有使用PerCPU
unsigned long node_hint_page_faults[MY_MEM_NODES] = {0};
unsigned long max_hint_page_faults = 0;
int most_access_node = 0;

/************* promote alloc fail stat ***************/
// 使用pgdat_free_space_enough函数无法检测到这种情况
// 即pgdat_free_space_enough函数显示空间充足，但实际上一直promote失败
// 不清楚具体原因，猜测可能和alloc_flag有关
// 此处使用一个统计变量来监测这种情况
unsigned long global_promote_fail_nomem = 0;
// 假设有许多次迁移失败，那就证明空间不充足
// 此处暂时使用了硬编码
// unsigned long global_promote_fail_nomem_threshold = 10000; // 1W * 4KB = 40M
unsigned long global_promote_fail_nomem_threshold = 100;

/************* promote stat ***************/
DEFINE_PER_CPU(struct promote_histogram, promote_histogram) = {
    .sum = 0,
    .histograms = {{{{0}}}},
};

const char *promote_event_name[NR_PROMOTE_EVENT] = {
    "try_promote",
    "promote_success",
    "promote_fail",
};

int get_stat_histogram_binary_order(int num)
{
    int order = -1;  // 初始化为-1，以处理num为0的情况

    // 如果值为0，理论上是无解的，但是为了直方图正确，依然返回0
    if (num == 0) {
        return 0;
    }

    while (num > 0) {
        num = num >> 1; // 右移一位
        order++;        // 递增阶数
    }

	if (order >= STAT_HISTOGRAM_LENGTH) {
		order = STAT_HISTOGRAM_LENGTH - 1;
	}

    return order;
}

void count_promote_event(enum promote_event type, int src_nid, int target_nid, int latency_sec)
{
    struct promote_histogram *promote_histogram_ptr = this_cpu_ptr(&promote_histogram);
	++ (promote_histogram_ptr->histograms[type][src_nid][target_nid][get_stat_histogram_binary_order(latency_sec)]);
	++ (promote_histogram_ptr->histogram_sums[type][src_nid][target_nid]);
	++ (promote_histogram_ptr->event_sums[type]);
    ++ (promote_histogram_ptr->sum);
    return ;
}

unsigned long get_cpu_promote_sum(int cpu)
{
    struct promote_histogram *promote_histogram_ptr = per_cpu_ptr(&promote_histogram, cpu);
	return (promote_histogram_ptr->sum);
}

unsigned long get_cpu_promote_count(int cpu, enum promote_event type, int src_nid, int target_nid, int latency_sec)
{
    struct promote_histogram *promote_histogram_ptr = per_cpu_ptr(&promote_histogram, cpu);
	return (promote_histogram_ptr->histograms[type][src_nid][target_nid][get_stat_histogram_binary_order(latency_sec)]);
}

void dump_histogram(unsigned long node_histograms[])
{
	int time;
	int max_time = STAT_HISTOGRAM_LENGTH;
	unsigned long sum = 0;

	for (time = 0; time < max_time; time++) {
		int tmp_cnt = node_histograms[time];
		if (tmp_cnt == 0)
			continue;
		if (time == max_time - 1) {
			printk(KERN_CONT "(max:%u), ", tmp_cnt);
		} else {
			printk(KERN_CONT "(%d:%u), ", time, tmp_cnt);
		}
		sum += tmp_cnt;
	}
	printk(KERN_CONT "sum=%lu\n", sum);
}

void dump_promote_events(void)
{
	int src_nid, target_nid;
	int event;
	int cpu;

	printk("promote_histogram:\n");
	for_each_online_cpu(cpu) {
		struct promote_histogram *promote_histogram_ptr = per_cpu_ptr(&promote_histogram, cpu);
		if (promote_histogram_ptr->sum == 0) {
			continue;
		}

		printk("cpu=%d, cpu_sum=%lu", cpu, promote_histogram_ptr->sum);
		for (event = 0; event < NR_PROMOTE_EVENT; event++) {
			printk("event=%s, event_sum=%lu", promote_event_name[event], promote_histogram_ptr->event_sums[event]);

			for (src_nid = 0; src_nid < MY_MEM_NODES; src_nid++) {
				for (target_nid = 0; target_nid < MY_MEM_NODES; target_nid++) {
					unsigned long *histogram = promote_histogram_ptr->histograms[event][src_nid][target_nid];
					unsigned long histogram_sum = promote_histogram_ptr->histogram_sums[event][src_nid][target_nid];

					if (histogram_sum == 0) {
						continue;
					}

					printk("(%d->%d), sum=%lu, histogram:",  src_nid, target_nid, histogram_sum);
					dump_histogram(histogram);
				}
			}
		}
	}
}


/************* exchange stat ***************/
DEFINE_PER_CPU(struct exchange_histogram, exchange_histogram) = {
    .sum = 0,
    .upper_histograms = {{{{0}}}},
    .lower_histograms = {{{{0}}}},
};
const char *exchange_event_name[NR_EXCHANGE_EVENT] = {
    "try_exchange",
    "exchange_success",
    "exchange_only_promote_success",
    "exchange_only_demote_success",
    "exchange_fail",
};

void count_exchange_event(enum exchange_event type, int src_nid, int target_nid, int upper_tier_latency_sec, int lower_tier_latency_sec)
{
    struct exchange_histogram *exchange_histogram_ptr = this_cpu_ptr(&exchange_histogram);

	++ (exchange_histogram_ptr->upper_histograms[type][src_nid][target_nid][get_stat_histogram_binary_order(upper_tier_latency_sec)]);
	++ (exchange_histogram_ptr->lower_histograms[type][src_nid][target_nid][get_stat_histogram_binary_order(lower_tier_latency_sec)]);
    
	++ (exchange_histogram_ptr->histogram_sums[type][src_nid][target_nid]);
	++ (exchange_histogram_ptr->event_sums[type]);
    ++ (exchange_histogram_ptr->sum);
    return ;
}


void dump_exchange_events(void)
{
	int src_nid, target_nid;
	int event;
	int cpu;

	printk("exchange_histogram:\n");
	for_each_online_cpu(cpu) {
		struct exchange_histogram *exchange_histogram_ptr = per_cpu_ptr(&exchange_histogram, cpu);
		if (exchange_histogram_ptr->sum == 0) {
			continue;
		}

		printk(">>> cpu=%d, cpu_sum=%lu", cpu, exchange_histogram_ptr->sum);
		for (event = 0; event < NR_EXCHANGE_EVENT; event++) {
			printk("event=%s, event_sum=%lu", exchange_event_name[event], exchange_histogram_ptr->event_sums[event]);
			for (src_nid = 0; src_nid < MY_MEM_NODES; src_nid++) {
				for (target_nid = 0; target_nid < MY_MEM_NODES; target_nid++) {
					unsigned long *upper_histogram = exchange_histogram_ptr->upper_histograms[event][src_nid][target_nid];
					unsigned long *lower_histogram = exchange_histogram_ptr->lower_histograms[event][src_nid][target_nid];
					unsigned long histogram_sum = exchange_histogram_ptr->histogram_sums[event][src_nid][target_nid];

					if (histogram_sum == 0) {
						continue;
					}

					printk("(%d<->%d): sum=%lu",  src_nid, target_nid, histogram_sum);
                    printk("upper_histogram (should cold): ");
					dump_histogram(upper_histogram);
                    printk("lower_histogram (should hot): ");
					dump_histogram(lower_histogram);
				}
			}
		}
	}
}

struct migrate_misplaced_page_stat migrate_misplaced_page_stat = {0};
struct autonuma_migrate_page autonuma_migrate_page = {0};

void dump_migrate_stat(void)
{
	if (static_branch_likely(&static_key_autonuma_state)) {
		printk("autonuma_migrate_page.all = %lu", autonuma_migrate_page.all);
		printk("autonuma_migrate_page.migrate_success = %lu", autonuma_migrate_page.migrate_success);
	}
	
	printk("migrate_misplaced_page_stat.all = %lu", migrate_misplaced_page_stat.all);
	printk("migrate_misplaced_page_stat.not_page_lru = %lu", migrate_misplaced_page_stat.not_page_lru);
	printk("migrate_misplaced_page_stat.mapcount_one_and_file_lru = %lu", migrate_misplaced_page_stat.mapcount_one_and_file_lru);
	printk("migrate_misplaced_page_stat.dirty_and_file_lru = %lu", migrate_misplaced_page_stat.dirty_and_file_lru);
	printk("migrate_misplaced_page_stat.isolate_fail = %lu", migrate_misplaced_page_stat.isolate_fail);
	printk("migrate_misplaced_page_stat.migrate_fail = %lu", migrate_misplaced_page_stat.migrate_fail);
	printk("migrate_misplaced_page_stat.migrate_success = %lu", migrate_misplaced_page_stat.migrate_success);
}
