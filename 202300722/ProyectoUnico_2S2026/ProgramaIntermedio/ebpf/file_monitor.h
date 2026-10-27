#ifndef FILE_MONITOR_H
#define FILE_MONITOR_H

#ifndef __VMLINUX_H__
#include <linux/types.h>
#endif

#define FILE_EVENT_PATH_LEN 256

struct file_event {
    __u32 pid;
    __u32 action;
    char path[FILE_EVENT_PATH_LEN];
};

enum file_action {
    FILE_ACTION_OPEN_WRITE = 1,
    FILE_ACTION_OPEN_CREATE = 2
};

#endif