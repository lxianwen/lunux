#ifndef __LINUX_CANDIDATE_PAGE_H
#define __LINUX_CANDIDATE_PAGE_H

#ifdef CONFIG_CANDIDATE_PAGE
extern bool candidate_page_empty(struct pglist_data *pgdat);
extern bool in_candidate_pages(struct page *page);
extern bool del_candidate_page(struct page *page);
extern bool enqueue_candidate_page(struct page *page);
extern struct page* dequeue_candidate_page(struct pglist_data *pgdat);
#else
static inline bool candidate_page_empty(struct pglist_data *pgdat) { return true; }
static inline bool in_candidate_pages(struct page *page) { return false; }
static inline bool del_candidate_page(struct page *page) { return false; }
static inline bool enqueue_candidate_page(struct page *page) { return false; }
static inline struct page* dequeue_candidate_page(struct pglist_data *pgdat) { return NULL; }
#endif // CONFIG_CANDIDATE_PAGE

#endif