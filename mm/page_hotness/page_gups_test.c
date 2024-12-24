#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/nodemask.h>

#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/mempolicy.h>
#include <linux/gfp.h>
#include <linux/vmstat.h>

#include <linux/page_hotness/page_gups_test.h>
#include <linux/page_common/page_hotness_sysctl.h>

// typedef uint64_t(*Area)[2];
// Area gups_areas = NULL;
// Area hot_areas = NULL;

struct area_warp area_warp;
pid_t gups_pid;

#ifdef CONFIG_PAGE_GUPS_TEST
bool __addr_in_hotset(uint64_t address, enum gups_test_type type)
{
    int i = 0;
    bool in_gups_area = false;
    bool in_hotset_area = false;

    if (area_warp.gups_areas == NULL || area_warp.hot_areas == NULL) {
        printk("area_warp.gups_areas and hot_areas are null");
        return false;
    }

    for (i = 0; i < area_warp.len_gups_areas; i++) {
        if (address >= area_warp.gups_areas[i].start && address <= area_warp.gups_areas[i].end) {
            in_gups_area = true;
            break;
        }
    }
    
    if (!in_gups_area) {
        if (type == GUPS_TEST_PREDICT)
            count_vm_event(GUPS_PREDICT_NOT_IN_AREA);
        else if (type == GUPS_TEST_PROMOTE) {
            count_vm_event(GUPS_PROMOTE_NOT_IN_AREA);
        } else if (type == GUPS_TEST_DEMOTE) {
            count_vm_event(GUPS_DEMOTE_NOT_IN_AREA);
        }
        // printk("comm=%s, not in gups_area, address=0x%llx", current->comm, address);
    }

    for (i = 0; i < area_warp.len_hot_area; i++) {
        if (address >= area_warp.hot_areas[i].start && address <= area_warp.hot_areas[i].end) {
            in_hotset_area = true;
            break;
        }
    }
    
    return in_hotset_area;
}

bool addr_in_hotset(uint64_t address, enum gups_test_type type)
{
    if (!static_branch_likely(&static_key_gups_hot_enable_state)) {
        return 0;
    }
    return __addr_in_hotset(address, type);
}

void dump_areas(void)
{
    int i = 0;
    
    printk("gups_areas: len_gups_areas = %ld", area_warp.len_gups_areas);
    for (i = 0; i < area_warp.len_gups_areas; i++) {
        printk("[0x%llx, 0x%llx]\n", area_warp.gups_areas[i].start,
                                       area_warp.gups_areas[i].end);
    }

    printk("hot_areas: len_hot_area = %ld", area_warp.len_hot_area);
    for (i = 0; i < area_warp.len_hot_area; i++) {
        printk("[0x%llx, 0x%llx]", area_warp.hot_areas[i].start,
                                       area_warp.hot_areas[i].end);
    }
}

#define NR_GUPS_NODE 4
struct gups_memory_layout_stat {
    unsigned long hot_pages[NR_GUPS_NODE];
    unsigned long all_pages[NR_GUPS_NODE];
};

static struct mm_struct *get_mm_from_pid(pid_t pid)
{
    struct pid *pid_struct = NULL;
    struct task_struct *task = NULL;
    struct mm_struct *mm = NULL;

    pid_struct = find_get_pid(pid); // get_pid
    if (pid_struct == NULL) {
        return NULL;
    }

    task = get_pid_task(pid_struct, PIDTYPE_PID);
    if (task != NULL) {
        mm = task->mm; // 获取 mm_struct
        put_task_struct(task);
    }

    mm = get_task_mm(task);
	if (!mm) {
		put_task_struct(task);
		return NULL;
	}

    return mm;
}

// 该函数非常耗时
void dump_whole_memory_layout(void)
{
    pid_t pid = gups_pid; // 使用全局变量
    struct mm_struct *mm;
    struct gups_memory_layout_stat gups_memory_layout_stat = {0};
    struct vm_area_struct *vma = NULL;
    unsigned long addr;
    struct page *page = NULL;
    int nid = 0;

    if (!static_branch_likely(&static_key_gups_hot_enable_state)) {
        return ;
    }

    printk("dump_whole_memory_layout");
    mm = get_mm_from_pid(pid);
    if (!mm) {
        printk("dump_whole_memory_layout: mm is null");
        return ;
    }

    mmap_read_lock(mm);
    vma = find_vma(mm, 0);
    for (; vma; vma = vma->vm_next) {
        for (addr = vma->vm_start; addr < vma->vm_end; addr += PAGE_SIZE) {
            bool in_hotset = __addr_in_hotset(addr, false);

            page = page_walk(vma, addr);
            if (!page) {
                continue;
            }
            nid = page_to_nid(page);

            gups_memory_layout_stat.all_pages[nid] ++;
            gups_memory_layout_stat.hot_pages[nid] += in_hotset ? 1 : 0;
        }
    }
    mmap_read_unlock(mm);

    for (nid = 0; nid < NR_GUPS_NODE; nid++) {
        printk("nid=%d, all=%lu, hot=%lu", nid,
                gups_memory_layout_stat.all_pages[nid],
                gups_memory_layout_stat.hot_pages[nid]);
    }
}
#endif

#ifdef CONFIG_PAGE_GUPS_TEST
SYSCALL_DEFINE5(page_hotset_init,
                    pid_t, pid,
                    struct area __user *, user_gups_areas,
                    size_t, len_gups_areas,
                    struct area __user *, user_hot_areas,
                    size_t, len_hot_area,
                    )
{
    if (!static_branch_likely(&static_key_gups_hot_enable_state)) {
        return 0;
    }

    gups_pid = pid;
    printk("page_hotset_init: gups_pid=%d", gups_pid);

    area_warp.gups_areas = NULL;
    area_warp.hot_areas = NULL;
    area_warp.len_gups_areas = len_gups_areas;
    area_warp.len_hot_area = len_hot_area;

    area_warp.gups_areas = kmalloc(len_gups_areas * sizeof(struct area), GFP_KERNEL);
    if (!area_warp.gups_areas) {
        area_warp.len_gups_areas = 0;
        printk("sys_page_hotset_init: gups_areas alloc failed");
        return -ENOMEM;
    }

    area_warp.hot_areas = kmalloc(len_hot_area * sizeof(struct area), GFP_KERNEL);
    if (!area_warp.hot_areas) {
        area_warp.len_hot_area = 0;
        printk("sys_page_hotset_init: hot_areas alloc failed");
        return -ENOMEM;
    }

    if (copy_from_user(area_warp.gups_areas, user_gups_areas, len_gups_areas * sizeof(struct area))) {
        kfree(area_warp.gups_areas);
        area_warp.len_gups_areas = 0;
        printk("sys_page_hotset_init: copy_from_user gups_areas failed");
        return -EFAULT;
    }
    if (copy_from_user(area_warp.hot_areas, user_hot_areas, len_hot_area * sizeof(struct area))) {
        kfree(area_warp.hot_areas);
        area_warp.len_hot_area = 0;
        printk("sys_page_hotset_init: copy_from_user hot_areas failed");
        return -EFAULT;
    }

    dump_areas();
    return 0;
}

SYSCALL_DEFINE0(page_hotset_exit)
{
    if (!static_branch_likely(&static_key_gups_hot_enable_state)) {
        printk("page_hotset_exit: gups disable, but we still need to try to free area");
    }

    if (area_warp.gups_areas && area_warp.hot_areas) {
        dump_whole_memory_layout();
    }

    if (!area_warp.gups_areas) {
        printk("area_warp.gups_areas is null");
    } else {
        kfree(area_warp.gups_areas);
    }

    if (!area_warp.hot_areas) {
        printk("area_warp.hot_areas is null");
    } else {
        kfree(area_warp.hot_areas);
    }

    area_warp.len_gups_areas = 0;
    area_warp.len_hot_area = 0;
    printk("clear area_warp.gups_areas and area_warp.hot_areas");

    return 0;
}

SYSCALL_DEFINE0(page_hotset_debug)
{
    if (area_warp.gups_areas && area_warp.hot_areas) {
        dump_whole_memory_layout();
    } else {
        printk("page_hotset_debug: area_warp.gups_areas=%px, area_warp.hot_areas=%px",
            area_warp.gups_areas, area_warp.hot_areas);
    }

    return 0;
}
#else // !CONFIG_PAGE_GUPS_TEST
SYSCALL_DEFINE5(page_hotset_init,
                    pid_t, pid,
                    struct area __user *, user_gups_areas,
                    size_t, len_gups_areas,
                    struct area __user *, user_hot_areas,
                    size_t, len_hot_area,
                    )
{
    return 0;
}
SYSCALL_DEFINE0(page_hotset_exit)
{
    return 0;
}
SYSCALL_DEFINE0(page_hotset_debug)
{
    return 0;
}
#endif // end CONFIG_PAGE_GUPS_TEST
