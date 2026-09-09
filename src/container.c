#include "container.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STACK_SIZE           (1024 * 1024)
#define TERMINATION_GRACE_MS 1000

#ifndef CAGE_RUNTIME_DIR
#define CAGE_RUNTIME_DIR "/tmp"
#endif

static int create_private_dir(struct container *container)
{
    char template[PATH_MAX];

    if (snprintf(template, sizeof(template), "%s/cage-XXXXXX",
                 CAGE_RUNTIME_DIR) >= ( int )sizeof(template)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mkdtemp(template) == NULL)
        return -1;

    if (snprintf(container->private_dir, sizeof(container->private_dir), "%s",
                 template) >= ( int )sizeof(container->private_dir)) {
        rmdir(template);
        errno = ENAMETOOLONG;
        return -1;
    }

    container->private_dir_fd =
            open(container->private_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (container->private_dir_fd == -1) {
        int saved_errno = errno;

        rmdir(container->private_dir);
        container->private_dir[0] = '\0';

        errno                     = saved_errno;
        return -1;
    }

    if (flock(container->private_dir_fd, LOCK_EX | LOCK_NB) == -1) {
        int saved_errno = errno;

        close(container->private_dir_fd);
        rmdir(container->private_dir);

        container->private_dir_fd = -1;
        container->private_dir[0] = '\0';

        errno                     = saved_errno;
        return -1;
    }

    return 0;
}

static void destroy_private_dir(struct container *container)
{
    if (container->private_dir_fd != -1) {
        flock(container->private_dir_fd, LOCK_UN);
        close(container->private_dir_fd);
        container->private_dir_fd = -1;
    }

    if (container->private_dir[0] != '\0') {
        rmdir(container->private_dir);
        container->private_dir[0] = '\0';
    }
}

struct child_context {
    const char *rootfs;
    char      **argv;

    int sync_fd;
};

static volatile sig_atomic_t command_pid   = -1;
static volatile sig_atomic_t container_pid = -1;

static int write_file(const char *path, const char *value)
{
    int    fd;
    size_t len;
    size_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);

    if (fd == -1)
        return -1;

    len     = strlen(value);
    written = 0;

    while (written < len) {
        ssize_t result;

        result = write(fd, value + written, len - written);

        if (result == -1) {
            if (errno == EINTR)
                continue;

            {
                int saved_errno = errno;

                close(fd);
                errno = saved_errno;
            }

            return -1;
        }

        if (result == 0) {
            close(fd);
            errno = EIO;
            return -1;
        }

        written += ( size_t )result;
    }

    if (close(fd) == -1)
        return -1;

    return 0;
}

static int configure_user_namespace(pid_t pid)
{
    char  path[128];
    char  map[128];
    uid_t uid;
    gid_t gid;
    int   length;

    uid    = getuid();
    gid    = getgid();

    length = snprintf(path, sizeof(path), "/proc/%ld/setgroups", ( long )pid);

    if (length < 0 || ( size_t )length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (write_file(path, "deny") == -1 && errno != ENOENT) {
        perror("write setgroups");
        return -1;
    }

    length = snprintf(map, sizeof(map), "0 %lu 1", ( unsigned long )uid);

    if (length < 0 || ( size_t )length >= sizeof(map)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    length = snprintf(path, sizeof(path), "/proc/%ld/uid_map", ( long )pid);

    if (length < 0 || ( size_t )length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (write_file(path, map) == -1) {
        perror("write uid_map");
        return -1;
    }

    length = snprintf(map, sizeof(map), "0 %lu 1", ( unsigned long )gid);

    if (length < 0 || ( size_t )length >= sizeof(map)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    length = snprintf(path, sizeof(path), "/proc/%ld/gid_map", ( long )pid);

    if (length < 0 || ( size_t )length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

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

    if (close(ctx->sync_fd) == -1) {
        ctx->sync_fd = -1;
        return -1;
    }

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
     * cage-init is PID 1 in the container PID namespace.
     * kill(-1, ...) therefore targets every other process that
     * cage-init is permitted to signal.
     */
    if (kill(-1, SIGTERM) == -1 && errno != ESRCH) {
        /*
         * Failure to signal one or more descendants must not prevent
         * the final SIGKILL/reap phase from running.
         */
        perror("terminate descendants");
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

    for (;;) {
        int   status;
        pid_t pid;

        if (deadline_expired(&deadline))
            break;

        pid = waitpid(-1, &status, WNOHANG);

        if (pid > 0)
            continue;

        if (pid == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD)
                return 0;

            perror("waitpid");
            break;
        }

        {
            struct timespec interval = {
                    .tv_sec  = 0,
                    .tv_nsec = 10000000L,
            };

            while (nanosleep(&interval, NULL) == -1) {
                if (errno != EINTR)
                    break;
            }
        }
    }

    /*
     * Anything still alive after the grace period must not survive
     * the container.
     */
    if (kill(-1, SIGKILL) == -1 && errno != ESRCH)
        perror("kill descendants");

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
    int   saved_errno;
    pid_t pid;

    saved_errno = errno;
    pid         = ( pid_t )command_pid;

    if (pid > 0)
        ( void )kill(pid, signal);

    errno = saved_errno;
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

    ( void )sigaction(SIGTERM, &action, NULL);
    ( void )sigaction(SIGINT, &action, NULL);
    ( void )sigaction(SIGHUP, &action, NULL);
    ( void )sigaction(SIGQUIT, &action, NULL);
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

static int wait_for_command(pid_t command, int *command_status)
{
    for (;;) {
        int   status;
        pid_t pid;

        pid = waitpid(-1, &status, 0);

        if (pid == -1) {
            if (errno == EINTR)
                continue;

            if (errno == ECHILD) {
                errno = ECHILD;
                return -1;
            }

            perror("waitpid");
            return -1;
        }

        if (pid != command)
            continue;

        if (WIFEXITED(status)) {
            *command_status = WEXITSTATUS(status);
            return 0;
        }

        if (WIFSIGNALED(status)) {
            *command_status = 128 + WTERMSIG(status);
            return 0;
        }

        *command_status = 1;
        return 0;
    }
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
        reset_signal_handlers();

        if (unblock_forwarded_signals() == -1)
            _exit(127);

        execvp(ctx->argv[0], ctx->argv);

        perror("exec");
        _exit(127);
    }

    command_pid = pid;

    /*
     * The command PID is now published, so cage-init can safely
     * receive forwarded signals.
     */
    if (unblock_forwarded_signals() == -1) {
        perror("sigprocmask");

        if (kill(pid, SIGKILL) == -1 && errno != ESRCH)
            perror("kill command");

        while (waitpid(pid, NULL, 0) == -1) {
            if (errno != EINTR)
                break;
        }

        command_pid = -1;

        return 1;
    }

    if (wait_for_command(pid, &status) == -1) {
        command_pid = -1;
        return 1;
    }

    command_pid = -1;

    /*
     * The requested command has exited. Any remaining processes
     * belong to the command's descendant tree and must be removed.
     */
    if (terminate_descendants() == -1)
        return 1;

    return status;
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

    if (chdir("/") == -1) {
        perror("chdir");
        return 1;
    }

    if (install_command_signal_handlers() == -1) {
        perror("install signal handlers");
        return 1;
    }

    status = run_command(ctx);

    return status;
}

static void forward_container_signal(int signal)
{
    int   saved_errno;
    pid_t pid;

    saved_errno = errno;
    pid         = ( pid_t )container_pid;

    if (pid > 0)
        ( void )kill(pid, signal);

    errno = saved_errno;
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

    ( void )sigaction(SIGTERM, &action, NULL);
    ( void )sigaction(SIGINT, &action, NULL);
    ( void )sigaction(SIGHUP, &action, NULL);
    ( void )sigaction(SIGQUIT, &action, NULL);
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

static void kill_and_reap_container(pid_t pid)
{
    if (kill(pid, SIGKILL) == -1 && errno != ESRCH)
        perror("kill container");

    for (;;) {
        if (waitpid(pid, NULL, 0) != -1)
            break;

        if (errno == EINTR)
            continue;

        if (errno == ECHILD || errno == ESRCH)
            break;

        perror("waitpid");
        break;
    }
}

int container_run(struct container *container)
{
    struct child_context ctx;
    sigset_t             old_mask;
    char                *stack;
    char                *stack_top;
    int                  sync_pipe[2];
    int                  status;
    char                 ready;
    int                  handlers_installed;

    if (container == NULL || container->rootfs == NULL ||
        container->argv == NULL || container->argv[0] == NULL) {
        errno = EINVAL;
        return -1;
    }

    container->pid = -1;
    command_pid    = -1;
    container_pid  = -1;

    if (create_private_dir(container) == -1) {
        perror("create private directory");
        return -1;
    }

    if (block_forwarded_signals(&old_mask) == -1) {
        perror("sigprocmask");
        destroy_private_dir(container);
        return -1;
    }

    if (pipe2(sync_pipe, O_CLOEXEC) == -1) {
        perror("pipe2");
        destroy_private_dir(container);
        ( void )restore_signal_mask(&old_mask);
        return -1;
    }

    stack = malloc(STACK_SIZE);

    if (stack == NULL) {
        perror("malloc");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        destroy_private_dir(container);
        ( void )restore_signal_mask(&old_mask);
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
        destroy_private_dir(container);
        ( void )restore_signal_mask(&old_mask);
        return -1;
    }

    container_pid      = container->pid;
    handlers_installed = 0;

    /*
     * Install forwarding before allowing signals through.
     */
    if (install_container_signal_handlers() == -1) {
        perror("install signal handlers");
        kill_and_reap_container(container->pid);
        close(sync_pipe[1]);
        destroy_private_dir(container);
        container_pid  = -1;
        container->pid = -1;
        free(stack);
        ( void )restore_signal_mask(&old_mask);
        return -1;
    }

    handlers_installed = 1;

    if (configure_user_namespace(container->pid) == -1) {
        kill_and_reap_container(container->pid);
        close(sync_pipe[1]);
        destroy_private_dir(container);
        container_pid  = -1;
        container->pid = -1;
        reset_container_signal_handlers();
        free(stack);
        ( void )restore_signal_mask(&old_mask);
        return -1;
    }

    ready = 1;

    if (write(sync_pipe[1], &ready, sizeof(ready)) != sizeof(ready)) {
        int saved_errno = errno;

        perror("release container");

        kill_and_reap_container(container->pid);
        close(sync_pipe[1]);

        destroy_private_dir(container);

        container_pid  = -1;
        container->pid = -1;

        if (handlers_installed)
            reset_container_signal_handlers();

        free(stack);
        ( void )restore_signal_mask(&old_mask);

        errno = saved_errno;
        return -1;
    }

    if (close(sync_pipe[1]) == -1) {
        int saved_errno = errno;

        perror("close sync pipe");

        kill_and_reap_container(container->pid);

        destroy_private_dir(container);

        container_pid  = -1;
        container->pid = -1;

        if (handlers_installed)
            reset_container_signal_handlers();

        free(stack);
        ( void )restore_signal_mask(&old_mask);

        errno = saved_errno;
        return -1;
    }

    /*
     * The container is fully established. Signals can now reach
     * the host-side forwarding handler.
     */
    if (restore_signal_mask(&old_mask) == -1) {
        perror("sigprocmask");

        kill_and_reap_container(container->pid);
        destroy_private_dir(container);

        container_pid  = -1;
        container->pid = -1;

        if (handlers_installed)
            reset_container_signal_handlers();

        free(stack);
        return -1;
    }

    /*
     * Forwarded signals interrupt waitpid(), so always retry.
     */
    for (;;) {
        if (waitpid(container->pid, &status, 0) != -1)
            break;

        if (errno == EINTR)
            continue;

        perror("waitpid");

        kill_and_reap_container(container->pid);
        destroy_private_dir(container);

        container_pid  = -1;
        container->pid = -1;

        if (handlers_installed)
            reset_container_signal_handlers();

        free(stack);
        return -1;
    }

    destroy_private_dir(container);

    container_pid  = -1;
    container->pid = -1;

    if (handlers_installed)
        reset_container_signal_handlers();

    free(stack);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return 1;
}
