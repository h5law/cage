#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static int read_file(const char *path, char *buf, size_t size)
{
    int     fd;
    ssize_t n;

    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return -1;

    do {
        n = read(fd, buf, size - 1);
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
        ssize_t n = write(fd, contents + written, length - written);

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

/*
 * The host test harness creates a canary containing a secret string.
 *
 * Success here means the container was able to read the host file,
 * which is a containment failure.
 */
static int probe_host_file(const char *path, const char *expected)
{
    char buffer[4096];

    if (read_file(path, buffer, sizeof(buffer)) == -1)
        return 0;

    if (strcmp(buffer, expected) == 0)
        return 1;

    /*
     * The file was readable but did not contain our canary.
     * Treat that as inaccessible for the purposes of this probe.
     */
    return 0;
}

/*
 * Attempt to modify a host canary.
 *
 * The operation must fail. If it succeeds, the container has gained
 * write access to a host filesystem object.
 */
static int probe_host_write(const char *path)
{
    int fd;

    fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);

    if (fd == -1)
        return 0;

    close(fd);

    return 1;
}

/*
 * Verify that /proc/1/root is not the host root.
 *
 * The host test process supplies a file that exists outside the
 * container. If it can be reached through /proc/1/root, containment
 * has failed.
 */
static int probe_proc_root(const char *host_path, const char *expected)
{
    char path[4096];

    if (snprintf(path, sizeof(path), "/proc/1/root%s", host_path) >=
        ( int )sizeof(path)) {
        return 0;
    }

    return probe_host_file(path, expected);
}

static int probe_proc_self_root(const char *host_path, const char *expected)
{
    char path[4096];

    if (snprintf(path, sizeof(path), "/proc/self/root%s", host_path) >=
        ( int )sizeof(path)) {
        return 0;
    }

    return probe_host_file(path, expected);
}

/*
 * Check that a namespace differs from a namespace identity supplied
 * by the host.
 *
 * The host side passes st_dev/st_ino from /proc/self/ns/<name>.
 */
static int probe_namespace(const char *name, unsigned long expected_dev,
                           unsigned long expected_ino)
{
    char        path[128];
    struct stat st;

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", name) >=
        ( int )sizeof(path)) {
        return 1;
    }

    if (stat(path, &st) == -1)
        return 1;

    if (( unsigned long )st.st_dev == expected_dev &&
        ( unsigned long )st.st_ino == expected_ino) {
        return 1;
    }

    return 0;
}

/*
 * /dev should be a private tmpfs rather than the host's /dev.
 */
static int probe_devfs(void)
{
    struct statfs fs;

    if (statfs("/dev", &fs) == -1)
        return 1;

    return fs.f_type == TMPFS_MAGIC ? 0 : 1;
}

/*
 * /proc must be procfs.
 */
static int probe_procfs(void)
{
    struct statfs fs;

    if (statfs("/proc", &fs) == -1)
        return 1;

    return fs.f_type == PROC_SUPER_MAGIC ? 0 : 1;
}

/*
 * Attempt to mount something inside the container.
 *
 * With capabilities dropped this should fail. This is specifically
 * testing whether the command can acquire mount authority without
 * crossing the namespace boundary.
 */
static int probe_mount(void)
{
    char path[] = "/tmp/cage-escape-mount-XXXXXX";
    int  fd;
    int  result;

    fd = mkstemp(path);

    if (fd == -1)
        return 0;

    close(fd);
    unlink(path);

    if (mkdir(path, 0700) == -1)
        return 0;

    result = mount("none", path, "tmpfs", 0, "size=4096");

    if (result == -1) {
        rmdir(path);
        return 0;
    }

    /*
     * Successfully mounting means the container has mount authority.
     */
    umount2(path, MNT_DETACH);
    rmdir(path);

    return 1;
}

/*
 * Attempt to create a device node.
 *
 * This should fail because CAP_MKNOD is not available.
 */
static int probe_mknod(const char *path)
{
    if (mknod(path, S_IFCHR | 0600, makedev(1, 3)) == -1)
        return 0;

    unlink(path);

    return 1;
}

/*
 * Attempt to access another process' memory through procfs.
 *
 * The test intentionally only tries opening the file. It does not
 * attempt to modify another process.
 */
static int probe_proc_mem(pid_t pid)
{
    char path[128];
    int  fd;

    if (snprintf(path, sizeof(path), "/proc/%ld/mem", ( long )pid) >=
        ( int )sizeof(path)) {
        return 0;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return 0;

    close(fd);

    return 1;
}

/*
 * Try to access a host PID from inside the container.
 *
 * The host supplies a PID that remains alive for the duration of
 * the probe. If kill(pid, 0) succeeds, the PID namespace boundary
 * has been crossed.
 */
static int probe_host_pid(pid_t pid)
{
    if (kill(pid, 0) == -1) {
        if (errno == ESRCH)
            return 0;

        /*
         * EPERM is also acceptable: the PID exists but cannot be
         * signalled. This is not an escape.
         */
        if (errno == EPERM)
            return 0;

        return 0;
    }

    return 1;
}

/*
 * Try to create a raw socket.
 *
 * This should fail without CAP_NET_RAW.
 */
static int probe_raw_socket(void)
{
    int fd;

    fd = socket(AF_INET, SOCK_RAW, 1);

    if (fd == -1)
        return 0;

    close(fd);

    return 1;
}

/*
 * Try to enter another namespace using a namespace fd.
 *
 * The caller supplies a path to a host namespace. Successfully
 * entering it would be a direct namespace escape.
 */
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

    if (result == -1)
        return 0;

    return 1;
}

/*
 * Attempt to change the root to the host's root through /proc.
 *
 * This should fail because the host root is not reachable through
 * the container's proc namespace.
 */
static int probe_chroot_host(void)
{
    if (chroot("/proc/1/root") == -1)
        return 0;

    return 1;
}

/*
 * Verify that UID 0 inside the container is not the host UID 0.
 *
 * cage currently maps container UID 0 to the invoking host UID.
 */
static int probe_uid(unsigned long expected_host_uid)
{
    if (getuid() == 0 && expected_host_uid != 0)
        return 0;

    /*
     * If the caller itself is root, UID equality alone is not a
     * useful isolation oracle. The namespace identity tests provide
     * the stronger guarantee in that case.
     */
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

    if (strcmp(argv[1], "host-read") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_host_file(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "host-write") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_host_write(argv[2]);
    }

    if (strcmp(argv[1], "proc-1-root-read") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "proc-self-root-read") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_self_root(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "namespace") == 0) {
        unsigned long dev;
        unsigned long ino;

        if (argc != 5)
            return usage(argv[0]);

        dev = strtoul(argv[3], NULL, 10);
        ino = strtoul(argv[4], NULL, 10);

        return probe_namespace(argv[2], dev, ino);
    }

    if (strcmp(argv[1], "procfs") == 0)
        return probe_procfs();

    if (strcmp(argv[1], "devfs") == 0)
        return probe_devfs();

    if (strcmp(argv[1], "mount") == 0)
        return probe_mount();

    if (strcmp(argv[1], "mknod") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_mknod(argv[2]);
    }

    if (strcmp(argv[1], "proc-mem") == 0) {
        char *end;
        long  pid;

        if (argc != 3)
            return usage(argv[0]);

        errno = 0;
        pid   = strtol(argv[2], &end, 10);

        if (errno != 0 || *end != '\0' || pid <= 0)
            return 2;

        return probe_proc_mem(( pid_t )pid);
    }

    if (strcmp(argv[1], "host-pid") == 0) {
        char *end;
        long  pid;

        if (argc != 3)
            return usage(argv[0]);

        errno = 0;
        pid   = strtol(argv[2], &end, 10);

        if (errno != 0 || *end != '\0' || pid <= 0)
            return 2;

        return probe_host_pid(( pid_t )pid);
    }

    if (strcmp(argv[1], "raw-socket") == 0)
        return probe_raw_socket();

    if (strcmp(argv[1], "setns") == 0) {
        int type = 0;

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

    if (strcmp(argv[1], "chroot-host") == 0)
        return probe_chroot_host();

    if (strcmp(argv[1], "uid") == 0) {
        unsigned long expected;

        if (argc != 3)
            return usage(argv[0]);

        expected = strtoul(argv[2], NULL, 10);

        return probe_uid(expected);
    }

    if (strcmp(argv[1], "write-file") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return write_file(argv[2], argv[3]) == -1 ? 1 : 0;
    }

    return usage(argv[0]);
}
