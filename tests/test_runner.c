#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_ROOTFS_TEMPLATE "/tmp/cage-test-rootfs-XXXXXX"
#define TEST_CONFIG_TEMPLATE "/tmp/cage-test-config-XXXXXX"

static int tests_run;
static int tests_failed;

static void test_pass(const char *name)
{
    tests_run++;
    printf("PASS: %s\n", name);
}

static void test_fail(const char *name, const char *reason)
{
    tests_run++;
    tests_failed++;
    printf("FAIL: %s: %s\n", name, reason);
}

static int wait_status(pid_t pid)
{
    int status;

    for (;;) {
        if (waitpid(pid, &status, 0) != -1)
            break;

        if (errno == EINTR)
            continue;

        return -1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

static int copy_file(const char *source, const char *destination)
{
    int     source_fd;
    int     destination_fd;
    char    buffer[8192];
    ssize_t count;

    source_fd = open(source, O_RDONLY);

    if (source_fd == -1)
        return -1;

    destination_fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0755);

    if (destination_fd == -1) {
        close(source_fd);
        return -1;
    }

    for (;;) {
        count = read(source_fd, buffer, sizeof(buffer));

        if (count == 0)
            break;

        if (count == -1) {
            if (errno == EINTR)
                continue;

            close(source_fd);
            close(destination_fd);
            return -1;
        }

        {
            ssize_t written = 0;

            while (written < count) {
                ssize_t result;

                result = write(destination_fd, buffer + written,
                               ( size_t )(count - written));

                if (result == -1) {
                    if (errno == EINTR)
                        continue;

                    close(source_fd);
                    close(destination_fd);
                    return -1;
                }

                written += result;
            }
        }
    }

    if (close(source_fd) == -1) {
        close(destination_fd);
        return -1;
    }

    if (close(destination_fd) == -1)
        return -1;

    return 0;
}

static int create_rootfs(const char *probe, char *rootfs, size_t size)
{
    char tests_path[PATH_MAX];
    char probe_path[PATH_MAX];

    if (strlen(TEST_ROOTFS_TEMPLATE) + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(rootfs, TEST_ROOTFS_TEMPLATE);

    if (mkdtemp(rootfs) == NULL)
        return -1;

    if (snprintf(tests_path, sizeof(tests_path), "%s/tests", rootfs) >=
        ( int )sizeof(tests_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (mkdir(tests_path, 0755) == -1)
        return -1;

    if (snprintf(probe_path, sizeof(probe_path), "%s/tests/container_probe",
                 rootfs) >= ( int )sizeof(probe_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (copy_file(probe, probe_path) == -1)
        return -1;

    return 0;
}

static void remove_rootfs(const char *rootfs)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "%s/tests/container_probe", rootfs) <
        ( int )sizeof(path)) {
        unlink(path);
    }

    if (snprintf(path, sizeof(path), "%s/tests", rootfs) <
        ( int )sizeof(path)) {
        rmdir(path);
    }

    rmdir(rootfs);
}

static int create_test_config(const char *rootfs, char *config, size_t size)
{
    int  fd;
    int  length;
    char buffer[PATH_MAX + 32];

    if (strlen(TEST_CONFIG_TEMPLATE) + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(config, TEST_CONFIG_TEMPLATE);

    fd = mkstemp(config);

    if (fd == -1)
        return -1;

    length = snprintf(buffer, sizeof(buffer), "rootfs = \"%s\"\n", rootfs);

    if (length < 0 || ( size_t )length >= sizeof(buffer)) {
        int saved_errno = errno;

        close(fd);
        unlink(config);
        errno = saved_errno ? saved_errno : ENAMETOOLONG;

        return -1;
    }

    {
        ssize_t written = 0;

        while (written < length) {
            ssize_t result;

            result = write(fd, buffer + written, ( size_t )(length - written));

            if (result == -1) {
                if (errno == EINTR)
                    continue;

                {
                    int saved_errno = errno;

                    close(fd);
                    unlink(config);
                    errno = saved_errno;
                }

                return -1;
            }

            written += result;
        }
    }

    if (close(fd) == -1) {
        int saved_errno = errno;

        unlink(config);
        errno = saved_errno;

        return -1;
    }

    return 0;
}

static int create_invalid_rootfs_config(char *config, size_t size)
{
    return create_test_config("/tmp/cage-rootfs-does-not-exist", config, size);
}

static int run_cage(const char *cage, const char *config, char *const argv[])
{
    pid_t  pid;
    size_t argc = 0;
    size_t i;
    char **args;
    int    status;

    while (argv[argc] != NULL)
        argc++;

    args = calloc(argc + 4, sizeof(*args));

    if (args == NULL)
        return -1;

    args[0] = ( char * )cage;
    args[1] = "--config";
    args[2] = ( char * )config;

    for (i = 0; i < argc; i++)
        args[i + 3] = argv[i];

    args[argc + 3] = NULL;

    pid            = fork();

    if (pid == -1) {
        free(args);
        return -1;
    }

    if (pid == 0) {
        execv(cage, args);
        _exit(127);
    }

    free(args);

    status = wait_status(pid);

    return status;
}

static int wait_for_ready(int fd)
{
    struct pollfd pfd = {
            .fd     = fd,
            .events = POLLIN,
    };
    char   buffer[sizeof("ready\n") - 1];
    size_t offset = 0;

    while (offset < sizeof(buffer)) {
        int     result;
        ssize_t count;

        result = poll(&pfd, 1, 5000);

        if (result == -1) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }

        count = read(fd, buffer + offset, sizeof(buffer) - offset);

        if (count == -1) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (count == 0) {
            errno = EPIPE;
            return -1;
        }

        offset += ( size_t )count;
    }

    if (memcmp(buffer, "ready\n", sizeof(buffer)) != 0) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static int start_cage(const char *cage, const char *config, char *const argv[],
                      pid_t *pid_out, int *ready_fd_out)
{
    int    pipefd[2];
    pid_t  pid;
    size_t argc = 0;
    size_t i;
    char **args;

    while (argv[argc] != NULL)
        argc++;

    args = calloc(argc + 4, sizeof(*args));

    if (args == NULL)
        return -1;

    if (pipe(pipefd) == -1) {
        free(args);
        return -1;
    }

    args[0] = ( char * )cage;
    args[1] = "--config";
    args[2] = ( char * )config;

    for (i = 0; i < argc; i++)
        args[i + 3] = argv[i];

    args[argc + 3] = NULL;

    pid            = fork();

    if (pid == -1) {
        int saved_errno = errno;

        close(pipefd[0]);
        close(pipefd[1]);
        free(args);
        errno = saved_errno;

        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1)
            _exit(127);

        close(pipefd[1]);

        execv(cage, args);
        _exit(127);
    }

    close(pipefd[1]);
    free(args);

    *pid_out      = pid;
    *ready_fd_out = pipefd[0];

    return 0;
}

static int wait_for_cage_ready(pid_t pid, int ready_fd)
{
    int result;

    result = wait_for_ready(ready_fd);
    close(ready_fd);

    if (result == -1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    return 0;
}

static int find_container_pid(pid_t cage_pid, pid_t *container_pid)
{
    char    path[PATH_MAX];
    char    buffer[64];
    int     fd;
    ssize_t count;
    char   *end;
    long    value;

    if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
                 ( long )cage_pid, ( long )cage_pid) >= ( int )sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(path, O_RDONLY);

    if (fd == -1)
        return -1;

    count = read(fd, buffer, sizeof(buffer) - 1);

    if (count == -1) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;

        return -1;
    }

    close(fd);

    if (count == 0) {
        errno = ESRCH;
        return -1;
    }

    buffer[count] = '\0';

    errno         = 0;
    value         = strtol(buffer, &end, 10);

    if (errno != 0 || end == buffer || value <= 0)
        return -1;

    *container_pid = ( pid_t )value;

    return 0;
}

static int namespace_differs(pid_t pid, const char *name)
{
    char        container_path[PATH_MAX];
    char        host_path[PATH_MAX];
    struct stat container_stat;
    struct stat host_stat;

    if (snprintf(host_path, sizeof(host_path), "/proc/self/ns/%s", name) >=
        ( int )sizeof(host_path))
        return -1;

    if (snprintf(container_path, sizeof(container_path), "/proc/%ld/ns/%s",
                 ( long )pid, name) >= ( int )sizeof(container_path))
        return -1;

    if (stat(host_path, &host_stat) == -1)
        return -1;

    if (stat(container_path, &container_stat) == -1)
        return -1;

    return host_stat.st_dev != container_stat.st_dev ||
           host_stat.st_ino != container_stat.st_ino;
}

static int check_id_map(pid_t pid, const char *name, unsigned long host_id)
{
    char    path[PATH_MAX];
    char    buffer[128];
    int     fd;
    ssize_t count;
    int     length;

    length =
            snprintf(path, sizeof(path), "/proc/%ld/%s_map", ( long )pid, name);

    if (length < 0 || ( size_t )length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(path, O_RDONLY);

    if (fd == -1)
        return -1;

    count = read(fd, buffer, sizeof(buffer) - 1);

    if (count == -1) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;

        return -1;
    }

    if (close(fd) == -1)
        return -1;

    buffer[count] = '\0';

    {
        unsigned long inside;
        unsigned long outside;
        unsigned long count_value;
        char          extra;

        if (sscanf(buffer, "%lu %lu %lu %c", &inside, &outside, &count_value,
                   &extra) != 3)
            return 0;

        if (inside != 0 || outside != host_id || count_value != 1)
            return 0;
    }

    return 1;
}

static int process_exists(pid_t pid)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/%ld", ( long )pid) >=
        ( int )sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (access(path, F_OK) == 0)
        return 1;

    if (errno == ENOENT)
        return 0;

    return -1;
}

static void test_exit_status(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "exit",
            "42",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 42) {
        test_pass("command exit status is propagated");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 42, got %d", status);

        test_fail("command exit status is propagated", reason);
    }
}

static void test_signal_forwarding(const char *cage, const char *config)
{
    static const int signals[] = {
            SIGTERM,
            SIGINT,
            SIGHUP,
            SIGQUIT,
    };
    static const char *const names[] = {
            "SIGTERM",
            "SIGINT",
            "SIGHUP",
            "SIGQUIT",
    };

    size_t i;

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        char *argv[] = {
                "/tests/container_probe",
                "signal",
                NULL,
        };
        pid_t pid;
        int   ready_fd;
        int   status;
        int   expected;

        if (start_cage(cage, config, argv, &pid, &ready_fd) == -1) {
            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        if (wait_for_cage_ready(pid, ready_fd) == -1) {
            test_fail("signals are forwarded to workload",
                      "workload did not become ready");
            continue;
        }

        if (kill(pid, signals[i]) == -1) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);

            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        status = wait_status(pid);

        if (status == -1) {
            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        expected = 128 + signals[i];

        if (status != expected) {
            char reason[128];

            snprintf(reason, sizeof(reason), "%s: expected %d, got %d",
                     names[i], expected, status);

            test_fail("signals are forwarded to workload", reason);
            continue;
        }

        test_pass(names[i]);
    }
}

static void test_descendant_cleanup(const char *cage, const char *config,
                                    const char *rootfs)
{
    const char *container_lock_path = "/tests/orphan.lock";
    char        host_lock_path[PATH_MAX];
    char       *orphan_argv[] = {
            "/tests/container_probe",
            "orphan",
            ( char * )container_lock_path,
            NULL,
    };
    char *check_argv[] = {
            "/tests/container_probe",
            "check-lock",
            ( char * )container_lock_path,
            NULL,
    };
    int status;

    if (snprintf(host_lock_path, sizeof(host_lock_path), "%s%s", rootfs,
                 container_lock_path) >= ( int )sizeof(host_lock_path)) {
        test_fail("descendants are terminated during teardown",
                  "lock path is too long");
        return;
    }

    unlink(host_lock_path);

    status = run_cage(cage, config, orphan_argv);

    if (status != 0) {
        char reason[64];

        snprintf(reason, sizeof(reason), "orphan test exited with %d", status);

        test_fail("descendants are terminated during teardown", reason);
        unlink(host_lock_path);
        return;
    }

    status = run_cage(cage, config, check_argv);

    unlink(host_lock_path);

    if (status == 0) {
        test_pass("descendants are terminated during teardown");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason),
                 "lock remained held, check exited with %d", status);

        test_fail("descendants are terminated during teardown", reason);
    }
}

static void test_descendant_force_cleanup(const char *cage, const char *config,
                                          const char *rootfs)
{
    const char *container_lock_path = "/tests/uncooperative.lock";
    char        host_lock_path[PATH_MAX];
    char       *orphan_argv[] = {
            "/tests/container_probe",
            "uncooperative-orphan",
            ( char * )container_lock_path,
            NULL,
    };
    char *check_argv[] = {
            "/tests/container_probe",
            "check-lock",
            ( char * )container_lock_path,
            NULL,
    };
    int status;

    if (snprintf(host_lock_path, sizeof(host_lock_path), "%s%s", rootfs,
                 container_lock_path) >= ( int )sizeof(host_lock_path)) {
        test_fail("SIGKILL cleans up uncooperative descendants",
                  "lock path is too long");
        return;
    }

    unlink(host_lock_path);

    status = run_cage(cage, config, orphan_argv);

    if (status != 0) {
        char reason[64];

        snprintf(reason, sizeof(reason), "orphan test exited with %d", status);

        test_fail("SIGKILL cleans up uncooperative descendants", reason);
        unlink(host_lock_path);
        return;
    }

    status = run_cage(cage, config, check_argv);

    unlink(host_lock_path);

    if (status == 0) {
        test_pass("SIGKILL cleans up uncooperative descendants");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason),
                 "lock remained held, check exited with %d", status);

        test_fail("SIGKILL cleans up uncooperative descendants", reason);
    }
}

static void test_pid_namespace(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "pid",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("PID namespace isolates process identity");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected PID 1, probe exited with %d",
                 status);

        test_fail("PID namespace isolates process identity", reason);
    }
}

static void test_user_namespace(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "uid",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("user namespace maps container root");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason),
                 "expected UID/GID 0, probe exited with %d", status);

        test_fail("user namespace maps container root", reason);
    }
}

static void test_user_namespace_mapping(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    int   ready_fd;
    uid_t uid;
    gid_t gid;
    int   uid_result;
    int   gid_result;
    int   status;

    if (start_cage(cage, config, argv, &cage_pid, &ready_fd) == -1) {
        test_fail("user namespace maps UID/GID explicitly", strerror(errno));
        return;
    }

    if (wait_for_cage_ready(cage_pid, ready_fd) == -1) {
        test_fail("user namespace maps UID/GID explicitly",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);

        test_fail("user namespace maps UID/GID explicitly",
                  "could not find container init");
        return;
    }

    uid        = getuid();
    gid        = getgid();

    uid_result = check_id_map(container_pid, "uid", uid);
    gid_result = check_id_map(container_pid, "gid", gid);

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);

    if (uid_result == 1 && gid_result == 1) {
        test_pass("user namespace maps UID/GID explicitly");
        return;
    }

    if (uid_result == 0 || gid_result == 0) {
        test_fail("user namespace maps UID/GID explicitly",
                  "UID/GID map does not map container 0 to host identity");
        return;
    }

    test_fail("user namespace maps UID/GID explicitly",
              "failed to inspect UID/GID maps");
}

static void test_mount_namespace(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    int   ready_fd;
    int   isolated;
    int   status;

    if (start_cage(cage, config, argv, &cage_pid, &ready_fd) == -1) {
        test_fail("mount namespace isolates mounts", strerror(errno));
        return;
    }

    if (wait_for_cage_ready(cage_pid, ready_fd) == -1) {
        test_fail("mount namespace isolates mounts",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);

        test_fail("mount namespace isolates mounts",
                  "could not find container init");
        return;
    }

    isolated = namespace_differs(container_pid, "mnt");

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);

    if (isolated == 1) {
        test_pass("mount namespace isolates mounts");
        return;
    }

    if (isolated == 0) {
        test_fail("mount namespace isolates mounts",
                  "container shares the host mount namespace");
        return;
    }

    test_fail("mount namespace isolates mounts",
              "failed to inspect mount namespace");
}

static void test_tmpfs_mount(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "tmpfs",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("/tmp is a private writable tmpfs");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 0, got %d", status);

        test_fail("/tmp is a private writable tmpfs", reason);
    }
}

static void test_proc_mount(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "proc",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("/proc is a proc filesystem");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 0, got %d", status);

        test_fail("/proc is a proc filesystem", reason);
    }
}

static void test_dev_mount(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "dev",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("/dev is a private tmpfs with device nodes");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 0, got %d", status);

        test_fail("/dev is a private tmpfs with device nodes", reason);
    }
}

static void test_network_namespace(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    int   ready_fd;
    int   isolated;
    int   status;

    if (start_cage(cage, config, argv, &cage_pid, &ready_fd) == -1) {
        test_fail("network namespace isolates networking", strerror(errno));
        return;
    }

    if (wait_for_cage_ready(cage_pid, ready_fd) == -1) {
        test_fail("network namespace isolates networking",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);

        test_fail("network namespace isolates networking",
                  "could not find container init");
        return;
    }

    isolated = namespace_differs(container_pid, "net");

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);

    if (isolated == 1) {
        test_pass("network namespace isolates networking");
        return;
    }

    if (isolated == 0) {
        test_fail("network namespace isolates networking",
                  "container shares the host network namespace");
        return;
    }

    test_fail("network namespace isolates networking",
              "failed to inspect network namespace");
}

static void test_ipc_namespace(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    int   ready_fd;
    int   isolated;
    int   status;

    if (start_cage(cage, config, argv, &cage_pid, &ready_fd) == -1) {
        test_fail("IPC namespace isolates IPC objects", strerror(errno));
        return;
    }

    if (wait_for_cage_ready(cage_pid, ready_fd) == -1) {
        test_fail("IPC namespace isolates IPC objects",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);

        test_fail("IPC namespace isolates IPC objects",
                  "could not find container init");
        return;
    }

    isolated = namespace_differs(container_pid, "ipc");

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);

    if (isolated == 1) {
        test_pass("IPC namespace isolates IPC objects");
        return;
    }

    if (isolated == 0) {
        test_fail("IPC namespace isolates IPC objects",
                  "container shares the host IPC namespace");
        return;
    }

    test_fail("IPC namespace isolates IPC objects",
              "failed to inspect IPC namespace");
}

static void test_capabilities(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "capabilities",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("capabilities are dropped");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 0, got %d", status);

        test_fail("capabilities are dropped", reason);
    }
}

static void test_no_new_privs(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "no-new-privs",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 0) {
        test_pass("no_new_privs is enabled");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 0, got %d", status);

        test_fail("no_new_privs is enabled", reason);
    }
}

static void test_fd_inheritance(const char *cage, const char *config)
{
    char  fd_string[32];
    char *argv[] = {
            "/tests/container_probe",
            "fd-inheritance",
            fd_string,
            NULL,
    };
    int fd;
    int status;

    fd = open("/dev/null", O_RDONLY);

    if (fd == -1) {
        test_fail("internal file descriptors are not inherited",
                  strerror(errno));
        return;
    }

    if (fd < 3) {
        close(fd);
        test_fail("internal file descriptors are not inherited",
                  "sentinel descriptor was below fd 3");
        return;
    }

    if (snprintf(fd_string, sizeof(fd_string), "%d", fd) >=
        ( int )sizeof(fd_string)) {
        close(fd);
        test_fail("internal file descriptors are not inherited",
                  "descriptor number is too long");
        return;
    }

    status = run_cage(cage, config, argv);

    if (close(fd) == -1) {
        test_fail("internal file descriptors are not inherited",
                  "failed to close sentinel descriptor");
        return;
    }

    if (status == 0) {
        test_pass("internal file descriptors are not inherited");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason),
                 "sentinel fd %d was inherited by workload", fd);

        test_fail("internal file descriptors are not inherited", reason);
    }
}

static void test_exec_failure(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/does-not-exist",
            NULL,
    };
    int status;

    status = run_cage(cage, config, argv);

    if (status == 127) {
        test_pass("exec failure is propagated");
        return;
    }

    {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 127, got %d", status);

        test_fail("exec failure is propagated", reason);
    }
}

static void test_invalid_rootfs(const char *cage)
{
    char  config[PATH_MAX];
    char *argv[] = {
            "/tests/container_probe",
            "exit",
            "0",
            NULL,
    };
    int status;

    if (create_invalid_rootfs_config(config, sizeof(config)) == -1) {
        test_fail("invalid rootfs is rejected",
                  "failed to create invalid test config");
        return;
    }

    status = run_cage(cage, config, argv);

    unlink(config);

    if (status != 0) {
        test_pass("invalid rootfs is rejected");
        return;
    }

    test_fail("invalid rootfs is rejected", "cage unexpectedly succeeded");
}

static void test_parent_death(const char *cage, const char *config)
{
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    int   ready_fd;
    int   status;
    int   result;

    if (start_cage(cage, config, argv, &cage_pid, &ready_fd) == -1) {
        test_fail("container dies when cage supervisor dies", strerror(errno));
        return;
    }

    if (wait_for_cage_ready(cage_pid, ready_fd) == -1) {
        test_fail("container dies when cage supervisor dies",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);

        test_fail("container dies when cage supervisor dies",
                  "could not find container init");
        return;
    }

    /*
     * Kill the cage supervisor without allowing normal container
     * teardown to run. The container should receive SIGKILL through
     * PR_SET_PDEATHSIG.
     */
    if (kill(cage_pid, SIGKILL) == -1) {
        test_fail("container dies when cage supervisor dies", strerror(errno));
        return;
    }

    if (waitpid(cage_pid, &status, 0) == -1) {
        test_fail("container dies when cage supervisor dies", strerror(errno));
        return;
    }

    /*
     * Wait briefly for the parent-death signal to terminate the
     * container process and for /proc to reflect its disappearance.
     */
    result = 0;

    for (int attempt = 0; attempt < 20; attempt++) {
        result = process_exists(container_pid);

        if (result == 0)
            break;

        if (result == -1)
            break;

        usleep(10000);
    }

    if (result == 0) {
        test_pass("container dies when cage supervisor dies");
        return;
    }

    if (result == -1) {
        test_fail("container dies when cage supervisor dies",
                  "failed to inspect container process");
        return;
    }

    /*
     * Clean up if parent-death handling failed, so a failed test
     * does not leave a container behind.
     */
    kill(container_pid, SIGKILL);

    {
        char reason[64];

        snprintf(reason, sizeof(reason),
                 "container PID %ld survived supervisor death",
                 ( long )container_pid);

        test_fail("container dies when cage supervisor dies", reason);
    }
}

int main(int argc, char **argv)
{
    char rootfs[PATH_MAX];
    char config[PATH_MAX];

    if (argc != 3) {
        fprintf(stderr, "usage: %s <cage> <container_probe>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (create_rootfs(argv[2], rootfs, sizeof(rootfs)) == -1) {
        perror("create test rootfs");
        return EXIT_FAILURE;
    }

    if (create_test_config(rootfs, config, sizeof(config)) == -1) {
        perror("create test config");
        remove_rootfs(rootfs);
        return EXIT_FAILURE;
    }

    test_exit_status(argv[1], config);
    test_signal_forwarding(argv[1], config);
    test_descendant_cleanup(argv[1], config, rootfs);
    test_descendant_force_cleanup(argv[1], config, rootfs);
    test_pid_namespace(argv[1], config);
    test_user_namespace(argv[1], config);
    test_user_namespace_mapping(argv[1], config);
    test_mount_namespace(argv[1], config);
    test_tmpfs_mount(argv[1], config);
    test_proc_mount(argv[1], config);
    test_dev_mount(argv[1], config);
    test_network_namespace(argv[1], config);
    test_ipc_namespace(argv[1], config);
    test_capabilities(argv[1], config);
    test_no_new_privs(argv[1], config);
    test_fd_inheritance(argv[1], config);
    test_exec_failure(argv[1], config);
    test_invalid_rootfs(argv[1]);
    test_parent_death(argv[1], config);

    unlink(config);
    remove_rootfs(rootfs);

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
