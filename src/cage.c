#include "container.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static int validate_rootfs(const char *rootfs)
{
    struct stat st;

    if (stat(rootfs, &st) == -1) {
        perror("rootfs");
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cage: rootfs '%s' is not a directory\n", rootfs);

        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: cage <rootfs> <command> [args...]\n");
}

int main(int argc, char **argv)
{
    struct container container;
    int              status;

    if (argc < 3) {
        usage();
        return EXIT_FAILURE;
    }

    if (validate_rootfs(argv[1]) == -1)
        return EXIT_FAILURE;

    container = (struct container){
            .rootfs         = argv[1],
            .argv           = &argv[2],
            .pid            = -1,
            .private_dir    = {0},
            .overlay_upper  = {0},
            .overlay_work   = {0},
            .overlay_root   = {0},
            .private_dir_fd = -1,
    };

    status = container_run(&container);

    if (status < 0)
        return EXIT_FAILURE;

    return status;
}
