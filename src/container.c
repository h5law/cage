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
#include <time.h>
#include <unistd.h>

#define STACK_SIZE           (1024 * 1024)
#define TERMINATION_GRACE_MS 1000

struct child_context {
    const char *rootfs;
    char      **argv;

    int sync_fd;
};

static volatile sig_atomic_t command_pid = -1;

static void forward_signal(int signal)
{
    pid_t pid = ( pid_t )command_pid;

    if (pid > 0)
        kill(pid, signal);
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = forward_signal;

    if (sigemptyset(&action.sa_mask) == -1)
        return -1;

    if (sigaction(SIGTERM, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGINT, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGHUP, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGQUIT, &action, NULL) == -1)
        return -1;

    return 0;
}

static int reset_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = SIG_DFL;

    if (sigemptyset(&action.sa_mask) == -1)
        return -1;

    if (sigaction(SIGTERM, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGINT, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGHUP, &action, NULL) == -1)
        return -1;

    if (sigaction(SIGQUIT, &action, NULL) == -1)
        return -1;

    return 0;
}

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
     * An unprivileged process must disable setgroups before it
     * can write gid_map.
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

static int deadline_expired(const struct timespec *deadline)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
        return 1;

    if (now.tv_sec > deadline->tv_sec)
        return 1;

    if (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)
        return 1;

    return 0;
}

static int terminate_descendants(void)
{
    struct timespec deadline;
    int             remaining = 1;

    /*
     * Send SIGTERM to every process in this PID namespace except
     * cage-init itself.
     */
    if (kill(-1, SIGTERM) == -1 && errno != ESRCH) {
        perror("terminate descendants");
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) == -1) {
        perror("clock_gettime");
        return -1;
    }

    deadline.tv_nsec += TERMINATION_GRACE_MS * 1000000L;

    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec  += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }

    /*
     * Reap children as they terminate. WNOHANG lets us check the
     * deadline without blocking indefinitely on a process that
     * refuses to exit.
     */
    while (!deadline_expired(&deadline)) {
        int   status;
        pid_t pid;

        pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0) {
            remaining = 0;
            continue;
        }

        if (pid == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                return 0;

            perror("waitpid");
            return -1;
        }

        remaining = 1;

        {
            struct timespec interval = {
                    .tv_sec  = 0,
                    .tv_nsec = 10000000L,
            };

            while (nanosleep(&interval, &interval) == -1) {
                if (errno != EINTR)
                    break;
            }
        }
    }

    /*
     * Anything still alive after the grace period gets SIGKILL.
     */
    if (remaining) {
        if (kill(-1, SIGKILL) == -1 && errno != ESRCH) {
            perror("kill descendants");
            return -1;
        }
    }

    /*
     * SIGKILL guarantees that remaining descendants will eventually
     * terminate. Reap all of them before cage-init exits.
     */
    for (;;) {
        int   status;
        pid_t pid;

        pid = waitpid(-1, &status, 0);

        if (pid == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                break;

            perror("waitpid");
            return -1;
        }
    }

    return 0;
}

static int run_command(struct child_context *ctx)
{
    pid_t pid;
    int   status;
    int   command_status;

    if (install_signal_handlers() == -1) {
        perror("install signal handlers");
        return 1;
    }

    pid = fork();

    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /*
         * The workload must not inherit cage-init's signal
         * forwarding handlers.
         */
        if (reset_signal_handlers() == -1) {
            perror("reset signal handlers");
            _exit(127);
        }

        execvp(ctx->argv[0], ctx->argv);

        perror("exec");
        _exit(127);
    }

    command_pid = pid;

    /*
     * PID 1 must reap every child it owns. For now there is only
     * the requested command, but this also gives us the correct
     * primitive for orphaned descendants.
     */
    for (;;) {
        pid_t waited = waitpid(-1, &status, 0);

        if (waited == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                break;

            perror("waitpid");
            command_pid = -1;
            return 1;
        }

        if (waited == pid) {
            if (WIFEXITED(status))
                command_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                command_status = 128 + WTERMSIG(status);
            else
                command_status = 1;

            command_pid = -1;

            /*
             * The requested command has exited. Any remaining
             * processes are descendants that must not survive
             * container teardown.
             */
            if (terminate_descendants() == -1)
                return 1;

            return command_status;
        }
    }

    command_pid = -1;

    return 1;
}

static int child_main(void *arg)
{
    struct child_context *ctx = arg;
    int                   status;

    /*
     * Wait until the parent has established the UID/GID mappings.
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

    if (chdir("/") == -1)
        return 1;

    /*
     * We are PID 1 inside the new PID namespace.
     * Run the requested command as our child rather than replacing
     * ourselves with it.
     */
    status = run_command(ctx);

    return status;
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

    /*
     * Release the cage-init process once its identity has
     * been configured.
     */
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
