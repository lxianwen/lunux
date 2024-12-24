#include <linux/page_common/page_hotness_sysctl.h>
#include <linux/capability.h>

DEFINE_STATIC_KEY_FALSE(static_key_autonuma_state);
DEFINE_STATIC_KEY_TRUE(static_key_kunmapd_state);
DEFINE_STATIC_KEY_TRUE(static_key_kdemote_state);
DEFINE_STATIC_KEY_FALSE(static_key_gups_hot_enable_state);

int sysctl_page_hotness_threshold = 2;
int sysctl_page_hotness_debug = 0;

unsigned int sysctl_page_unmap_hot_threshold_sec = 1; // s
// unsigned int sysctl_page_unmap_cold_threshold_sec = 10; // s
unsigned int sysctl_page_unmap_cold_threshold_sec = 5; // s
unsigned int sysctl_page_lantency_histogram_max_sec = 15; // s

unsigned int sysctl_page_recently_accessed_threshold_sec = 1; // second

static void set_autonuma_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&static_key_autonuma_state);
	else
		static_branch_disable(&static_key_autonuma_state);
}

static void set_kunmapd_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&static_key_kunmapd_state);
	else
		static_branch_disable(&static_key_kunmapd_state);
}

static void set_kdemote_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&static_key_kdemote_state);
	else
		static_branch_disable(&static_key_kdemote_state);
}

static void set_gups_state(bool enabled)
{
	if (enabled)
		static_branch_enable(&static_key_gups_hot_enable_state);
	else
		static_branch_disable(&static_key_gups_hot_enable_state);
}

static int sysctl_autonuma_handler(struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
    struct ctl_table t;
	int err;
	int state = static_branch_likely(&static_key_autonuma_state);

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;

	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

    // 根据输入值设置state
	if (write)
		set_autonuma_state(state);
	return err;
}

static int sysctl_kunmapd_handler(struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
    struct ctl_table t; // struct ctl_table类型的局部变量
	int err;
	int state = static_branch_likely(&static_key_kunmapd_state); // 获取当前状态

    // 如果是写操作,并且当前进程没有CAP_SYS_ADMIN权限,则返回权限错误码EPERM。
	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

    // 将参数table的内容复制到t(局部变量)中，其中data字段指向state变量(局部变量)的地址，以便于后续调用proc_dointvec_minmax函数。
	t = *table;
	t.data = &state;

    // 读取输入值，结果会存到t.data（即state变量）中
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

    // 根据输入值设置state
	if (write)
		set_kunmapd_state(state);
	return err;
}

static int sysctl_kdemote_handler(struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
    struct ctl_table t;
	int err;
	int state = static_branch_likely(&static_key_kdemote_state); // 获取当前状态

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;

	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	if (write)
		set_kdemote_state(state);
	return err;
}

static int sysctl_gups_handler(struct ctl_table *table, int write,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
    struct ctl_table t;
	int err;
	int state = static_branch_likely(&static_key_gups_hot_enable_state); // 获取当前状态

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &state;

	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (err < 0)
		return err;

	if (write)
		set_gups_state(state);
	return err;
}

struct ctl_table sysctl_page_hotness_table[] = {
    {
        .procname = "debug_enable",
        .data = &sysctl_page_hotness_debug,
        .maxlen = sizeof(int),
        .mode = 0644,
        .proc_handler = &proc_dointvec,
    },
    {
        .procname = "autonuma_enable",
        .data = NULL, // 在sysctl_autonuma_handler中处理
        .maxlen = sizeof(int),
        .mode = 0644,
        .proc_handler = &sysctl_autonuma_handler,
    },
    {
        .procname = "kunmapd_enable",
        .data = NULL, // 在sysctl_kunmapd_handler中处理
        .maxlen = sizeof(int),
        .mode = 0644,
        .proc_handler = &sysctl_kunmapd_handler,
    },
    {
        .procname = "gups_enable",
        .data = NULL,
        .maxlen = sizeof(int),
        .mode = 0644,
        .proc_handler = &sysctl_gups_handler,
    },
    {
        .procname = "demote_enable",
        .data = NULL,
        .maxlen = sizeof(int),
        .mode = 0644,
        .proc_handler = &sysctl_kdemote_handler,
    },
    // page unmap parameter
    {
        .procname = "page_unmap_hot_threshold",
        .data = &sysctl_page_unmap_hot_threshold_sec,
        .maxlen = sizeof(unsigned int),
        .mode = 0644,
        .proc_handler = &proc_douintvec,
    },
    {
        .procname = "page_unmap_cold_threshold_sec",
        .data = &sysctl_page_unmap_cold_threshold_sec,
        .maxlen = sizeof(unsigned int),
        .mode = 0644,
        .proc_handler = &proc_douintvec,
    },
    {
        .procname = "page_lantency_histogram_max_sec",
        .data = &sysctl_page_lantency_histogram_max_sec,
        .maxlen = sizeof(unsigned int),
        .mode = 0644,
        .proc_handler = &proc_douintvec,
    },
    {
        .procname = "page_recently_accessed_threshold_sec",
        .data = &sysctl_page_recently_accessed_threshold_sec,
        .maxlen = sizeof(unsigned int),
        .mode = 0644,
        .proc_handler = &proc_douintvec,
    },
    {}
};

struct ctl_table sysctl_page_hotness_root[] = {
    {
        .procname = "page_hotness",
        .mode = 0555,
        .child = sysctl_page_hotness_table,
    },
    {}
};

struct ctl_table_header *sysctl_page_hotness_header;
int sysctl_page_hotness_init(void)
{
    sysctl_page_hotness_header = register_sysctl_table(sysctl_page_hotness_root);
    if (!sysctl_page_hotness_header) {
        printk("sysctl_page_hotness_init failed\n");
        return -1;
    }
    printk("sysctl_page_hotness_init\n");

    return 0;
}

void sysctl_page_hotness_exit(void)
{
    if (sysctl_page_hotness_header)
        unregister_sysctl_table(sysctl_page_hotness_header);
    printk("sysctl_page_hotness_exit\n");
}
