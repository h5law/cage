#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_ROOTFS_TEMPLATE "/tmp/cage-test-rootfs-XXXXXX"

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

    fprintf(stderr, "FAIL: %s: %s\n", name, reason);
}

static int run_cage(const char *cage, const char *rootfs, char *const argv[])
{
    pid_t pid;
    int   status;

    pid = fork();

    if (pid == -1)
        return -1;

    if (pid == 0) {
        char  *args[32];
        size_t i;

        args[0] = ( char * )cage;
        args[1] = ( char * )rootfs;

        for (i = 0; argv[i] != NULL; i++)
            args[i + 2] = argv[i];

        args[i + 2] = NULL;

        execv(cage, args);
        _exit(127);
    }

    for (;;) {
        if (waitpid(pid, &status, 0) != -1)
            break;

        if (errno != EINTR)
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

    if (snprintf(path, sizeof(path), "%s/tests/signal.ready", rootfs) <
        ( int )sizeof(path)) {
        unlink(path);
    }

    if (snprintf(path, sizeof(path), "%s/tests/orphan.lock", rootfs) <
        ( int )sizeof(path)) {
        unlink(path);
    }

    if (snprintf(path, sizeof(path), "%s/tests", rootfs) <
        ( int )sizeof(path)) {
        rmdir(path);
    }

    rmdir(rootfs);
}

static int wait_for_file(const char *path)
{
    struct timespec interval = {
            .tv_sec  = 0,
            .tv_nsec = 10000000L,
    };
    struct stat st;
    int         attempts = 500;

    while (attempts-- > 0) {
        if (stat(path, &st) == 0)
            return 0;

        if (errno != ENOENT)
            return -1;

        while (nanosleep(&interval, NULL) == -1) {
            if (errno != EINTR)
                return -1;
        }
    }

    errno = ETIMEDOUT;
    return -1;
}

static int find_container_pid(pid_t cage_pid, pid_t *container_pid)
{
    char    path[PATH_MAX];
    char    buffer[128];
    char   *end;
    long    value;
    int     fd;
    ssize_t count;

    if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
                 ( long )cage_pid, ( long )cage_pid) >= ( int )sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(path, O_RDONLY);

    if (fd == -1)
        return -1;

    count = read(fd, buffer, sizeof(buffer) - 1);

    close(fd);

    if (count <= 0) {
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

static void test_network_namespace(const char *cage, const char *rootfs)
{
    char  ready_path[PATH_MAX];
    char  container_ready_path[PATH_MAX];
    char *argv[4];
    pid_t cage_pid;
    pid_t container_pid;
    int   isolated;
    int   status;

    if (snprintf(container_ready_path, sizeof(container_ready_path),
                 "/tests/network.ready") >=
        ( int )sizeof(container_ready_path)) {
        test_fail("network namespace isolates networking",
                  "ready path is too long");
        return;
    }

    if (snprintf(ready_path, sizeof(ready_path), "%s%s", rootfs,
                 container_ready_path) >= ( int )sizeof(ready_path)) {
        test_fail("network namespace isolates networking",
                  "host ready path is too long");
        return;
    }

    unlink(ready_path);

    argv[0]  = "/tests/container_probe";
    argv[1]  = "signal";
    argv[2]  = container_ready_path;
    argv[3]  = NULL;

    cage_pid = fork();

    if (cage_pid == -1) {
        test_fail("network namespace isolates networking", strerror(errno));
        return;
    }

    if (cage_pid == 0) {
        char *args[6];

        args[0] = ( char * )cage;
        args[1] = ( char * )rootfs;
        args[2] = argv[0];
        args[3] = argv[1];
        args[4] = argv[2];
        args[5] = NULL;

        execv(cage, args);
        _exit(127);
    }

    if (wait_for_file(ready_path) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("network namespace isolates networking",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("network namespace isolates networking",
                  "could not find container init");
        return;
    }

    isolated = namespace_differs(container_pid, "net");

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);
    unlink(ready_path);

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

static void test_exit_status(const char *cage, const char *rootfs)
{
    char *argv[] = {
            "/tests/container_probe",
            "exit",
            "42",
            NULL,
    };
    int status;

    status = run_cage(cage, rootfs, argv);

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

static void test_signal_forwarding(const char *cage, const char *rootfs)
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
        char  host_ready_path[PATH_MAX];
        char *argv[] = {
                "/tests/container_probe",
                "signal",
                "/tests/signal.ready",
                NULL,
        };
        pid_t pid;
        int   status;
        int   expected;

        if (snprintf(host_ready_path, sizeof(host_ready_path),
                     "%s/tests/signal.ready",
                     rootfs) >= ( int )sizeof(host_ready_path)) {
            test_fail("signals are forwarded to workload",
                      "ready path is too long");
            continue;
        }

        unlink(host_ready_path);

        pid = fork();

        if (pid == -1) {
            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        if (pid == 0) {
            char *args[6];

            args[0] = ( char * )cage;
            args[1] = ( char * )rootfs;
            args[2] = argv[0];
            args[3] = argv[1];
            args[4] = argv[2];
            args[5] = NULL;

            execv(cage, args);
            _exit(127);
        }

        if (wait_for_file(host_ready_path) == -1) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            unlink(host_ready_path);

            test_fail("signals are forwarded to workload",
                      "workload did not become ready");
            continue;
        }

        if (kill(pid, signals[i]) == -1) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            unlink(host_ready_path);

            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        for (;;) {
            if (waitpid(pid, &status, 0) != -1)
                break;

            if (errno == EINTR)
                continue;

            unlink(host_ready_path);

            test_fail("signals are forwarded to workload", strerror(errno));
            continue;
        }

        if (!WIFEXITED(status)) {
            test_fail("signals are forwarded to workload",
                      "cage did not exit normally");
            continue;
        }

        expected = 128 + signals[i];

        if (WEXITSTATUS(status) != expected) {
            char reason[128];

            snprintf(reason, sizeof(reason), "%s: expected %d, got %d",
                     names[i], expected, WEXITSTATUS(status));

            test_fail("signals are forwarded to workload", reason);
            continue;
        }

        test_pass(names[i]);
    }
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

    /*
     * /proc may use variable whitespace, so parse the three
     * numeric fields rather than comparing the textual formatting.
     */
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

static void test_descendant_cleanup(const char *cage, const char *rootfs)
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

    status = run_cage(cage, rootfs, orphan_argv);

    if (status != 0) {
        char reason[64];

        snprintf(reason, sizeof(reason), "orphan test exited with %d", status);

        test_fail("descendants are terminated during teardown", reason);
        unlink(host_lock_path);
        return;
    }

    status = run_cage(cage, rootfs, check_argv);

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

static void test_pid_namespace(const char *cage, const char *rootfs)
{
    char *argv[] = {
            "/tests/container_probe",
            "pid",
            NULL,
    };
    int status;

    status = run_cage(cage, rootfs, argv);

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

static void test_user_namespace(const char *cage, const char *rootfs)
{
    char *argv[] = {
            "/tests/container_probe",
            "uid",
            NULL,
    };
    int status;

    status = run_cage(cage, rootfs, argv);

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

static void test_user_namespace_mapping(const char *cage, const char *rootfs)
{
    char  ready_path[PATH_MAX];
    char *argv[] = {
            "/tests/container_probe",
            "signal",
            "/tests/signal.ready",
            NULL,
    };
    pid_t cage_pid;
    pid_t container_pid;
    uid_t uid;
    gid_t gid;
    int   uid_result;
    int   gid_result;
    int   status;

    if (snprintf(ready_path, sizeof(ready_path), "%s/tests/signal.ready",
                 rootfs) >= ( int )sizeof(ready_path)) {
        test_fail("user namespace maps UID/GID explicitly",
                  "ready path is too long");
        return;
    }

    unlink(ready_path);

    cage_pid = fork();

    if (cage_pid == -1) {
        test_fail("user namespace maps UID/GID explicitly", strerror(errno));
        return;
    }

    if (cage_pid == 0) {
        char *args[6];

        args[0] = ( char * )cage;
        args[1] = ( char * )rootfs;
        args[2] = argv[0];
        args[3] = argv[1];
        args[4] = argv[2];
        args[5] = NULL;

        execv(cage, args);
        _exit(127);
    }

    if (wait_for_file(ready_path) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("user namespace maps UID/GID explicitly",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("user namespace maps UID/GID explicitly",
                  "could not find container init");
        return;
    }

    uid        = getuid();
    gid        = getgid();

    uid_result = check_id_map(container_pid, "uid", ( unsigned long )uid);
    gid_result = check_id_map(container_pid, "gid", ( unsigned long )gid);

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);
    unlink(ready_path);

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

static void test_mount_namespace(const char *cage, const char *rootfs)
{
    char  mount_path[PATH_MAX];
    char *argv[] = {
            "/tests/container_probe",
            "mount",
            NULL,
    };
    struct stat st;
    int         status;

    if (snprintf(mount_path, sizeof(mount_path), "%s/mnt", rootfs) >=
        ( int )sizeof(mount_path)) {
        test_fail("mount namespace isolates mounts", "mount path is too long");
        return;
    }

    if (mkdir(mount_path, 0755) == -1) {
        test_fail("mount namespace isolates mounts", strerror(errno));
        return;
    }

    status = run_cage(cage, rootfs, argv);

    if (status != 0) {
        rmdir(mount_path);

        {
            char reason[64];

            snprintf(reason, sizeof(reason), "mount probe exited with %d",
                     status);

            test_fail("mount namespace isolates mounts", reason);
        }

        return;
    }

    if (stat(mount_path, &st) == -1) {
        test_fail("mount namespace isolates mounts", strerror(errno));
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        test_fail("mount namespace isolates mounts",
                  "host mountpoint is no longer a directory");
        return;
    }

    if (rmdir(mount_path) == -1) {
        test_fail("mount namespace isolates mounts",
                  "failed to remove host mountpoint");
        return;
    }

    test_pass("mount namespace isolates mounts");
}

static void test_ipc_namespace(const char *cage, const char *rootfs)
{
    char  ready_path[PATH_MAX];
    char  container_ready_path[PATH_MAX];
    char *argv[4];
    pid_t cage_pid;
    pid_t container_pid;
    int   isolated;
    int   status;

    if (snprintf(container_ready_path, sizeof(container_ready_path),
                 "/tests/ipc.ready") >= ( int )sizeof(container_ready_path)) {
        test_fail("IPC namespace isolates IPC objects",
                  "ready path is too long");
        return;
    }

    if (snprintf(ready_path, sizeof(ready_path), "%s%s", rootfs,
                 container_ready_path) >= ( int )sizeof(ready_path)) {
        test_fail("IPC namespace isolates IPC objects",
                  "host ready path is too long");
        return;
    }

    unlink(ready_path);

    argv[0]  = "/tests/container_probe";
    argv[1]  = "signal";
    argv[2]  = container_ready_path;
    argv[3]  = NULL;

    cage_pid = fork();

    if (cage_pid == -1) {
        test_fail("IPC namespace isolates IPC objects", strerror(errno));
        return;
    }

    if (cage_pid == 0) {
        char *args[6];

        args[0] = ( char * )cage;
        args[1] = ( char * )rootfs;
        args[2] = argv[0];
        args[3] = argv[1];
        args[4] = argv[2];
        args[5] = NULL;

        execv(cage, args);
        _exit(127);
    }

    if (wait_for_file(ready_path) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("IPC namespace isolates IPC objects",
                  "workload did not become ready");
        return;
    }

    if (find_container_pid(cage_pid, &container_pid) == -1) {
        kill(cage_pid, SIGKILL);
        waitpid(cage_pid, NULL, 0);
        unlink(ready_path);

        test_fail("IPC namespace isolates IPC objects",
                  "could not find container init");
        return;
    }

    isolated = namespace_differs(container_pid, "ipc");

    kill(cage_pid, SIGTERM);
    waitpid(cage_pid, &status, 0);
    unlink(ready_path);

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

static void test_descendant_force_cleanup(const char *cage, const char *rootfs)
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

    status = run_cage(cage, rootfs, orphan_argv);

    if (status != 0) {
        char reason[64];

        snprintf(reason, sizeof(reason), "orphan test exited with %d", status);

        test_fail("SIGKILL cleans up uncooperative descendants", reason);
        unlink(host_lock_path);
        return;
    }

    status = run_cage(cage, rootfs, check_argv);

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

static void test_invalid_rootfs(const char *cage)
{
    char *argv[] = {
            "/tests/container_probe",
            "exit",
            "0",
            NULL,
    };
    int status;

    status = run_cage(cage, "/tmp/cage-rootfs-does-not-exist", argv);

    if (status != 0) {
        test_pass("invalid rootfs is rejected");
        return;
    }

    test_fail("invalid rootfs is rejected", "cage unexpectedly succeeded");
}

static void test_exec_failure(const char *cage, const char *rootfs)
{
    char *argv[] = {
            "/tests/does-not-exist",
            NULL,
    };
    int status;

    status = run_cage(cage, rootfs, argv);

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

int main(int argc, char **argv)
{
    char rootfs[PATH_MAX];

    if (argc != 3) {
        fprintf(stderr, "usage: %s <cage> <container_probe>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (create_rootfs(argv[2], rootfs, sizeof(rootfs)) == -1) {
        perror("create test rootfs");
        return EXIT_FAILURE;
    }

    test_exit_status(argv[1], rootfs);
    test_signal_forwarding(argv[1], rootfs);
    test_descendant_cleanup(argv[1], rootfs);
    test_descendant_force_cleanup(argv[1], rootfs);
    test_pid_namespace(argv[1], rootfs);
    test_user_namespace(argv[1], rootfs);
    test_user_namespace_mapping(argv[1], rootfs);
    test_mount_namespace(argv[1], rootfs);
    test_network_namespace(argv[1], rootfs);
    test_ipc_namespace(argv[1], rootfs);
    test_exec_failure(argv[1], rootfs);
    test_invalid_rootfs(argv[1]);

    remove_rootfs(rootfs);

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
