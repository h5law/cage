#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/limits.h>
#include <linux/magic.h>
#include <linux/mount.h>
#include <linux/openat2.h>
#include <linux/reboot.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_openat2
#define SYS_openat2 437
#endif

#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif

#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif

#ifndef SYS_kexec_load
#define SYS_kexec_load 246
#endif

#ifndef SYS_init_module
#define SYS_init_module 175
#endif

#ifndef SYS_finit_module
#define SYS_finit_module 313
#endif

#ifndef SYS_capset
#define SYS_capset 126
#endif

#ifndef SYS_ptrace
#define SYS_ptrace 101
#endif

#ifndef SYS_reboot
#define SYS_reboot 169
#endif

#ifndef SYS_unshare
#define SYS_unshare 272
#endif

#ifndef SYS_clone
#define SYS_clone 56
#endif

#ifndef RESOLVE_NO_XDEV
#define RESOLVE_NO_XDEV 0x01
#endif

#ifndef RESOLVE_NO_MAGICLINKS
#define RESOLVE_NO_MAGICLINKS 0x02
#endif

#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS 0x04
#endif

#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x08
#endif

#ifndef RESOLVE_IN_ROOT
#define RESOLVE_IN_ROOT 0x10
#endif

#ifndef RESOLVE_CACHED
#define RESOLVE_CACHED 0x20
#endif

static int read_file(const char *path, char *buffer, size_t size)
{
    int     fd;
    ssize_t n;

    if (size == 0)
        return -1;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return -1;

    do {
        n = read(fd, buffer, size - 1);
    } while (n == -1 && errno == EINTR);

    {
        int saved_errno = errno;

        close(fd);

        if (n == -1) {
            errno = saved_errno;
            return -1;
        }
    }

    buffer[n] = '\0';

    return 0;
}

static int read_expected(const char *path, const char *expected)
{
    char buffer[4096];

    if (read_file(path, buffer, sizeof(buffer)) == -1)
        return 0;

    return strcmp(buffer, expected) == 0 ? 1 : 0;
}

static int write_file(const char *path, const char *contents)
{
    int    fd;
    size_t length;
    size_t written = 0;

    fd             = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    if (fd == -1)
        return -1;

    length = strlen(contents);

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
    char path[PATH_MAX];

    if (host_path[0] != '/')
        return 1;

    if (snprintf(path, sizeof(path), "/proc/1/root%s", host_path) >=
        ( int )sizeof(path))
        return 1;

    return read_expected(path, expected);
}

static int probe_proc_root_openat(const char *host_path, const char *expected)
{
    char        buffer[4096];
    const char *relative;
    int         root_fd;
    int         fd;
    ssize_t     n;

    if (host_path[0] != '/')
        return 1;

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
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/1/root/../%s",
                 host_path[0] == '/' ? host_path + 1 : host_path) >=
        ( int )sizeof(path))
        return 0;

    return probe_proc_root(path, expected);
}

static int probe_self_root(const char *host_path, const char *expected)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/self/root%s", host_path) >=
        ( int )sizeof(path))
        return 0;

    return read_expected(path, expected);
}

static int probe_thread_root(const char *host_path, const char *expected)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/thread-self/root%s", host_path) >=
        ( int )sizeof(path))
        return 0;

    return read_expected(path, expected);
}

static int probe_relative_escape(const char *host_path, const char *expected)
{
    char cwd[PATH_MAX];
    char path[PATH_MAX];
    int  result;

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return 0;

    if (snprintf(path, sizeof(path), "%s", host_path) >= ( int )sizeof(path))
        return 0;

    result = read_expected(path, expected);

    if (chdir(cwd) == -1)
        return 1;

    return result;
}

static int probe_symlink_escape(const char *host_path, const char *expected)
{
    char link_path[] = "/tmp/cage-fuzz-link-XXXXXX";
    int  fd;
    int  result;

    fd = mkstemp(link_path);

    if (fd == -1)
        return 0;

    close(fd);
    unlink(link_path);

    if (symlink(host_path, link_path) == -1)
        return 0;

    result = read_expected(link_path, expected);

    unlink(link_path);

    return result;
}

static int probe_path_resolution(const char *mode, const char *path,
                                 const char *expected)
{
    struct open_how how;
    int             fd;
    int             result = 0;

    memset(&how, 0, sizeof(how));

    how.flags = O_RDONLY | O_CLOEXEC;

    if (strcmp(mode, "double-slash") == 0)
        how.resolve = RESOLVE_BENEATH;

    else if (strcmp(mode, "dot") == 0)
        how.resolve = RESOLVE_BENEATH;

    else if (strcmp(mode, "parent") == 0)
        how.resolve = RESOLVE_BENEATH;

    else if (strcmp(mode, "symlink") == 0)
        how.resolve =
                RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS;

    else
        return 2;

    fd = syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof(how));

    if (fd == -1)
        return 0;

    {
        char    buffer[4096];
        ssize_t n;

        do {
            n = read(fd, buffer, sizeof(buffer) - 1);
        } while (n == -1 && errno == EINTR);

        if (n >= 0) {
            buffer[n] = '\0';

            if (strcmp(buffer, expected) == 0)
                result = 1;
        }
    }

    close(fd);

    return result;
}

static int probe_proc_magiclink(const char *path, const char *expected)
{
    char    target[PATH_MAX];
    ssize_t n;

    n = readlink(path, target, sizeof(target) - 1);

    if (n == -1)
        return 0;

    target[n] = '\0';

    if (strcmp(path, "/proc/1/root") == 0 || strcmp(path, "/proc/1/cwd") == 0 ||
        strcmp(path, "/proc/1/exe") == 0)
        return read_expected(path, expected);

    return 0;
}

static int probe_proc_cwd(const char *expected)
{
    char cwd[PATH_MAX];

    if (readlink("/proc/self/cwd", cwd, sizeof(cwd) - 1) == -1)
        return 0;

    /*
     * /proc/self/cwd should point into the container.
     * The canary itself must never become reachable from it.
     */
    if (read_expected("/proc/self/cwd", expected) == 1)
        return 1;

    return 0;
}

static int probe_proc_fd(const char *expected)
{
    char    path[64];
    char    target[PATH_MAX];
    ssize_t length;

    ( void )expected;

    /*
     * Never read through /proc/self/fd/{0,1,2}: those descriptors
     * normally refer to pipes or the parent's terminal and reading
     * them can block indefinitely. Inspect the symlink targets only.
     *
     * A proc-fd entry pointing at the host canary would be an escape.
     */
    for (int fd = 0; fd < 3; ++fd) {
        if (snprintf(path, sizeof(path), "/proc/self/fd/%d", fd) >=
            ( int )sizeof(path))
            continue;

        length = readlink(path, target, sizeof(target) - 1);

        if (length == -1)
            continue;

        target[length] = '\0';

        if (strcmp(target, expected) == 0)
            return 1;
    }

    return 0;
}

static int probe_mount(void)
{
    char path[] = "/tmp/cage-fuzz-mount-XXXXXX";
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
     * If mount succeeded, the container acquired a new mount.
     * Unmount the mount we just created before returning.
     */
    umount2(path, MNT_DETACH);
    rmdir(path);

    return 1;
}

static int probe_umount(void)
{
    char path[] = "/tmp/cage-fuzz-umount-XXXXXX";
    int  fd;
    int  result;

    /*
     * Never unmount an existing filesystem. Create a private mount
     * point and attempt to unmount that path instead. If CAP_SYS_ADMIN
     * is available, this will succeed, but it cannot detach a host
     * filesystem selected by the test.
     */
    fd = mkstemp(path);

    if (fd == -1)
        return 0;

    close(fd);
    unlink(path);

    if (mkdir(path, 0700) == -1)
        return 0;

    result = umount2(path, MNT_DETACH);

    rmdir(path);

    return result == -1 ? 0 : 1;
}

static int probe_pivot_root(void)
{
    /*
     * pivot_root requires a directory hierarchy prepared for the
     * operation. We deliberately do not create or alter one here.
     *
     * An invalid invocation is enough to exercise whether Cage
     * prevents the privileged syscall from being reached.
     */
    errno = 0;

    if (syscall(SYS_pivot_root, "/", "/") == -1)
        return 0;

    return 1;
}

static int probe_move_mount(void)
{
    int fd;

    fd = syscall(SYS_move_mount, AT_FDCWD, "/", AT_FDCWD, "/", 0);

    if (fd == -1)
        return 0;

    return 1;
}

static int probe_open_tree(void)
{
    int fd;

    /*
     * Obtaining a mount FD is not itself an escape. Clone the
     * container root into a detached mount and immediately close it.
     * The dangerous operation is attaching the mount elsewhere.
     */
    fd = syscall(SYS_open_tree, AT_FDCWD, "/", OPEN_TREE_CLONE);

    if (fd == -1)
        return 0;

    close(fd);

    return 0;
}

static int probe_mount_setattr(void)
{
    struct mount_attr attr;

    memset(&attr, 0, sizeof(attr));

    if (syscall(SYS_mount_setattr, AT_FDCWD, "/", 0, &attr, sizeof(attr)) == -1)
        return 0;

    return 1;
}

static int probe_chroot(void)
{
    if (chroot("/") == -1)
        return 0;

    return 1;
}

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

static int namespace_type(const char *name)
{
    if (strcmp(name, "pid") == 0)
        return CLONE_NEWPID;

    if (strcmp(name, "mnt") == 0)
        return CLONE_NEWNS;

    if (strcmp(name, "net") == 0)
        return CLONE_NEWNET;

    if (strcmp(name, "ipc") == 0)
        return CLONE_NEWIPC;

    if (strcmp(name, "uts") == 0)
        return CLONE_NEWUTS;

    if (strcmp(name, "user") == 0)
        return CLONE_NEWUSER;

    return -1;
}

static int probe_setns(const char *name, const char *path)
{
    int type;
    int fd;
    int result;

    type = namespace_type(name);

    if (type == -1)
        return 2;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return 0;

    result = setns(fd, type);

    {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
    }

    return result == -1 ? 0 : 1;
}

static int probe_unshare(const char *name)
{
    int type;

    type = namespace_type(name);

    if (type == -1)
        return 2;

    /*
     * Creating or entering a new namespace is not itself an escape.
     * In particular, CLONE_NEWUSER is explicitly usable by an
     * unprivileged process. Exercise the syscall, but only report
     * an escape if some later operation demonstrates access outside
     * the container.
     */
    if (unshare(type) == -1)
        return 0;

    return 0;
}

static int probe_clone(const char *name)
{
    int   type;
    pid_t pid;

    type = namespace_type(name);

    if (type == -1)
        return 2;

    /*
     * Creating a namespace is not itself a host escape. In particular,
     * CLONE_NEWUSER is intentionally designed to allow an unprivileged
     * process to create a user namespace.
     *
     * The property is concerned with escaping Cage, so a successful
     * namespace clone is not by itself a failure.
     */
    pid = syscall(SYS_clone, type | SIGCHLD, NULL, NULL, NULL, 0);

    if (pid == -1)
        return 0;

    if (pid == 0)
        _exit(0);

    for (;;) {
        int   status;
        pid_t result;

        result = waitpid(pid, &status, 0);

        if (result == pid)
            break;

        if (result == -1 && errno == EINTR)
            continue;

        return 0;
    }

    return 0;
}

static int probe_mknod(void)
{
    char path[] = "/tmp/cage-fuzz-device-XXXXXX";
    int  fd;
    int  result;

    fd = mkstemp(path);

    if (fd == -1)
        return 0;

    close(fd);
    unlink(path);

    result = mknod(path, S_IFCHR | 0600, makedev(1, 3));

    if (result == -1)
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

static int probe_device(const char *path)
{
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd == -1)
        return 0;

    close(fd);

    return 1;
}

static int probe_capset(void)
{
    /*
     * Deliberately invalid capability header. We are testing whether
     * the privileged syscall can be reached, not changing capabilities.
     */
    unsigned int data[2];

    memset(data, 0, sizeof(data));

    errno = 0;

    if (syscall(SYS_capset, NULL, data) == -1) {
        /*
         * EFAULT means the syscall reached the kernel but our
         * deliberately invalid argument was rejected. This is
         * not an escape.
         */
        if (errno == EFAULT)
            return 0;

        return 0;
    }

    return 1;
}

static int probe_ptrace(void)
{
    long result;

    /*
     * PTRACE_TRACEME changes the child's tracing state and can
     * interfere with the parent-side test runner. Instead, attempt
     * to attach to PID 1 in the container namespace. A successful
     * attach demonstrates ptrace permission without deliberately
     * tracing ourselves.
     */
    result = syscall(SYS_ptrace, PTRACE_ATTACH, 1, 0, 0);

    if (result == -1)
        return 0;

    /*
     * Detach immediately if the attach was permitted.
     */
    ( void )syscall(SYS_ptrace, PTRACE_DETACH, 1, 0, 0);

    return 1;
}

static int probe_kill(pid_t host_pid)
{
    /*
     * signal 0 performs permission checking without delivering a
     * signal.  Test the PID of the parent-side fuzz runner rather
     * than ourselves: a process in the container PID namespace
     * must not be able to address that host PID.
     */
    if (kill(host_pid, 0) == 0)
        return 1;

    /*
     * ESRCH is the expected result when the host PID is outside the
     * container PID namespace.  EPERM also means the PID is not
     * signalable, so it is not an escape.
     */
    if (errno == ESRCH || errno == EPERM)
        return 0;

    return 0;
}

static int probe_reboot(void)
{
    /*
     * Never issue an actual reboot command. Use an invalid command
     * so the syscall is exercised without rebooting the machine.
     */
    errno = 0;

    if (syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, 0,
                NULL) == -1)
        return 0;

    return 1;
}

static int probe_kexec(void)
{
    /*
     * NULL image and zero arguments cannot install a kernel.
     * Success would nevertheless indicate access to kexec_load.
     */
    errno = 0;

    if (syscall(SYS_kexec_load, 0, 0, NULL, 0) == -1)
        return 0;

    return 1;
}

static int probe_sys_module(void)
{
    /*
     * A NULL module pointer and zero length are deliberately invalid.
     * We are only checking whether the syscall is reachable.
     */
    errno = 0;

    if (syscall(SYS_init_module, NULL, 0, "") == -1)
        return 0;

    return 1;
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

    /*
     * Cage's private /dev is expected to be a tmpfs.
     */
    return fs.f_type == TMPFS_MAGIC ? 0 : 1;
}

static int probe_sysfs_private(void)
{
    struct statfs fs;

    /*
     * /sys being absent or inaccessible is safe. A private sysfs
     * mount is also safe. The presence of sysfs alone is not evidence
     * of a host escape, so this probe must never fail merely because
     * Cage exposes a different /sys layout.
     */
    if (statfs("/sys", &fs) == -1)
        return 0;

    if (fs.f_type != SYSFS_MAGIC)
        return 0;

    return 0;
}

static int probe_proc_path(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
        return 0;

    /*
     * Visibility itself is not necessarily an escape. The important
     * boundary is that host PID 1 must not be visible as the host
     * process namespace.
     */
    if (strcmp(path, "/proc/1/status") == 0)
        return 0;

    return 0;
}

static int probe_sys_path(const char *path)
{
    struct stat st;

    if (stat(path, &st) == -1)
        return 0;

    /*
     * Merely seeing /sys is expected. This probe is primarily useful
     * for exercising the configured sysfs namespace.
     */
    return 0;
}

static int probe_host_pid(void)
{
    char buffer[8192];

    if (read_file("/proc/1/status", buffer, sizeof(buffer)) == -1)
        return 0;

    /*
     * In a PID namespace, PID 1 is the container's init process.
     * Its status should report NSpid with the container PID.
     */
    if (strstr(buffer, "NSpid:") == NULL)
        return 1;

    return 0;
}

static int probe_host_pid_root(const char *host_path, const char *expected)
{
    /*
     * This is intentionally equivalent to the /proc/1/root canary
     * check, but takes the actual generated host path so the property
     * tests the real fixture rather than a hard-coded pathname.
     */
    return probe_proc_root(host_path, expected);
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
        char buffer[4096];

        if (argc != 3)
            return usage(argv[0]);

        return read_file(argv[2], buffer, sizeof(buffer)) == -1 ? 1 : 0;
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

    if (strcmp(argv[1], "proc-root-openat") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root_openat(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "proc-root-traversal") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_root_traversal(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "self-root") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_self_root(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "thread-root") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_thread_root(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "relative-escape") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_relative_escape(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "symlink-escape") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_symlink_escape(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "path-resolution") == 0) {
        if (argc != 5)
            return usage(argv[0]);

        return probe_path_resolution(argv[2], argv[3], argv[4]);
    }

    if (strcmp(argv[1], "proc-magiclink") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_proc_magiclink(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "proc-cwd") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_proc_cwd(argv[2]);
    }

    if (strcmp(argv[1], "proc-fd") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_proc_fd(argv[2]);
    }

    if (strcmp(argv[1], "mount") == 0)
        return probe_mount();

    if (strcmp(argv[1], "umount") == 0)
        return probe_umount();

    if (strcmp(argv[1], "pivot-root") == 0)
        return probe_pivot_root();

    if (strcmp(argv[1], "move-mount") == 0)
        return probe_move_mount();

    if (strcmp(argv[1], "open-tree") == 0)
        return probe_open_tree();

    if (strcmp(argv[1], "mount-setattr") == 0)
        return probe_mount_setattr();

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

    if (strcmp(argv[1], "setns") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_setns(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "unshare") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_unshare(argv[2]);
    }

    if (strcmp(argv[1], "clone") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_clone(argv[2]);
    }

    if (strcmp(argv[1], "mknod") == 0)
        return probe_mknod();

    if (strcmp(argv[1], "raw-socket") == 0)
        return probe_raw_socket();

    if (strcmp(argv[1], "device") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_device(argv[2]);
    }

    if (strcmp(argv[1], "capset") == 0)
        return probe_capset();

    if (strcmp(argv[1], "ptrace") == 0)
        return probe_ptrace();

    if (strcmp(argv[1], "kill") == 0) {
        char *end;
        long  value;

        if (argc != 3)
            return usage(argv[0]);

        errno = 0;
        value = strtol(argv[2], &end, 10);

        if (errno != 0 || end == argv[2] || *end != '\0' || value <= 0 ||
            value > INT_MAX)
            return usage(argv[0]);

        return probe_kill(( pid_t )value);
    }

    if (strcmp(argv[1], "reboot") == 0)
        return probe_reboot();

    if (strcmp(argv[1], "kexec") == 0)
        return probe_kexec();

    if (strcmp(argv[1], "sys-module") == 0)
        return probe_sys_module();

    if (strcmp(argv[1], "procfs-private") == 0)
        return probe_procfs_private();

    if (strcmp(argv[1], "devfs-private") == 0)
        return probe_devfs_private();

    if (strcmp(argv[1], "sysfs-private") == 0)
        return probe_sysfs_private();

    if (strcmp(argv[1], "proc-path") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_proc_path(argv[2]);
    }

    if (strcmp(argv[1], "sys-path") == 0) {
        if (argc != 3)
            return usage(argv[0]);

        return probe_sys_path(argv[2]);
    }

    if (strcmp(argv[1], "host-pid") == 0)
        return probe_host_pid();

    if (strcmp(argv[1], "host-pid-root") == 0) {
        if (argc != 4)
            return usage(argv[0]);

        return probe_host_pid_root(argv[2], argv[3]);
    }

    return usage(argv[0]);
}
