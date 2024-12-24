#ifndef __LINUX_GUPS_TEST_H
#define __LINUX_GUPS_TEST_H

#include <linux/sched.h>
#include <uapi/linux/perf_event.h>

// CONFIG_PAGE_GUPS_TEST
struct area {
    uint64_t start;
    uint64_t end;
};
struct area_warp {
    struct area *gups_areas;
    size_t len_gups_areas;
    struct area *hot_areas;
    size_t len_hot_area;
};
enum gups_test_type {
    GUPS_TEST_PROMOTE,
    GUPS_TEST_DEMOTE,
    GUPS_TEST_PREDICT,
};

extern struct area_warp area_warp;
#ifdef CONFIG_PAGE_GUPS_TEST
bool addr_in_hotset(uint64_t address, enum gups_test_type type);
void dump_whole_memory_layout(void);
#else
static inline bool addr_in_hotset(uint64_t address, enum gups_test_type type) {return false;}
static inline void dump_whole_memory_layout(void) { }
#endif

#endif /* __LINUX_GUPS_TEST_H */
