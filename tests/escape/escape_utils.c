#include "escape_utils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned int tests_run;
static unsigned int tests_passed;

void escape_test_begin(const char *name)
{
    printf("  %-60s ", name);
    fflush(stdout);

    ++tests_run;
}

void escape_test_pass(void)
{
    puts("[PASS]");
    ++tests_passed;
}

void escape_test_fail(const char *message)
{
    puts("[FAIL]");

    if (message != NULL)
        fprintf(stderr, "    %s\n", message);
}

int escape_test_run(void)
{
    printf("\n%u/%u escape tests passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}

int escape_run_process(char *const argv[])
{
    pid_t pid;

    pid = fork();

    if (pid == -1)
        return -1;

    if (pid == 0) {
        execv(argv[0], argv);
        perror("exec test runner");
        _exit(127);
    }

    return escape_wait_process(pid);
}

pid_t escape_start_process(const char *path, char *const argv[])
{
    pid_t pid;

    pid = fork();

    if (pid == -1)
        return -1;

    if (pid == 0) {
        execv(path, argv);
        perror("exec test process");
        _exit(127);
    }

    return pid;
}

int escape_wait_process(pid_t pid)
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

int escape_make_temp_dir(const char *template, char *out, size_t size)
{
    if (strlen(template) + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(out, template);

    return mkdtemp(out) == NULL ? -1 : 0;
}

int escape_make_temp_file(const char *template, char *out, size_t size)
{
    int fd;

    if (strlen(template) + 1 > size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(out, template);

    fd = mkstemp(out);

    if (fd == -1)
        return -1;

    return close(fd);
}

int escape_write_file(const char *path, const char *contents)
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

int escape_read_file(const char *path, char *buf, size_t size)
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

int escape_file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int remove_tree_internal(const char *path)
{
    DIR           *dir;
    struct dirent *entry;
    struct stat    st;
    char           child[PATH_MAX];

    dir = opendir(path);

    if (dir == NULL) {
        if (errno == ENOENT)
            return 0;

        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            ( int )sizeof(child)) {
            errno = ENAMETOOLONG;
            closedir(dir);
            return -1;
        }

        if (lstat(child, &st) == -1) {
            if (errno == ENOENT)
                continue;

            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (remove_tree_internal(child) == -1) {
                closedir(dir);
                return -1;
            }
        } else {
            if (unlink(child) == -1) {
                closedir(dir);
                return -1;
            }
        }
    }

    if (closedir(dir) == -1)
        return -1;

    return rmdir(path);
}

int escape_remove_tree(const char *path) { return remove_tree_internal(path); }

static int copy_file(const char *source, const char *destination)
{
    int  in;
    int  out;
    char buffer[8192];

    in = open(source, O_RDONLY | O_CLOEXEC);

    if (in == -1)
        return -1;

    out = open(destination, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);

    if (out == -1) {
        int saved_errno = errno;

        close(in);
        errno = saved_errno;

        return -1;
    }

    for (;;) {
        ssize_t n;

        do {
            n = read(in, buffer, sizeof(buffer));
        } while (n == -1 && errno == EINTR);

        if (n == 0)
            break;

        if (n == -1) {
            int saved_errno = errno;

            close(in);
            close(out);
            errno = saved_errno;

            return -1;
        }

        {
            size_t written = 0;

            while (written < ( size_t )n) {
                ssize_t w;

                do {
                    w = write(out, buffer + written, ( size_t )n - written);
                } while (w == -1 && errno == EINTR);

                if (w == -1) {
                    int saved_errno = errno;

                    close(in);
                    close(out);
                    errno = saved_errno;

                    return -1;
                }

                if (w == 0) {
                    close(in);
                    close(out);
                    errno = EIO;

                    return -1;
                }

                written += ( size_t )w;
            }
        }
    }

    if (fchmod(out, 0755) == -1) {
        int saved_errno = errno;

        close(in);
        close(out);
        errno = saved_errno;

        return -1;
    }

    if (close(in) == -1) {
        int saved_errno = errno;

        close(out);
        errno = saved_errno;

        return -1;
    }

    return close(out);
}

int escape_create_rootfs(const char *probe_path, const char *template,
                         char *rootfs, size_t size)
{
    char bin[PATH_MAX];
    char probe[PATH_MAX];

    if (escape_make_temp_dir(template, rootfs, size) == -1) {
        perror("escape_create_rootfs: mkdtemp");
        return -1;
    }

    if (snprintf(bin, sizeof(bin), "%s/bin", rootfs) < 0 ||
        ( size_t )snprintf(bin, sizeof(bin), "%s/bin", rootfs) >= sizeof(bin)) {
        errno = ENAMETOOLONG;
        perror("escape_create_rootfs: bin path");
        goto error;
    }

    if (mkdir(bin, 0755) == -1) {
        perror("escape_create_rootfs: mkdir");
        goto error;
    }

    if (snprintf(probe, sizeof(probe), "%s/bin/escape-probe", rootfs) < 0 ||
        ( size_t )snprintf(probe, sizeof(probe), "%s/bin/escape-probe",
                           rootfs) >= sizeof(probe)) {
        errno = ENAMETOOLONG;
        perror("escape_create_rootfs: probe path");
        goto error;
    }

    if (copy_file(probe_path, probe) == -1) {
        perror("escape_create_rootfs: copy_file");
        goto error;
    }

    return 0;

error: {
    int saved_errno = errno;

    escape_remove_tree(rootfs);
    errno = saved_errno;
}

    return -1;
}

int escape_create_config(const char *path, const char *rootfs,
                         const char *extra)
{
    FILE *file;

    file = fopen(path, "w");

    if (file == NULL)
        return -1;

    if (fprintf(file,
                "# cage escape-suite configuration\n"
                "#\n"
                "# The rootfs path is generated uniquely for each test.\n"
                "# Additional mount entries are supplied by the individual\n"
                "# test case.\n"
                "\n"
                "rootfs = \"%s\"\n",
                rootfs) < 0) {
        fclose(file);
        return -1;
    }

    if (extra != NULL && fputs(extra, file) == EOF) {
        fclose(file);
        return -1;
    }

    return fclose(file);
}

int escape_namespace_identity(const char *name, unsigned long *dev,
                              unsigned long *ino)
{
    char        path[128];
    struct stat st;

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", name) >=
        ( int )sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (stat(path, &st) == -1)
        return -1;

    *dev = ( unsigned long )st.st_dev;
    *ino = ( unsigned long )st.st_ino;

    return 0;
}
