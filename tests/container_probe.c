#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
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

static int check_pid_namespace(void)
{
    return getppid() == 1 ? 0 : 1;
}

static int check_user_namespace(void)
{
    return geteuid() == 0 && getegid() == 0 ? 0 : 1;
}

static int check_mount_namespace(void)
{
    if (mkdir("/mnt", 0755) == -1 && errno != EEXIST)
        return 1;

    if (mount("tmpfs", "/mnt", "tmpfs", 0, NULL) == -1)
        return 1;

    return 0;
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
        if (argc != 2)
            return 2;

        return check_mount_namespace();
    }

    if (strcmp(argv[1], "uncooperative-orphan") == 0) {
        if (argc != 3)
            return 2;

        return run_uncooperative_orphan(argv[2]);
    }

    return 2;
}