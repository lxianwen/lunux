#include <linux/types.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/page_ext.h>
#include <linux/page_hotness/page_info.h>
#include <linux/page_hotness/base_histogram.h>

/***************************** base_histogram *****************************/

#ifdef CONFIG_ACCESS_HISTOGRAM

#if 0
static bool check_base_histogram(char *name, struct base_histogram *base_histogram)
{
    int i;
    unsigned long sum = 0;

    for (i = 0; i < base_histogram->len; i++) {
		sum += base_histogram->histogram[i];
	}

    if (sum != base_histogram->sum) {
        printk("%s: sum=%lu, base_histogram->sum=%lu", name, sum, base_histogram->sum);
        return false;
    }

    return true;
}
#else
static inline bool check_base_histogram(char *name, struct base_histogram *base_histogram)
{
    return true;
}
#endif

#if 1
void base_histogram_dump(const char *name, struct base_histogram *base_histogram, bool locked)
{
    int time = 0;
    int max_time;
    unsigned long cur_sum = 0;
    unsigned long irq_flags;
    
    if (!base_histogram || !base_histogram->histogram)
        return ;

    if (!locked)
        spin_lock_irqsave(&base_histogram->histogram_lock, irq_flags);
    max_time = base_histogram->len;

    printk("%s", name);
    printk(KERN_CONT ", sum=%lu, ", base_histogram->sum);
    for (time = 0; time < base_histogram->len; time++) {
        if (!base_histogram->histogram[time]) {
			continue;
		}

        if (time == max_time - 1) {
			printk(KERN_CONT "(max:%lu)", base_histogram->histogram[time]);
		} else {
			printk(KERN_CONT "(%d:%lu), ", time, base_histogram->histogram[time]);
		}
        cur_sum += base_histogram->histogram[time];
    }
    printk(KERN_CONT "cur_sum=%lu\n", cur_sum);
    if (!locked)
        spin_unlock_irqrestore(&base_histogram->histogram_lock, irq_flags);
}
#else
inline void base_histogram_dump(const char *name, struct base_histogram *base_histogram, bool locked)
{
    return ;
}
#endif

void init_base_histogram(struct base_histogram *base_histogram, int len, enum histogram_type type, int nid)
{
    // init len
    WARN_ON(type!=NORMAL_COUNT && type!=ORDER_COUNT);
    base_histogram->type = type;
    base_histogram->len = len;
    base_histogram->sum = 0;

    // alloc and init histogram
    // base_histogram->histogram = kmalloc(sizeof(unsigned long) * len, GFP_KERNEL);
    // WARN_ON(!base_histogram->histogram);
    if (base_histogram->histogram) {
        memset(base_histogram->histogram, 0, sizeof(unsigned long) * len);
    }

    check_base_histogram("init_base_histogram", base_histogram);

    spin_lock_init(&base_histogram->histogram_lock);
    base_histogram->nid = nid;
}

static int __get_base_histogram_sum(struct base_histogram *base_histogram)
{
    return base_histogram->sum;
}

static int get_binary_order(int num)
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
    return order;
}

// unused
// static bool is_power_of_two(int num)
// {
//     // ps: 1 = 2 ^ 0
//     // 用位运算判断是否只有一个位是1
//     return (num > 0) && ((num & (num - 1)) == 0);
// }

// 前提：old_val是在直方图中存在的
bool __base_histogram_update(struct base_histogram *base_histogram, unsigned long old_val, unsigned long new_val)
{
    int old_index;
    int new_index;

    if (!base_histogram || !base_histogram->histogram)
        return false;

    check_base_histogram("before __base_histogram_update", base_histogram);

    if (base_histogram->type == NORMAL_COUNT) {
        old_index = old_val;
        new_index = new_val;
    } else if (base_histogram->type == ORDER_COUNT) {
        old_index = get_binary_order(old_val);
        new_index = get_binary_order(new_val);
    }

    // border test
    if (old_index >= base_histogram->len) {
        old_index = base_histogram->len - 1;
    }
    if (new_index >= base_histogram->len) {
        new_index = base_histogram->len - 1;
    }

    if (base_histogram->histogram[old_index] == 0) {
        printk("lmy-warn: base_histogram_update: base_histogram->histogram[%d] == 0, nid=%d", old_index, base_histogram->nid);
        return false;
    }

    --base_histogram->histogram[old_index];
    ++base_histogram->histogram[new_index];

    check_base_histogram("after __base_histogram_update", base_histogram);

    // printk("update: access_histogram[%u]=%lu, access_histogram[%u]=%lu", 
    //     old_index, base_histogram->histogram[old_index],
    //     new_index, base_histogram->histogram[new_index]);

    return true;
}

// 前提：当前直方图不存在该项
bool __base_histogram_insert(struct base_histogram *base_histogram, unsigned long new_val)
{
    int new_index;

    if (!base_histogram || !base_histogram->histogram)
        return false;

    check_base_histogram("before __base_histogram_insert", base_histogram);

    if (base_histogram->type == NORMAL_COUNT) {
        new_index = new_val;
    } else if (base_histogram->type == ORDER_COUNT) {
        new_index = get_binary_order(new_val);
    }

    if (new_index >= base_histogram->len) {
        new_index = base_histogram->len - 1;
    }

    ++base_histogram->histogram[new_index];
    ++base_histogram->sum;

    if (!check_base_histogram("after __base_histogram_insert", base_histogram)) {
        printk("new_val=%ld, new_index=%d", new_val, new_index);
    }

    // printk("base_histogram_insert new_val=%lu", new_val);

    // printk("insert: access_histogram[%u]=%lu, base_histogram->sum=%lu", 
    //     new_index, base_histogram->histogram[new_index], base_histogram->sum);

    return true;
}

void __base_histogram_delete(struct base_histogram *base_histogram, unsigned long old_val)
{
    int old_index;

    if (!base_histogram || !base_histogram->histogram)
        return ;

    check_base_histogram("before __base_histogram_delete", base_histogram);

    if (base_histogram->type == NORMAL_COUNT) {
        old_index = old_val;
    } else if (base_histogram->type == ORDER_COUNT) {
        old_index = get_binary_order(old_val);
    }

    if (old_index >= base_histogram->len) {
        old_index = base_histogram->len - 1;
    }
    --base_histogram->histogram[old_index];
    base_histogram->sum --;

    check_base_histogram("after __base_histogram_delete", base_histogram);

    // printk("delete: access_histogram[%u]=%lu, base_histogram->sum=%lu", 
    //     old_index, base_histogram->histogram[old_index], base_histogram->sum);
}

unsigned long base_histogram_get_prefix_sum(struct base_histogram *base_histogram, unsigned long test_val)
{
    int64_t cur_sum;
    int test_index;
    int i;
    unsigned long irq_flags;

    if (!base_histogram || !base_histogram->histogram) {
        printk("base_histogram_get_prefix_sum: !base_histogram || !base_histogram->histogram");
        return 0;
    }

    spin_lock_irqsave(&base_histogram->histogram_lock, irq_flags);

    check_base_histogram("before base_histogram_get_prefix_sum", base_histogram);

    if (base_histogram->type == NORMAL_COUNT) {
        test_index = test_val;
    } else if (base_histogram->type == ORDER_COUNT) {
        test_index = get_binary_order(test_val);
    }

    if (test_index >= base_histogram->len) {
        test_index = base_histogram->len - 1;
    }

    cur_sum = 0;
    for (i = 0; i < test_index; i++) {
		cur_sum += base_histogram->histogram[i];
	}

    check_base_histogram("after base_histogram_get_prefix_sum", base_histogram);

    spin_unlock_irqrestore(&base_histogram->histogram_lock, irq_flags);
    return cur_sum;
}

bool in_access_histogram(struct page* page)
{
    struct page_ext *page_ext = lookup_page_ext(page);
    struct page_info *pi = NULL;
    bool in_status = true;
    unsigned long irq_flags;

	// 系统刚启动的时候，这里的page_ext可能为NULL，不进行判断的话，会导致系统无法正常启动
	if (unlikely(!page_ext))
		return false;

    if (unlikely(!PageTracked(page_ext)))
		return false;

    pi = get_page_info(page_ext);
    spin_lock_irqsave(&pi->pi_lock, irq_flags);
    in_status = PageHistogram(page_ext) ? true : false;
    spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

    return in_status;
}

// access_histogram
void __access_histogram_insert_or_update(struct page* page, unsigned long latency_sec)
{
    struct base_histogram *access_histogram = NULL;
    struct page_ext *page_ext = NULL;
    struct page_info *pi = NULL;
    
    page_ext = lookup_page_ext(page);
    pi = get_page_info(page_ext);
    if (!page_ext || !pi) {
        return ;
    }

    access_histogram = &page_pgdat(page)->access_histogram;
    if (!access_histogram || !access_histogram->histogram) {
        return ;
    }

    if (PageHistogram(page_ext)) {
        __base_histogram_update(access_histogram, pi->access_interval_sec, latency_sec);
    } else {
		__base_histogram_insert(access_histogram, latency_sec);
    }
}

int get_access_histogram_sum(struct page* page)
{
    struct base_histogram *access_histogram = NULL;
    unsigned long irq_flags;
    int size;

    access_histogram = &page_pgdat(page)->access_histogram;
    if (!access_histogram || !access_histogram->histogram) {
        return 0;
    }

    spin_lock_irqsave(&access_histogram->histogram_lock, irq_flags);
    size = __get_base_histogram_sum(access_histogram);
    spin_unlock_irqrestore(&access_histogram->histogram_lock, irq_flags);

    return size;
}

void access_histogram_insert_or_update(struct page* page, unsigned long latency_sec, bool locked)
{
    struct base_histogram *access_histogram = NULL;
    struct page_ext *page_ext = NULL;
    struct page_info *pi;
    unsigned long irq_flags;

    access_histogram = &page_pgdat(page)->access_histogram;
    if (!access_histogram || !access_histogram->histogram) {
        return ;
    }

    page_ext = lookup_page_ext(page);
    if (!page_ext || !PageTracked(page_ext))
        return ;

    pi = get_page_info(page_ext);
    if (!locked) {
        spin_lock_irqsave(&access_histogram->histogram_lock, irq_flags);
        spin_lock(&pi->pi_lock);
    }

    __access_histogram_insert_or_update(page, latency_sec);
    // 存储时间间隔到page info里面，用于下次update使用
    pi->access_interval_sec = latency_sec;
    SetPageHistogram(lookup_page_ext(page));

    // check_base_histogram("after access_histogram_insert_or_update", access_histogram);
    if (!locked) {
        spin_unlock(&pi->pi_lock);
        spin_unlock_irqrestore(&access_histogram->histogram_lock, irq_flags);
    }
}

void access_histogram_delete(struct page* page)
{
    struct base_histogram *access_histogram = NULL;
    struct page_ext *page_ext;
    struct page_info *pi;
    unsigned long irq_flags;
    
    page_ext = lookup_page_ext(page);
    if (!page_ext || !PageTracked(page_ext)) {
        return ;
    }

    pi = get_page_info(page_ext);
    access_histogram = &page_pgdat(page)->access_histogram;
    if (!access_histogram || !access_histogram->histogram) {
        return ;
    }

    spin_lock_irqsave(&access_histogram->histogram_lock, irq_flags);
    spin_lock(&pi->pi_lock);
    check_base_histogram("before access_histogram_delete", access_histogram);

    if (!PageHistogram(page_ext)) {
        printk("warn: access_histogram delete: !PageHistogram(page_ext), page=%px, nid=%d", page, access_histogram->nid);
        goto unlock;
    }
    
    ClearPageHistogram(page_ext);
    __base_histogram_delete(access_histogram, pi->access_interval_sec);
    
unlock:
    check_base_histogram("after access_histogram_delete", access_histogram);
    spin_unlock(&pi->pi_lock);
    spin_unlock_irqrestore(&access_histogram->histogram_lock, irq_flags);
}

void access_histogram_copy(struct page* old_page, struct page* new_page)
{
    struct base_histogram *old_access_histogram = NULL;
    struct base_histogram *new_access_histogram = NULL;
    struct base_histogram *first_access_histogram = NULL;
    struct base_histogram *second_access_histogram = NULL;
    struct page_ext *old_ext, *new_ext;
	struct page_info *old_pi, *new_pi;
    struct page_info *first_pi, *second_pi;
    unsigned int tmp_access_interval_sec;
    unsigned long irq_flags;

    // 在内存规整函数（kcompactd）中，源页面和目标页面处于同一个节点
    // 如果不跳过，后面会出现死锁
    if (page_pgdat(old_page) == page_pgdat(new_page)) {
        return ;
    }

    old_access_histogram = &page_pgdat(old_page)->access_histogram;
    new_access_histogram = &page_pgdat(new_page)->access_histogram;
    if (!old_access_histogram || !old_access_histogram->histogram) {
        return ;
    }
    if (!new_access_histogram || !new_access_histogram->histogram) {
        return ;
    }

	old_ext = lookup_page_ext(old_page);
	new_ext = lookup_page_ext(new_page);
	if (unlikely(!old_ext || !new_ext))
		return;

    if (unlikely(!PageTracked(old_ext) || !PageTracked(new_ext))) {
        return ;
    }

	old_pi = get_page_info(old_ext);
	new_pi = get_page_info(new_ext);

    // 为了避免死锁，这里定义锁的顺序：根据指针地址，优先对低地址进行加锁
    if (old_access_histogram < new_access_histogram) {
        first_access_histogram = old_access_histogram;
        second_access_histogram = new_access_histogram;
    } else {
        first_access_histogram = new_access_histogram;
        second_access_histogram = old_access_histogram;
    }

    if (old_pi < new_pi) {
        first_pi = old_pi;
        second_pi = new_pi;
    } else {
        first_pi = new_pi;
        second_pi = old_pi;
    }

    // printk("first_access_histogram=%px, histogram_lock=%px", first_access_histogram, &first_access_histogram->histogram_lock);
    // printk("second_access_histogram=%px, histogram_lock=%px", second_access_histogram, &second_access_histogram->histogram_lock);
    // printk("first_pi=%px, pi_lock=%px", first_pi, &first_pi->pi_lock);
    // printk("second_pi=%px, pi_lock=%px", second_pi, &second_pi->pi_lock);

    spin_lock_irqsave_nested(&first_access_histogram->histogram_lock, irq_flags, 3);
    spin_lock_nested(&second_access_histogram->histogram_lock, 2);
    spin_lock_nested(&first_pi->pi_lock, 1);
    spin_lock_nested(&second_pi->pi_lock, 0);

#if 0
    static int debug_cnt = 0;
    if (debug_cnt < 10) {
        debug_cnt ++;
        printk("before copy: old_ext=(%d,%u), new_ext=(%d,%u)", PageHistogram(old_ext), old_pi->access_interval_sec,
                    PageHistogram(new_ext), new_pi->access_interval_sec);
        base_histogram_dump("access_histogram_copy before: old_access_histogram", old_access_histogram, true);
        base_histogram_dump("access_histogram_copy before: new_access_histogram", new_access_histogram, true);
    }
#endif

    // 旧页面没有直方图信息，无需拷贝
    if (!PageHistogram(old_ext)) {
        WARN_ON(PageHistogram(new_ext));
        goto unlock;
    }
    // 移除旧页的直方图信息
    tmp_access_interval_sec = old_pi->access_interval_sec;
    __base_histogram_delete(old_access_histogram, tmp_access_interval_sec);
    // 移除旧页的flag与时间间隔字段信息
    ClearPageHistogram(old_ext);
    old_pi->access_interval_sec = 0;

    // 理论上，new page是新页，不应该有直方图信息
    // WARN_ON(PageHistogram(new_ext));
    // 建立新页的直方图信息
    __base_histogram_insert(new_access_histogram, tmp_access_interval_sec);
    // 建立新页的flag与时间间隔字段信息
    new_pi->access_interval_sec = tmp_access_interval_sec;
    SetPageHistogram(new_ext);

#if 0
    if (debug_cnt < 10) {
        printk("after copy: old_ext=(%d,%u), new_ext=(%d,%u)", PageHistogram(old_ext), old_pi->access_interval_sec,
                        PageHistogram(new_ext), new_pi->access_interval_sec);
        base_histogram_dump("access_histogram_copy after: old_access_histogram", old_access_histogram, true);
        base_histogram_dump("access_histogram_copy after: new_access_histogram", new_access_histogram, true);
    }
#endif

unlock:
    spin_unlock(&second_pi->pi_lock);
    spin_unlock(&first_pi->pi_lock);
    spin_unlock(&second_access_histogram->histogram_lock);
    spin_unlock_irqrestore(&first_access_histogram->histogram_lock, irq_flags);

    return ;
}

void access_histogram_exchange(struct page* from_page, struct page* to_page)
{
    struct base_histogram *from_access_histogram = NULL;
    struct base_histogram *to_access_histogram = NULL;
    struct base_histogram *first_access_histogram = NULL;
    struct base_histogram *second_access_histogram = NULL;
    struct page_ext *from_ext, *to_ext;
	struct page_info *from_pi, *to_pi;
    struct page_info *first_pi, *second_pi;
    
    unsigned long irq_flags;

    // 在内存规整函数（kcompactd）中，源页面和目标页面处于同一个节点
    if (page_pgdat(from_page) == page_pgdat(to_page)) {
        return ;
    }

    from_access_histogram = &page_pgdat(from_page)->access_histogram;
    to_access_histogram = &page_pgdat(to_page)->access_histogram;
    if (!from_access_histogram || !from_access_histogram->histogram) {
        return ;
    }
    if (!to_access_histogram || !to_access_histogram->histogram) {
        return ;
    }

	from_ext = lookup_page_ext(from_page);
	to_ext = lookup_page_ext(to_page);
	if (unlikely(!from_ext || !to_ext))
		return;

    if (unlikely(!PageTracked(from_ext) || !PageTracked(to_ext))) {
        return ;
    }

	from_pi = get_page_info(from_ext);
	to_pi = get_page_info(to_ext);

    // 为了避免死锁，这里定义锁的顺序：根据指针地址，优先对低地址进行加锁
    if (from_access_histogram < to_access_histogram) {
        first_access_histogram = from_access_histogram;
        second_access_histogram = to_access_histogram;
    } else {
        first_access_histogram = to_access_histogram;
        second_access_histogram = from_access_histogram;
    }

    if (from_pi < to_pi) {
        first_pi = from_pi;
        second_pi = to_pi;
    } else {
        first_pi = to_pi;
        second_pi = from_pi;
    }

    // printk("first_access_histogram=%px, histogram_lock=%px", first_access_histogram, &first_access_histogram->histogram_lock);
    // printk("second_access_histogram=%px, histogram_lock=%px", second_access_histogram, &second_access_histogram->histogram_lock);
    // printk("first_pi=%px, pi_lock=%px", first_pi, &first_pi->pi_lock);
    // printk("second_pi=%px, pi_lock=%px", second_pi, &second_pi->pi_lock);

    spin_lock_irqsave_nested(&first_access_histogram->histogram_lock, irq_flags, 3);
    spin_lock_nested(&second_access_histogram->histogram_lock, 2);
    spin_lock_nested(&first_pi->pi_lock, 1);
    spin_lock_nested(&second_pi->pi_lock, 0);

    // base_histogram_dump("access_histogram_exchange before from_access_histogram", from_access_histogram, true);
    // base_histogram_dump("access_histogram_exchange before to_access_histogram", to_access_histogram, true);

    // 实际的exchange操作
    {
        struct page_ext tmp_ext = {.flags = 0};
        struct page_info tmp_pi;
        unsigned int tmp_access_interval_sec;
        
#if 0
        static int debug_cnt = 0;
        if (debug_cnt < 10) {
            printk("before exchange: from_ext=(%d,%u), to_ext=(%d,%u)", PageHistogram(from_ext), from_pi->access_interval_sec,
                            PageHistogram(to_ext), to_pi->access_interval_sec);
        }
#endif
        // from_pi -> tmp_pi
        if (PageHistogram(from_ext)) {
            tmp_access_interval_sec = from_pi->access_interval_sec;
            // del from_pi
            ClearPageHistogram(from_ext);
            from_pi->access_interval_sec = 0;
            __base_histogram_delete(from_access_histogram, tmp_access_interval_sec);
            // save tmp_pi
            tmp_pi.access_interval_sec = tmp_access_interval_sec;
            SetPageHistogram(&tmp_ext);
        }

        // to_pi -> from_pi
        if (PageHistogram(to_ext)) {
            tmp_access_interval_sec = to_pi->access_interval_sec;
            // del to_pi
            ClearPageHistogram(to_ext);
            to_pi->access_interval_sec = 0;
            __base_histogram_delete(to_access_histogram, tmp_access_interval_sec);
            // save new from_pi
            __base_histogram_insert(from_access_histogram, tmp_access_interval_sec);
            from_pi->access_interval_sec = tmp_access_interval_sec;
            SetPageHistogram(from_ext);
        }

        if (PageHistogram(&tmp_ext)) {
            tmp_access_interval_sec = tmp_pi.access_interval_sec;
            // save new to_pi
            __base_histogram_insert(to_access_histogram, tmp_access_interval_sec);
            to_pi->access_interval_sec = tmp_pi.access_interval_sec;
            SetPageHistogram(to_ext);
        }

#if 0
        if (debug_cnt < 10) {
            printk("after exchange: from_ext=(%d,%u), to_ext=(%d,%u)", PageHistogram(from_ext), from_pi->access_interval_sec,
                            PageHistogram(to_ext), to_pi->access_interval_sec);
        }
#endif
    }

    // base_histogram_dump("access_histogram_exchange after from_access_histogram", from_access_histogram, true);
    // base_histogram_dump("access_histogram_exchange after to_access_histogram", to_access_histogram, true);

unlock:
    spin_unlock(&second_pi->pi_lock);
    spin_unlock(&first_pi->pi_lock);
    spin_unlock(&second_access_histogram->histogram_lock);
    spin_unlock_irqrestore(&first_access_histogram->histogram_lock, irq_flags);


    return ;
}

#endif // CONFIG_ACCESS_HISTOGRAM