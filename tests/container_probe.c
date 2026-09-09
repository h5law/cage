#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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

static int run_signal(const char *ready_path)
{
    int fd;

    fd = open(ready_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd == -1)
        return 1;

    if (write(fd, "ready\n", 6) != 6) {
        close(fd);
        return 1;
    }

    close(fd);

    for (;;)
        pause();
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
        if (argc != 3)
            return 2;

        return run_signal(argv[2]);
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

    return 2;
}
