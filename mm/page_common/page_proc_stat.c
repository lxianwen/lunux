#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>

#include <linux/page_common/page_hotness_stat.h>

static int my_counter = 0;

static void reset_page_hotness_stat(void)
{
    printk("reset_page_hotness_stat: not used");
    return ;
}

static void dump_page_hotness_proc_stat(struct seq_file *m)
{
    printk("dump_page_hotness_proc_stat: not used");
    return ;
}

static void reset_stat(void)
{
    reset_page_hotness_stat();
}

static void show_stat(struct seq_file *m)
{
    dump_page_hotness_proc_stat(m);
    
    return ;
}

static int my_stat_show(struct seq_file *m, void *v) {
    // Replace this with your own code to collect statistics
    seq_printf(m, "Sample Stat: %d\n", ++my_counter);
    show_stat(m);
    
    return 0;
}

static int my_stat_open(struct inode *inode, struct file *file) {
    return single_open(file, my_stat_show, NULL);
}

static ssize_t my_stat_reset(struct file *file, const char __user *unsafe_buf, size_t count, loff_t *ppos) {
    // 解析用户空间传递的命令，例如 "reset"，然后根据命令执行相应的操作
    // 这里假设用户输入的命令是 "reset"
    char buf[33];
    int len = 33;
    if (len > count) {
        len = count;
    }

    if (copy_from_user(buf, unsafe_buf, len)) {
        printk("my_stat_reset: copy_from_user\n");
		return -EFAULT;
    }
    buf[len-1] = '\0';

    if (strncmp(buf, "reset", 5) == 0) {
        my_counter = 0;
        reset_stat();
        printk("my_stat_reset: reset my_counter\n");
        return 5; // 返回处理的字符数
    }

    printk("my_stat_reset: unvalid parameter\n");
    return -EINVAL; // 命令无效
}

static const struct proc_ops my_stat_fops = {
    .proc_open = my_stat_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
    .proc_write = my_stat_reset,
};

int my_stat_init(void) {
    proc_create("my_stat", 0, NULL, &my_stat_fops);
    return 0;
}

void my_stat_exit(void) {
    remove_proc_entry("my_stat", NULL);
}