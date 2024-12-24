#ifndef __LINUX_PAGE_PROC_STAT_H
#define __LINUX_PAGE_PROC_STAT_H

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>

extern int my_stat_init(void);
extern void my_stat_exit(void);

#endif /* __LINUX_PAGE_PROC_STAT_H */