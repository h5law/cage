#ifndef CAGE_CONTAINER_H
#define CAGE_CONTAINER_H

#include <sys/types.h>

struct container {
    const char *rootfs;
    char      **argv;

    pid_t pid;
};

int container_run(struct container *container);

#endif
