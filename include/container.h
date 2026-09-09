#ifndef CAGE_CONTAINER_H
#define CAGE_CONTAINER_H

#include <limits.h>
#include <sys/types.h>

struct container {
    const char *rootfs;
    char      **argv;
    pid_t       pid;

    char private_dir[PATH_MAX];
    char overlay_upper[PATH_MAX];
    char overlay_work[PATH_MAX];
    char overlay_root[PATH_MAX];

    int private_dir_fd;
};

int container_run(struct container *container);

#endif
