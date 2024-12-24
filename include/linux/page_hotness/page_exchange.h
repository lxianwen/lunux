#ifndef _LINUX_PAGE_EXCHANGE_H
#define _LINUX_PAGE_EXCHANGE_H

#ifdef CONFIG_PAGE_EXCHANGE
extern bool exchange_pages(struct page *from_page, struct page *to_page);
extern bool try_exchange_page(struct page *page, int target_id);
extern int exchange_by_migrate_twice(struct page *from_page, struct vm_area_struct *vma, int target_nid);
#else
static inline bool exchange_pages(struct page *from_page, struct page *to_page) { return false; }
static inline bool try_exchange_page(struct page *page, int target_id) { return false; }
static inline int exchange_by_migrate_twice(struct page *from_page, struct vm_area_struct *vma, int target_nid) { return 0; }
#endif // CONFIG_PAGE_EXCHANGE

#endif /* _LINUX_PAGE_EXCHANGE_H */