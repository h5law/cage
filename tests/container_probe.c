#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        buf += n;
        len -= ( size_t )n;
    }

    return 0;
}

static int probe_write_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd == -1)
        return 1;

    if (write_all(fd, contents, strlen(contents)) == -1) {
        close(fd);
        return 1;
    }

    if (close(fd) == -1)
        return 1;

    return 0;
}

static int probe_read_file(const char *path, const char *expected)
{
    char   buf[4096];
    size_t expected_len = strlen(expected);
    int    fd           = open(path, O_RDONLY);

    if (fd == -1)
        return 1;

    ssize_t n = read(fd, buf, sizeof(buf));

    close(fd);

    if (n < 0)
        return 1;

    if (( size_t )n != expected_len)
        return 1;

    return memcmp(buf, expected, expected_len) == 0 ? 0 : 1;
}

static int probe_exit(const char *arg)
{
    char *end;
    long  status;

    errno  = 0;
    status = strtol(arg, &end, 10);

    if (errno != 0 || *end != '\0' || status < 0 || status > 255)
        return 1;

    return ( int )status;
}

static int probe_signal(const char *arg)
{
    char *end;
    long  signum;

    errno  = 0;
    signum = strtol(arg, &end, 10);

    if (errno != 0 || *end != '\0' || signum <= 0 || signum >= NSIG)
        return 1;

    raise(( int )signum);

    return 1;
}

static int probe_namespace(const char *name, unsigned long expected_dev,
                           unsigned long expected_ino)
{
    char        path[PATH_MAX];
    struct stat current;

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", name) >=
        ( int )sizeof(path))
        return 1;

    if (stat(path, &current) == -1)
        return 1;

    if (( unsigned long )current.st_dev == expected_dev &&
        ( unsigned long )current.st_ino == expected_ino)
        return 1;

    return 0;
}

static volatile sig_atomic_t received_signal;

static void handle_test_signal(int signal_number)
{
    received_signal = signal_number;
}

static int probe_wait_signal(const char *arg)
{
    char            *end;
    long             signum;
    struct sigaction action;

    errno  = 0;
    signum = strtol(arg, &end, 10);

    if (errno != 0 || *end != '\0' || signum <= 0 || signum >= NSIG)
        return 1;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_test_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(( int )signum, &action, NULL) == -1)
        return 1;

    for (;;) {
        if (received_signal != 0)
            return 128 + received_signal;

        pause();
    }
}

static int probe_uid_gid_map(unsigned long expected_uid,
                             unsigned long expected_gid)
{
    char          buffer[128];
    int           fd;
    unsigned long inside;
    unsigned long outside;
    unsigned long count;

    fd = open("/proc/self/uid_map", O_RDONLY);

    if (fd == -1)
        return 1;

    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

    close(fd);

    if (n <= 0)
        return 1;

    buffer[n] = '\0';

    if (sscanf(buffer, "%lu %lu %lu", &inside, &outside, &count) != 3)
        return 1;

    if (inside != 0 || outside != expected_uid || count != 1)
        return 1;

    fd = open("/proc/self/gid_map", O_RDONLY);

    if (fd == -1)
        return 1;

    n = read(fd, buffer, sizeof(buffer) - 1);

    close(fd);

    if (n <= 0)
        return 1;

    buffer[n] = '\0';

    if (sscanf(buffer, "%lu %lu %lu", &inside, &outside, &count) != 3)
        return 1;

    if (inside != 0 || outside != expected_gid || count != 1)
        return 1;

    return 0;
}

static int probe_orphan(int uncooperative)
{
    pid_t pid = fork();

    if (pid == -1)
        return 1;

    if (pid == 0) {
        if (uncooperative)
            signal(SIGTERM, SIG_IGN);

        for (;;)
            pause();
    }

    return 0;
}

static int probe_check_lock(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);

    if (fd == -1)
        return 1;

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int probe_pid(void)
{
    printf("%ld\n", ( long )getpid());
    return 0;
}

static int probe_uid(void)
{
    printf("%ld %ld\n", ( long )getuid(), ( long )getgid());
    return 0;
}

static int probe_tmpfs(const char *path)
{
    struct statfs statfs_buf;

    if (statfs(path, &statfs_buf) == -1)
        return 1;

    return statfs_buf.f_type == TMPFS_MAGIC ? 0 : 1;
}

static int probe_proc(const char *path)
{
    struct statfs statfs_buf;

    if (statfs(path, &statfs_buf) == -1)
        return 1;

    return statfs_buf.f_type == PROC_SUPER_MAGIC ? 0 : 1;
}

static int probe_dev(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
        return 1;

    return S_ISCHR(st.st_mode) ? 0 : 1;
}

static int probe_capabilities(void)
{
    FILE *fp;
    char  buf[4096];

    fp = fopen("/proc/self/status", "r");
    if (fp == NULL)
        return 1;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strncmp(buf, "CapEff:", 7) == 0) {
            unsigned long long caps;

            if (sscanf(buf + 7, "%llx", &caps) != 1) {
                fclose(fp);
                return 1;
            }

            fclose(fp);
            return caps == 0 ? 0 : 1;
        }
    }

    fclose(fp);
    return 1;
}

static int probe_no_new_privs(void)
{
    int value = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);

    if (value == -1)
        return 1;

    return value == 1 ? 0 : 1;
}

static int probe_fd_inheritance(void)
{
    long max_fd;
    int  found = 0;

    max_fd     = sysconf(_SC_OPEN_MAX);

    if (max_fd < 0)
        max_fd = 1024;

    for (int fd = 3; fd < max_fd; fd++) {
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            found = 1;
            break;
        }
    }

    return found ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;

    if (strcmp(argv[1], "exit") == 0) {
        if (argc != 3)
            return 1;
        return probe_exit(argv[2]);
    }

    if (strcmp(argv[1], "namespace") == 0) {
        unsigned long dev;
        unsigned long ino;

        if (argc != 5)
            return 2;

        dev = strtoul(argv[3], NULL, 10);
        ino = strtoul(argv[4], NULL, 10);

        return probe_namespace(argv[2], dev, ino) ? 1 : 0;
    }

    if (strcmp(argv[1], "signal") == 0) {
        if (argc != 3)
            return 1;
        return probe_signal(argv[2]);
    }

    if (strcmp(argv[1], "wait-signal") == 0) {
        if (argc != 3)
            return 2;

        return probe_wait_signal(argv[2]);
    }

    if (strcmp(argv[1], "orphan") == 0)
        return probe_orphan(0);

    if (strcmp(argv[1], "uncooperative-orphan") == 0)
        return probe_orphan(1);

    if (strcmp(argv[1], "check-lock") == 0) {
        if (argc != 3)
            return 1;
        return probe_check_lock(argv[2]);
    }

    if (strcmp(argv[1], "pid") == 0)
        return probe_pid();

    if (strcmp(argv[1], "uid") == 0)
        return probe_uid();

    if (strcmp(argv[1], "uid-map") == 0) {
        if (argc != 4)
            return 2;

        return probe_uid_gid_map(strtoul(argv[2], NULL, 10),
                                 strtoul(argv[3], NULL, 10));
    }

    if (strcmp(argv[1], "tmpfs") == 0) {
        if (argc != 3)
            return 1;
        return probe_tmpfs(argv[2]);
    }

    if (strcmp(argv[1], "proc") == 0) {
        if (argc != 3)
            return 1;
        return probe_proc(argv[2]);
    }

    if (strcmp(argv[1], "dev") == 0) {
        if (argc != 3)
            return 1;
        return probe_dev(argv[2]);
    }

    if (strcmp(argv[1], "capabilities") == 0)
        return probe_capabilities();

    if (strcmp(argv[1], "no-new-privs") == 0)
        return probe_no_new_privs();

    if (strcmp(argv[1], "fd-inheritance") == 0)
        return probe_fd_inheritance();

    if (strcmp(argv[1], "write-file") == 0) {
        if (argc != 4)
            return 1;
        return probe_write_file(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "read-file") == 0) {
        if (argc != 4)
            return 1;
        return probe_read_file(argv[2], argv[3]);
    }

    return 1;
}
