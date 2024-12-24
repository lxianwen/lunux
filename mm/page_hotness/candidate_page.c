#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/types.h>
#include <linux/page_ext.h>
#include <linux/nodemask.h>
#include <linux/bug.h>
#include <linux/page_ext.h>

#include <linux/page_hotness/page_info.h>
#include <linux/page_hotness/page_hotness.h>
#include <linux/page_common/page_hotness_stat.h>
#include <linux/page_common/page_hotness_sysctl.h>
#include <linux/page_hotness/candidate_page.h>

#ifdef CONFIG_CANDIDATE_PAGE
#if 0
static int check_candidate_pages_len(char *name, struct pglist_data *pgdat)
{
	int size = 0;
	struct page_info *pi = NULL;
    unsigned long irq_flags;

	spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);
	list_for_each_entry (pi, &pgdat->candidate_pages, candidate_page_node) {
		size ++;
	}
	if (pgdat->candidate_pages_len != size) {
		printk("bug: %s", name);
		printk("size=%d, candidate_pages_len=%d", size, pgdat->candidate_pages_len);
	}
	BUG_ON(pgdat->candidate_pages_len != size);
    spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);

	return size;
}
#else
static inline int check_candidate_pages_len(char *name, struct pglist_data *pgdat)
{
	return 0;
}
#endif

bool candidate_page_empty(struct pglist_data *pgdat)
{
	bool is_empty = false;
    unsigned long irq_flags;

	check_candidate_pages_len("candidate_page_empty before", pgdat);

	spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);
    is_empty = pgdat->candidate_pages_len == 0;
#if 1
    {
        bool test_empty = false;
        if (list_empty(&pgdat->candidate_pages)) {
            test_empty = true;
        }
        WARN_ON(is_empty != test_empty);
    }
#endif
    spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);

	check_candidate_pages_len("candidate_page_empty after", pgdat);

	return is_empty;
}

bool in_candidate_pages(struct page *page)
{
    struct page_ext *page_ext = NULL;
	struct page_info *pi = NULL;
	bool rc = false;
    unsigned long irq_flags;

	check_candidate_pages_len("in_candidate_pages before", page_pgdat(page));

    page_ext = lookup_page_ext(page);
    if (!page_ext || !PageTracked(page_ext))
        return false;

    pi = get_page_info(page_ext);
	if (!pi)
		return false;

    spin_lock_irqsave(&pi->pi_lock, irq_flags);
	if (PageCandidate(page_ext)) {
		rc = true;
	}
    spin_unlock_irqrestore(&pi->pi_lock, irq_flags);

#if 0
	{
		bool test_rc = false;
        struct pglist_data *pgdat = page_pgdat(page);
        spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);
	
		list_for_each_entry (pi, &pgdat->candidate_pages, candidate_page_node) {
			// printk("pi=%px, pi->page=%px, page=%px", pi, pi->page, page);
			if (pi->page == page) {
				test_rc = true;
				break;
			}
		}
		spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);
		WARN_ON(rc != test_rc);
	}
#endif

	check_candidate_pages_len("in_candidate_pages after", page_pgdat(page));

	return rc;
}

bool del_candidate_page(struct page *page)
{
	struct pglist_data *pgdat = page_pgdat(page);
	struct page_ext *page_ext = NULL;
	struct page_info *pi = NULL;
	unsigned long irq_flags;

    page_ext = lookup_page_ext(page);
    if (!page_ext || !PageTracked(page_ext))
        return false;

    pi = get_page_info(page_ext);
	if (!pi)
		return false;

	check_candidate_pages_len("del_candidate_page before", pgdat);

    spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);
    spin_lock(&pi->pi_lock);

	if (!PageCandidate(page_ext)) {
		printk("del_candidate_page: !PageCandidate");
		spin_unlock(&pi->pi_lock);
    	spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);
		return false;
	}
	list_del_init(&pi->candidate_page_node);
	pgdat->candidate_pages_len --;
	ClearPageCandidate(lookup_page_ext(page));

    spin_unlock(&pi->pi_lock);
    spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);
    smp_mb__after_unlock_lock();

	check_candidate_pages_len("del_candidate_page after", pgdat);
	// printk("del_candidate_page, page=%px, nid=%d, size=%d", page, pgdat->node_id, get_candidate_pages_len(pgdat));

	return true;
}

bool enqueue_candidate_page(struct page *page)
{
    struct page_ext *page_ext;
	struct page_info *pi;
	struct pglist_data *pgdat = page_pgdat(page);
    unsigned long irq_flags;
    bool rc = false;

    page_ext = lookup_page_ext(page);
    if (!page_ext || !PageTracked(page_ext))
        return false;

    pi = get_page_info(page_ext);
	if (!pi) {
		printk("warn: add_candidate_page: !pi");
		return false;
	}

	check_candidate_pages_len("get_candidate_page before", pgdat);

    spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);
    spin_lock(&pi->pi_lock);

	if (PageCandidate(lookup_page_ext(page))) {
		goto unlock;
	}

	// 加入队列
	list_add_tail(&pi->candidate_page_node, &pgdat->candidate_pages);
	// 更新pi元数据
	pi->page = page;
	// 更新队列统计数据
	pgdat->candidate_pages_len ++;
    // 最后更新flag
	SetPageCandidate(lookup_page_ext(page));
    rc = true;

unlock:
    spin_unlock(&pi->pi_lock);
    spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);
    if (rc)
        smp_mb__after_unlock_lock();

	check_candidate_pages_len("get_candidate_page after", pgdat);

	return page;
}

struct page* dequeue_candidate_page(struct pglist_data *pgdat)
{
	struct page_ext *page_ext;
	struct page_info *pi;
    unsigned long irq_flags;
    struct page *page = NULL; // 此处的page需要从candidate page中取出

	check_candidate_pages_len("dequeue_candidate_page before", pgdat);
    spin_lock_irqsave(&pgdat->candidate_pages_lock, irq_flags);

	// 检查list情况
	if (list_empty(&pgdat->candidate_pages)) {
		goto unlock;
	}

	// 找到合适的pi与page
	pi = list_first_entry_or_null(&pgdat->candidate_pages, struct page_info, candidate_page_node);
	if (!pi) {
		goto unlock;
	}
	page = pi->page;
	page_ext = lookup_page_ext(page);
	WARN_ON(!PageCandidate(page_ext));

	// 从列表中移除，并重置flag
	list_del_init(&pi->candidate_page_node);
	pgdat->candidate_pages_len --;
	ClearPageCandidate(page_ext);

unlock:
    spin_unlock_irqrestore(&pgdat->candidate_pages_lock, irq_flags);
    if (!page)
        smp_mb__after_unlock_lock();
	check_candidate_pages_len("dequeue_candidate_page after", pgdat);

	return page;
}

#endif // CONFIG_CANDIDATE_PAGE