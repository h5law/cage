#include "container.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (1024 * 1024)

struct child_context {
    const char *rootfs;
    char      **argv;

    int sync_fd;
};

static int write_file(const char *path, const char *value)
{
    int     fd;
    size_t  len;
    ssize_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd == -1)
        return -1;

    len     = strlen(value);

    written = write(fd, value, len);
    if (written != ( ssize_t )len) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;

        return -1;
    }

    if (close(fd) == -1)
        return -1;

    return 0;
}

static int configure_user_namespace(pid_t pid)
{
    char path[128];
    char map[128];

    uid_t uid = getuid();
    gid_t gid = getgid();

    /*
     * An unprivileged process must disable setgroups before it can
     * write gid_map.
     */
    snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);

    if (write_file(path, "deny") == -1 && errno != ENOENT) {
        perror("write setgroups");
        return -1;
    }

    snprintf(map, sizeof(map), "0 %u 1", ( unsigned )uid);

    snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);

    if (write_file(path, map) == -1) {
        perror("write uid_map");
        return -1;
    }

    snprintf(map, sizeof(map), "0 %u 1", ( unsigned )gid);

    snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);

    if (write_file(path, map) == -1) {
        perror("write gid_map");
        return -1;
    }

    return 0;
}

static int make_mounts_private(void)
{
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("mount --make-rprivate /");
        return -1;
    }

    return 0;
}

static int wait_for_parent(struct child_context *ctx)
{
    char    byte;
    ssize_t result;

    do {
        result = read(ctx->sync_fd, &byte, sizeof(byte));
    } while (result == -1 && errno == EINTR);

    if (result != sizeof(byte)) {
        if (result == 0)
            errno = ECANCELED;

        return -1;
    }

    close(ctx->sync_fd);
    ctx->sync_fd = -1;

    return 0;
}

static int child_main(void *arg)
{
    struct child_context *ctx = arg;

    /*
     * Do not perform any namespace-dependent setup until the parent
     * has established the UID/GID mappings.
     */
    if (wait_for_parent(ctx) == -1) {
        perror("wait for namespace setup");
        return 1;
    }

    if (make_mounts_private() == -1)
        return 1;

    if (chroot(ctx->rootfs) == -1) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }

    execvp(ctx->argv[0], ctx->argv);

    perror("exec");
    return 127;
}

int container_run(struct container *container)
{
    struct child_context ctx;
    char                *stack;
    char                *stack_top;
    int                  sync_pipe[2];
    int                  status;
    char                 ready = 1;

    if (container == NULL || container->rootfs == NULL ||
        container->argv == NULL || container->argv[0] == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (pipe2(sync_pipe, O_CLOEXEC) == -1) {
        perror("pipe2");
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        perror("malloc");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return -1;
    }

    ctx = (struct child_context){
            .rootfs  = container->rootfs,
            .argv    = container->argv,
            .sync_fd = sync_pipe[0],
    };

    stack_top      = stack + STACK_SIZE;

    container->pid = clone(child_main, stack_top,
                           CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS |
                                   CLONE_NEWNET | CLONE_NEWIPC | SIGCHLD,
                           &ctx);

    close(sync_pipe[0]);

    if (container->pid == -1) {
        perror("clone");
        free(stack);
        close(sync_pipe[1]);
        return -1;
    }

    if (configure_user_namespace(container->pid) == -1) {
        kill(container->pid, SIGKILL);
        close(sync_pipe[1]);
        waitpid(container->pid, NULL, 0);
        free(stack);

        return -1;
    }

    if (write(sync_pipe[1], &ready, sizeof(ready)) != sizeof(ready)) {
        perror("release container");
        kill(container->pid, SIGKILL);
        close(sync_pipe[1]);
        waitpid(container->pid, NULL, 0);
        free(stack);

        return -1;
    }

    close(sync_pipe[1]);

    if (waitpid(container->pid, &status, 0) == -1) {
        perror("waitpid");
        free(stack);
        return -1;
    }

    free(stack);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
