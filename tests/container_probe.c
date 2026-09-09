#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static int parse_status(const char *value)
{
    char *end;
    long  status;

    errno  = 0;
    status = strtol(value, &end, 10);

    if (errno != 0 || *end != '\0' || status < 0 || status > 255)
        return -1;

    return ( int )status;
}

static int run_orphan(const char *path)
{
    pid_t pid;
    int   fd;

    fd = open(path, O_CREAT | O_RDWR, 0600);

    if (fd == -1)
        return 1;

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        close(fd);
        return 1;
    }

    pid = fork();

    if (pid == -1) {
        close(fd);
        return 1;
    }

    if (pid == 0) {
        for (;;)
            pause();
    }

    close(fd);

    return 0;
}

static int check_lock(const char *path)
{
    int fd;

    fd = open(path, O_CREAT | O_RDWR, 0600);

    if (fd == -1)
        return 1;

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        close(fd);
        return 1;
    }

    close(fd);

    return 0;
}

static int run_signal(void)
{
    if (write(STDOUT_FILENO, "ready\n", 6) != 6)
        return 1;

    for (;;)
        pause();
}

static int check_pid_namespace(void) { return getppid() == 1 ? 0 : 1; }

static int check_user_namespace(void)
{
    return geteuid() == 0 && getegid() == 0 ? 0 : 1;
}

static int check_mount_namespace(pid_t host_pid)
{
    struct stat self_ns;
    struct stat host_ns;
    char        path[64];

    if (stat("/proc/self/ns/mnt", &self_ns) == -1)
        return 1;

    if (snprintf(path, sizeof(path), "/proc/%ld/ns/mnt", ( long )host_pid) >=
        ( int )sizeof(path))
        return 1;

    if (stat(path, &host_ns) == -1)
        return 1;

    return self_ns.st_dev != host_ns.st_dev || self_ns.st_ino != host_ns.st_ino
                   ? 0
                   : 1;
}

static int check_tmpfs(void)
{
    struct statfs fs;
    int           fd;

    if (statfs("/tmp", &fs) == -1)
        return 1;

    if (fs.f_type != TMPFS_MAGIC)
        return 1;

    fd = open("/tmp/cage-tmpfs-test", O_CREAT | O_WRONLY, 0600);

    if (fd == -1)
        return 1;

    close(fd);

    if (unlink("/tmp/cage-tmpfs-test") == -1)
        return 1;

    return 0;
}

static int check_proc(void)
{
    struct statfs fs;

    if (statfs("/proc", &fs) == -1)
        return 1;

    if (fs.f_type != PROC_SUPER_MAGIC)
        return 1;

    return 0;
}

static int check_dev(void)
{
    static const struct {
        const char  *path;
        unsigned int major;
        unsigned int minor;
    } devices[] = {
            {"/dev/null",    1, 3},
            {"/dev/zero",    1, 5},
            {"/dev/random",  1, 8},
            {"/dev/urandom", 1, 9},
            {"/dev/tty",     5, 0},
    };

    struct statfs fs;
    struct stat   st;
    int           fd;

    if (statfs("/dev", &fs) == -1)
        return 1;

    if (fs.f_type != TMPFS_MAGIC)
        return 1;

    for (size_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (stat(devices[i].path, &st) == -1)
            return 1;

        if (!S_ISCHR(st.st_mode))
            return 1;

        if (major(st.st_rdev) != devices[i].major ||
            minor(st.st_rdev) != devices[i].minor)
            return 1;
    }

    fd = open("/dev/null", O_WRONLY);

    if (fd == -1)
        return 1;

    if (write(fd, "", 0) == -1) {
        close(fd);
        return 1;
    }

    if (close(fd) == -1)
        return 1;

    return 0;
}

static int check_capabilities(void)
{
    FILE *file;
    char  line[256];

    file = fopen("/proc/self/status", "r");

    if (file == NULL)
        return 1;

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long effective;

        if (sscanf(line, "CapEff:\t%llx", &effective) == 1) {
            fclose(file);
            return effective == 0 ? 0 : 1;
        }
    }

    fclose(file);
    return 1;
}

static int check_no_new_privs(void)
{
    FILE         *file;
    char          line[128];
    unsigned long value;

    file = fopen("/proc/self/status", "r");

    if (file == NULL)
        return 1;

    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "NoNewPrivs: %lu", &value) == 1) {
            fclose(file);
            return value == 1 ? 0 : 1;
        }
    }

    fclose(file);
    return 1;
}

static int run_uncooperative_orphan(const char *path)
{
    struct sigaction action;
    pid_t            pid;
    int              fd;

    fd = open(path, O_CREAT | O_RDWR, 0600);

    if (fd == -1)
        return 1;

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        close(fd);
        return 1;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGTERM, &action, NULL) == -1) {
        close(fd);
        return 1;
    }

    pid = fork();

    if (pid == -1) {
        close(fd);
        return 1;
    }

    if (pid == 0) {
        for (;;)
            pause();
    }

    close(fd);

    return 0;
}

int main(int argc, char **argv)
{
    int status;

    if (argc < 2)
        return 2;

    if (strcmp(argv[1], "exit") == 0) {
        if (argc != 3)
            return 2;

        status = parse_status(argv[2]);

        if (status < 0)
            return 2;

        return status;
    }

    if (strcmp(argv[1], "sleep") == 0) {
        if (argc != 2)
            return 2;

        for (;;)
            pause();
    }

    if (strcmp(argv[1], "signal") == 0) {
        if (argc != 2)
            return 2;

        return run_signal();
    }

    if (strcmp(argv[1], "orphan") == 0) {
        if (argc != 3)
            return 2;

        return run_orphan(argv[2]);
    }

    if (strcmp(argv[1], "check-lock") == 0) {
        if (argc != 3)
            return 2;

        return check_lock(argv[2]);
    }

    if (strcmp(argv[1], "pid") == 0) {
        if (argc != 2)
            return 2;

        return check_pid_namespace();
    }

    if (strcmp(argv[1], "uid") == 0) {
        if (argc != 2)
            return 2;

        return check_user_namespace();
    }

    if (strcmp(argv[1], "mount") == 0) {
        char *end;
        long  pid;

        if (argc != 3)
            return 2;

        errno = 0;
        pid   = strtol(argv[2], &end, 10);

        if (errno != 0 || *end != '\0' || pid <= 0)
            return 2;

        return check_mount_namespace(( pid_t )pid);
    }

    if (strcmp(argv[1], "tmpfs") == 0) {
        if (argc != 2)
            return 2;

        return check_tmpfs();
    }

    if (strcmp(argv[1], "proc") == 0) {
        if (argc != 2)
            return 2;

        return check_proc();
    }

    if (strcmp(argv[1], "dev") == 0) {
        if (argc != 2)
            return 2;

        return check_dev();
    }

    if (strcmp(argv[1], "capabilities") == 0) {
        if (argc != 2)
            return 2;

        return check_capabilities();
    }

    if (strcmp(argv[1], "no-new-privs") == 0) {
        if (argc != 2)
            return 2;

        return check_no_new_privs();
    }

    if (strcmp(argv[1], "uncooperative-orphan") == 0) {
        if (argc != 3)
            return 2;

        return run_uncooperative_orphan(argv[2]);
    }

    return 2;
}
