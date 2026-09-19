#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static int read_file(const char *path)
{
    int     fd;
    char    buf[4096];
    ssize_t n;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return -1;

    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n == -1 && errno == EINTR);

    {
        int saved_errno = errno;

        close(fd);

        if (n == -1) {
            errno = saved_errno;
            return -1;
        }
    }

    buf[n] = '\0';
    fputs(buf, stdout);

    return 0;
}

static int write_file(const char *path, const char *contents)
{
    int    fd;
    size_t length;
    size_t written;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd == -1)
        return -1;

    length  = strlen(contents);
    written = 0;

    while (written < length) {
        ssize_t n;

        do {
            n = write(fd, contents + written, length - written);
        } while (n == -1 && errno == EINTR);

        if (n == -1) {
            int saved_errno = errno;

            close(fd);
            errno = saved_errno;

            return -1;
        }

        if (n == 0) {
            close(fd);
            errno = EIO;

            return -1;
        }

        written += ( size_t )n;
    }

    return close(fd);
}

static int probe_proc_root(const char *host_path, const char *expected)
{
    char    buffer[4096];
    char    path[4096];
    int     fd;
    ssize_t n;

    if (host_path[0] != '/') {
        errno = EINVAL;
        return 1;
    }

    if (snprintf(path, sizeof(path), "/proc/1/root%s", host_path) >=
        ( int )sizeof(path))
        return 1;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return 0;

    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n == -1 && errno == EINTR);

    {
        int saved_errno = errno;

        close(fd);

        if (n == -1) {
            errno = saved_errno;
            return 0;
        }
    }

    buffer[n] = '\0';

    return strcmp(buffer, expected) == 0 ? 1 : 0;
}

static int probe_proc_root_openat(const char *host_path, const char *expected)
{
    char        buffer[4096];
    int         root_fd;
    int         fd;
    ssize_t     n;
    const char *relative;

    if (host_path[0] != '/') {
        errno = EINVAL;
        return 1;
    }

    relative = host_path + 1;

    root_fd  = open("/proc/1/root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (root_fd == -1)
        return 0;

    fd = openat(root_fd, relative, O_RDONLY | O_CLOEXEC);

    if (fd == -1) {
        close(root_fd);
        return 0;
    }

    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n == -1 && errno == EINTR);

    {
        int saved_errno = errno;

        close(fd);
        close(root_fd);

        if (n == -1) {
            errno = saved_errno;
            return 0;
        }
    }

    buffer[n] = '\0';

    return strcmp(buffer, expected) == 0 ? 1 : 0;
}

static int probe_proc_root_traversal(const char *host_path,
                                     const char *expected)
{
    char path[4096];

    if (snprintf(path, sizeof(path), "/proc/1/root/../%s",
                 host_path[0] == '/' ? host_path + 1 : host_path) >=
        ( int )sizeof(path))
        return 0;

    /*
     * A successful read of the expected host canary means traversal
     * escaped through the proc root.
     */
    return probe_proc_root(path, expected) == 1 ? 1 : 0;
}

static int probe_procfs_private(void)
{
    struct statfs fs;

    if (statfs("/proc", &fs) == -1)
        return 1;

    return fs.f_type == PROC_SUPER_MAGIC ? 0 : 1;
}

static int probe_devfs_private(void)
{
    struct statfs fs;

    if (statfs("/dev", &fs) == -1)
        return 1;

    return fs.f_type == TMPFS_MAGIC ? 0 : 1;
}

static int probe_mount(void)
{
    char path[] = "/tmp/cage-escape-mount-XXXXXX";
    int  fd;
    int  result;

    fd = mkstemp(path);

    if (fd == -1)
        return 1;

    close(fd);
    unlink(path);

    if (mkdir(path, 0700) == -1)
        return 1;

    result = mount("none", path, "tmpfs", 0, "size=4096");

    if (result == -1) {
        rmdir(path);
        return 0;
    }

    umount2(path, MNT_DETACH);
    rmdir(path);

    return 1;
}

static int probe_mknod(const char *path)
{
    if (mknod(path, S_IFCHR | 0600, makedev(1, 3)) == -1)
        return 0;

    unlink(path);

    return 1;
}

static int probe_raw_socket(void)
{
    int fd;

    fd = socket(AF_INET, SOCK_RAW, 1);

    if (fd == -1)
        return 0;

    close(fd);

    return 1;
}

static int probe_setns(const char *path, int namespace_type)
{
    int fd;
    int result;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return 0;

    result = setns(fd, namespace_type);

    {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
    }

    return result == -1 ? 0 : 1;
}

static int probe_chroot(void) { return chroot("/tmp") == -1 ? 0 : 1; }

static int probe_namespace(const char *name, unsigned long expected_dev,
                           unsigned long expected_ino)
{
    char        path[128];
    struct stat st;

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", name) >=
        ( int )sizeof(path))
        return 1;

    if (stat(path, &st) == -1)
        return 1;

    if (( unsigned long )st.st_dev == expected_dev &&
        ( unsigned long )st.st_ino == expected_ino)
        return 1;

    return 0;
}

static int usage(const char *argv0)
{
    fprintf(stderr, "usage: %s <probe> [arguments...]\n", argv0);

    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage(argv[0]);

    if (strcmp(argv[1], "read-file") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return read_file(argv[2]) == -1 ? 1 : 0;
    }

    if (strcmp(argv[1], "write-file") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return write_file(argv[2], argv[3]) == -1 ? 1 : 0;
    }

    if (strcmp(argv[1], "proc-root") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "openat-traversal") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root_openat(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "proc-root-traversal") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root_traversal(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "procfs-private") == 0)
        return probe_procfs_private();

    if (strcmp(argv[1], "devfs-private") == 0)
        return probe_devfs_private();

    if (strcmp(argv[1], "mount") == 0)
        return probe_mount();

    if (strcmp(argv[1], "mknod") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_mknod(argv[2]);
    }

    if (strcmp(argv[1], "raw-socket") == 0)
        return probe_raw_socket();

    if (strcmp(argv[1], "setns") == 0) {
        int type;

        if (argc != 4)
            return usage(argv[0]);

        if (strcmp(argv[2], "mnt") == 0)
            type = CLONE_NEWNS;
        else if (strcmp(argv[2], "pid") == 0)
            type = CLONE_NEWPID;
        else if (strcmp(argv[2], "net") == 0)
            type = CLONE_NEWNET;
        else if (strcmp(argv[2], "ipc") == 0)
            type = CLONE_NEWIPC;
        else if (strcmp(argv[2], "uts") == 0)
            type = CLONE_NEWUTS;
        else if (strcmp(argv[2], "user") == 0)
            type = CLONE_NEWUSER;
        else
            return 2;

        return probe_setns(argv[3], type);
    }

    if (strcmp(argv[1], "chroot") == 0)
        return probe_chroot();

    if (strcmp(argv[1], "namespace") == 0) {
        unsigned long dev;
        unsigned long ino;

        if (argc != 5)
            return usage(argv[0]);

        errno = 0;
        dev   = strtoul(argv[3], NULL, 10);

        if (errno != 0)
            return 2;

        errno = 0;
        ino   = strtoul(argv[4], NULL, 10);

        if (errno != 0)
            return 2;

        return probe_namespace(argv[2], dev, ino);
    }

    return usage(argv[0]);
}
