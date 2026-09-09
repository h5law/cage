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

static volatile sig_atomic_t command_pid   = -1;
static volatile sig_atomic_t container_pid = -1;

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

    while (!deadline_expired(&deadline)) {
        int   status;
        pid_t pid;

        pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0)
            continue;

        if (pid == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                return 0;

            perror("waitpid");
            return -1;
        }

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
     *
     * kill(-1, ...) targets every process we are permitted to signal
     * in this PID namespace except the caller itself.
     */
    if (kill(-1, SIGKILL) == -1 && errno != ESRCH) {
        perror("kill descendants");
        return -1;
    }

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

static void forward_command_signal(int signal)
{
    pid_t pid = ( pid_t )command_pid;

    if (pid > 0)
        kill(pid, signal);
}

static int install_command_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_command_signal;
    sigemptyset(&action.sa_mask);

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

static void reset_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);

    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
}

static int unblock_forwarded_signals(void)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGQUIT);

    return sigprocmask(SIG_UNBLOCK, &set, NULL);
}

static int run_command(struct child_context *ctx)
{
    pid_t pid;
    int   status;

    /*
     * Signals are blocked here so cage-init cannot receive a
     * forwarded signal between fork() and recording the child's PID.
     */
    pid = fork();

    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /*
         * The workload must receive normal signal semantics rather
         * than inheriting cage-init's forwarding handlers.
         */
        reset_signal_handlers();

        if (unblock_forwarded_signals() == -1)
            _exit(127);

        execvp(ctx->argv[0], ctx->argv);

        perror("exec");
        _exit(127);
    }

    command_pid = pid;

    /*
     * The child PID is now published. Signals can safely be
     * delivered to cage-init.
     */
    if (unblock_forwarded_signals() == -1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        command_pid = -1;
        return 1;
    }

    /*
     * PID 1 must reap every child it owns.
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
            int command_status;

            command_pid = -1;

            if (WIFEXITED(status))
                command_status = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                command_status = 128 + WTERMSIG(status);
            else
                command_status = 1;

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
     * Signals remain blocked until the parent has established the
     * UID/GID mappings and released us.
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

    if (install_command_signal_handlers() == -1) {
        perror("install signal handlers");
        return 1;
    }

    status = run_command(ctx);

    return status;
}

static void forward_container_signal(int signal)
{
    pid_t pid = ( pid_t )container_pid;

    if (pid > 0)
        kill(pid, signal);
}

static int install_container_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_container_signal;
    sigemptyset(&action.sa_mask);

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

static void reset_container_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);

    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
}

static int block_forwarded_signals(sigset_t *old_mask)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGQUIT);

    return sigprocmask(SIG_BLOCK, &set, old_mask);
}

static int restore_signal_mask(const sigset_t *mask)
{
    return sigprocmask(SIG_SETMASK, mask, NULL);
}

int container_run(struct container *container)
{
    struct child_context ctx;
    sigset_t             old_mask;
    char                *stack;
    char                *stack_top;
    int                  sync_pipe[2];
    int                  status;
    char                 ready              = 1;
    int                  signals_blocked    = 0;
    int                  handlers_installed = 0;

    if (container == NULL || container->rootfs == NULL ||
        container->argv == NULL || container->argv[0] == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (block_forwarded_signals(&old_mask) == -1) {
        perror("sigprocmask");
        return -1;
    }

    signals_blocked = 1;

    if (pipe2(sync_pipe, O_CLOEXEC) == -1) {
        perror("pipe2");
        restore_signal_mask(&old_mask);
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (stack == NULL) {
        perror("malloc");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        restore_signal_mask(&old_mask);
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
        restore_signal_mask(&old_mask);
        return -1;
    }

    /*
     * container->pid is now valid while signals are still blocked.
     * Install forwarding before allowing signals through.
     */
    container_pid = container->pid;

    if (install_container_signal_handlers() == -1) {
        perror("install signal handlers");
        kill(container->pid, SIGKILL);
        close(sync_pipe[1]);
        waitpid(container->pid, NULL, 0);
        container_pid = -1;
        free(stack);
        restore_signal_mask(&old_mask);
        return -1;
    }

    handlers_installed = 1;

    if (configure_user_namespace(container->pid) == -1) {
        kill(container->pid, SIGKILL);
        close(sync_pipe[1]);
        waitpid(container->pid, NULL, 0);
        container_pid = -1;
        reset_container_signal_handlers();
        free(stack);
        restore_signal_mask(&old_mask);
        return -1;
    }

    /*
     * Release cage-init once its identity has been configured.
     */
    if (write(sync_pipe[1], &ready, sizeof(ready)) != sizeof(ready)) {
        perror("release container");
        kill(container->pid, SIGKILL);
        close(sync_pipe[1]);
        waitpid(container->pid, NULL, 0);
        container_pid = -1;
        reset_container_signal_handlers();
        free(stack);
        restore_signal_mask(&old_mask);
        return -1;
    }

    close(sync_pipe[1]);

    /*
     * The container is fully established. Signals can now reach
     * the forwarding handler.
     */
    if (restore_signal_mask(&old_mask) == -1) {
        perror("sigprocmask");
        kill(container->pid, SIGKILL);
        waitpid(container->pid, NULL, 0);
        container_pid = -1;

        if (handlers_installed)
            reset_container_signal_handlers();

        free(stack);
        return -1;
    }

    signals_blocked = 0;

    if (waitpid(container->pid, &status, 0) == -1) {
        if (errno == EINTR) {
            /*
             * A forwarded signal interrupts waitpid(). Continue
             * waiting for the container to terminate.
             */
            for (;;) {
                if (waitpid(container->pid, &status, 0) != -1)
                    break;

                if (errno != EINTR) {
                    perror("waitpid");
                    container_pid = -1;
                    reset_container_signal_handlers();
                    free(stack);
                    return -1;
                }
            }
        } else {
            perror("waitpid");
            container_pid = -1;
            reset_container_signal_handlers();
            free(stack);
            return -1;
        }
    }

    container_pid = -1;

    if (handlers_installed)
        reset_container_signal_handlers();

    free(stack);

    if (signals_blocked)
        restore_signal_mask(&old_mask);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
