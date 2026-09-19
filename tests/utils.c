#include "utils.h"

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

void test_begin(const char *name)
{
    printf("  %-50s ", name);
    fflush(stdout);

    ++tests_run;
}

void test_pass(void)
{
    puts("[PASS]");

    ++tests_passed;
}

void test_fail(const char *message)
{
    puts("[FAIL]");

    if (message != NULL)
        fprintf(stderr, "    %s\n", message);
}

void test_skip(void) { puts("[SKIPPED]"); }

int test_run(void)
{
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}

int run_process(char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }

    return wait_process(pid);
}

pid_t start_process(const char *path, char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execv(path, argv);
        _exit(127);
    }

    return pid;
}

int wait_process(pid_t pid)
{
    int status;

    do {
        if (waitpid(pid, &status, 0) < 0)
            return -1;
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);

    return -1;
}

int make_temp_dir(const char *template, char *out, size_t size)
{
    if (strlen(template) + 1 > size)
        return -1;

    strcpy(out, template);

    return mkdtemp(out) != NULL ? 0 : -1;
}

int make_temp_file(const char *template, char *path, size_t size)
{
    if (strlen(template) + 1 > size)
        return -1;

    strcpy(path, template);

    int fd = mkstemp(path);

    if (fd < 0)
        return -1;

    return close(fd);
}

int write_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return -1;

    size_t len = strlen(contents);
    size_t off = 0;

    while (off < len) {
        ssize_t written = write(fd, contents + off, len - off);

        if (written < 0) {
            int saved_errno = errno;

            close(fd);
            errno = saved_errno;

            return -1;
        }

        if (written == 0) {
            close(fd);
            errno = EIO;

            return -1;
        }

        off += ( size_t )written;
    }

    return close(fd);
}

int read_file(const char *path, char *buf, size_t size)
{
    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;

    ssize_t n           = read(fd, buf, size - 1);
    int     saved_errno = errno;

    if (close(fd) < 0 && n >= 0) {
        buf[0] = '\0';
        return -1;
    }

    if (n < 0) {
        errno = saved_errno;
        return -1;
    }

    buf[n] = '\0';

    return 0;
}

int file_exists(const char *path)
{
    struct stat status;

    return stat(path, &status) == 0;
}

int remove_tree(const char *path)
{
    char command[PATH_MAX + 32];

    if (snprintf(command, sizeof(command), "rm -rf -- '%s'", path) >=
        ( int )sizeof(command))
        return -1;

    return system(command);
}

int count_dirs_with_prefix(const char *directory, const char *prefix)
{
    DIR           *dir;
    struct dirent *entry;
    int            count = 0;

    dir                  = opendir(directory);

    if (dir == NULL)
        return -1;

    while ((entry = readdir(dir)) != NULL) {
        struct stat status;

        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
            continue;

        char path[PATH_MAX];

        if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) >=
            ( int )sizeof(path)) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }

        if (stat(path, &status) == -1) {
            if (errno == ENOENT)
                continue;

            closedir(dir);
            return -1;
        }

        if (!S_ISDIR(status.st_mode))
            continue;

        ++count;
    }

    if (closedir(dir) == -1)
        return -1;

    return count;
}

int create_rootfs(const char *probe_path, const char *template, char *rootfs,
                  size_t size)
{
    if (make_temp_dir(template, rootfs, size) < 0)
        return -1;

    char bin[PATH_MAX];

    if (snprintf(bin, sizeof(bin), "%s/bin", rootfs) >= ( int )sizeof(bin))
        goto error;

    if (mkdir(bin, 0755) < 0)
        goto error;

    char probe[PATH_MAX];

    if (snprintf(probe, sizeof(probe), "%s/bin/probe", rootfs) >=
        ( int )sizeof(probe))
        goto error;

    int in = open(probe_path, O_RDONLY);

    if (in < 0)
        goto error;

    int out = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0755);

    if (out < 0) {
        close(in);
        goto error;
    }

    char    buffer[8192];
    ssize_t n;

    while ((n = read(in, buffer, sizeof(buffer))) > 0) {
        size_t off = 0;

        while (off < ( size_t )n) {
            ssize_t written = write(out, buffer + off, ( size_t )n - off);

            if (written < 0) {
                int saved_errno = errno;

                close(in);
                close(out);
                errno = saved_errno;

                goto error;
            }

            if (written == 0) {
                close(in);
                close(out);
                errno = EIO;

                goto error;
            }

            off += ( size_t )written;
        }
    }

    int saved_errno = errno;

    close(in);
    close(out);

    if (n < 0) {
        errno = saved_errno;
        goto error;
    }

    return 0;

error:
    remove_tree(rootfs);
    return -1;
}

int create_config(const char *path, const char *rootfs, const char *extra)
{
    FILE *file = fopen(path, "w");

    if (file == NULL)
        return -1;

    if (fprintf(file, "rootfs = \"%s\"\n", rootfs) < 0) {
        fclose(file);
        return -1;
    }

    if (extra != NULL && fputs(extra, file) == EOF) {
        fclose(file);
        return -1;
    }

    return fclose(file);
}
