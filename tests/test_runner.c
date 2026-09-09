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
    const char *container_ready_path = "/tests/signal.ready";
    char        host_ready_path[PATH_MAX];
    char       *argv[] = {
            "/tests/container_probe",
            "signal",
            ( char * )container_ready_path,
            NULL,
    };
    pid_t pid;
    int   status;

    if (snprintf(host_ready_path, sizeof(host_ready_path), "%s%s", rootfs,
                 container_ready_path) >= ( int )sizeof(host_ready_path)) {
        test_fail("signals are forwarded to workload",
                  "ready path is too long");
        return;
    }

    unlink(host_ready_path);

    pid = fork();

    if (pid == -1) {
        test_fail("signals are forwarded to workload", strerror(errno));
        return;
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
        return;
    }

    if (kill(pid, SIGTERM) == -1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        unlink(host_ready_path);

        test_fail("signals are forwarded to workload", strerror(errno));
        return;
    }

    for (;;) {
        if (waitpid(pid, &status, 0) != -1)
            break;

        if (errno != EINTR) {
            test_fail("signals are forwarded to workload", strerror(errno));
            return;
        }
    }

    unlink(host_ready_path);

    if (!WIFEXITED(status)) {
        test_fail("signals are forwarded to workload",
                  "cage did not exit normally");
        return;
    }

    if (WEXITSTATUS(status) != 143) {
        char reason[64];

        snprintf(reason, sizeof(reason), "expected 143, got %d",
                 WEXITSTATUS(status));

        test_fail("signals are forwarded to workload", reason);
        return;
    }

    test_pass("signals are forwarded to workload");
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

    remove_rootfs(rootfs);

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
