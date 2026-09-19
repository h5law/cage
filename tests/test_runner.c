#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_ROOTFS  "/tmp/cage-test-rootfs-XXXXXX"
#define TEST_RUNTIME "/tmp/cage-test-runtime-XXXXXX"
#define TEST_MOUNT   "/tmp/cage-test-mount-XXXXXX"

static const char *cage_path;
static const char *probe_path;

static int failures;

static void pass(const char *name) { printf("PASS: %s\n", name); }

static void fail(const char *name)
{
    fprintf(stderr, "FAIL: %s\n", name);
    failures++;
}

static int run_process(char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }

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

static pid_t start_cage(char *const argv[])
{
    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        execv(cage_path, argv);
        _exit(127);
    }

    return pid;
}

static int wait_process(pid_t pid)
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

static int make_temp_dir(const char *template, char *out, size_t size)
{
    if (strlen(template) + 1 > size)
        return -1;

    strcpy(out, template);

    return mkdtemp(out) ? 0 : -1;
}

static int write_file(const char *path, const char *contents)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return -1;

    size_t  len     = strlen(contents);
    ssize_t written = write(fd, contents, len);

    int saved_errno = errno;

    close(fd);

    if (written != ( ssize_t )len) {
        errno = saved_errno;
        return -1;
    }

    return 0;
}

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int read_file(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;

    ssize_t n           = read(fd, buf, size - 1);
    int     saved_errno = errno;

    close(fd);

    if (n < 0) {
        errno = saved_errno;
        return -1;
    }

    buf[n] = '\0';

    return 0;
}

static int create_rootfs(char *rootfs, size_t size)
{
    if (make_temp_dir(TEST_ROOTFS, rootfs, size) < 0)
        return -1;

    char bin[PATH_MAX];

    if (snprintf(bin, sizeof(bin), "%s/bin", rootfs) >= ( int )sizeof(bin))
        return -1;

    if (mkdir(bin, 0755) < 0)
        return -1;

    char probe[PATH_MAX];

    if (snprintf(probe, sizeof(probe), "%s/bin/probe", rootfs) >=
        ( int )sizeof(probe))
        return -1;

    int in = open(probe_path, O_RDONLY);

    if (in < 0)
        return -1;

    int out = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0755);

    if (out < 0) {
        close(in);
        return -1;
    }

    char    buf[8192];
    ssize_t n;

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        char   *p         = buf;
        ssize_t remaining = n;

        while (remaining > 0) {
            ssize_t written = write(out, p, remaining);

            if (written < 0) {
                close(in);
                close(out);
                return -1;
            }

            p         += written;
            remaining -= written;
        }
    }

    int saved_errno = errno;

    close(in);
    close(out);

    if (n < 0) {
        errno = saved_errno;
        return -1;
    }

    return 0;
}

static int remove_tree(const char *path)
{
    char command[PATH_MAX + 32];

    if (snprintf(command, sizeof(command), "rm -rf -- '%s'", path) >=
        ( int )sizeof(command))
        return -1;

    return system(command);
}

static int create_config(const char *path, const char *rootfs,
                         const char *extra)
{
    FILE *fp = fopen(path, "w");

    if (!fp)
        return -1;

    if (fprintf(fp, "rootfs = \"%s\"\n", rootfs) < 0) {
        fclose(fp);
        return -1;
    }

    if (extra && fputs(extra, fp) == EOF) {
        fclose(fp);
        return -1;
    }

    return fclose(fp);
}

static int test_exit_status(void)
{
    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            NULL,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"42",
            NULL,
    };

    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    argv[2]    = config;

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 42)
        return -1;

    pass("command exit status is propagated");
    return 0;
}

static int test_signal(int sig, const char *name)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"wait-signal",
            "15",
            NULL,
    };

    pid_t pid = start_cage(argv);

    if (pid < 0)
        goto error_rootfs;

    usleep(100000);

    if (kill(pid, sig) < 0) {
        kill(pid, SIGKILL);
        wait_process(pid);
        goto error_rootfs;
    }

    int status = wait_process(pid);

    unlink(config);
    remove_tree(rootfs);

    if (status != 128 + sig)
        return -1;

    pass(name);
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_namespace(const char *probe, const char *name)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    struct stat st;
    char        path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", probe) >=
        ( int )sizeof(path))
        goto error_rootfs;

    if (stat(path, &st) < 0)
        goto error_rootfs;

    char dev[32];
    char ino[32];

    snprintf(dev, sizeof(dev), "%llu", ( unsigned long long )st.st_dev);
    snprintf(ino, sizeof(ino), "%llu", ( unsigned long long )st.st_ino);

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"namespace",
            ( char * )probe,
            dev,
            ino,
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass(name);
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_uid_mapping(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];
    char uid_string[32];
    char gid_string[32];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    snprintf(uid_string, sizeof(uid_string), "%lu", ( unsigned long )getuid());
    snprintf(gid_string, sizeof(gid_string), "%lu", ( unsigned long )getgid());

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"uid-map",
            uid_string,
            gid_string,
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("user namespace identity mapping");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_tmpfs(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"tmpfs",
            ( char * )"/tmp",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("/tmp is a private writable tmpfs");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_proc(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"proc",
            ( char * )"/proc",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("/proc is a proc filesystem");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_dev(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"dev",
            ( char * )"/dev/null",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0) {
        fprintf(stderr, "dev probe exited with status %d\n", status);
        return -1;
    }

    pass("/dev is a private tmpfs with device nodes");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_capabilities(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",     config,
            ( char * )"/bin/probe", ( char * )"capabilities", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("capabilities are dropped");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_no_new_privs(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",     config,
            ( char * )"/bin/probe", ( char * )"no-new-privs", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("no_new_privs is enabled");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_fd_inheritance(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",       config,
            ( char * )"/bin/probe", ( char * )"fd-inheritance", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0)
        return -1;

    pass("internal file descriptors are not inherited");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_exec_failure(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/does-not-exist",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 127)
        return -1;

    pass("exec failure is propagated");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_invalid_rootfs(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    if (create_config(config, "/tmp/cage-rootfs-that-does-not-exist", NULL) <
        0) {
        unlink(config);
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            "0",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);

    if (status == 0)
        return -1;

    pass("invalid rootfs is rejected");
    return 0;
}

static int test_parent_death(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (create_config(config, rootfs, NULL) < 0)
        goto error_rootfs;

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config", config,
            ( char * )"/bin/probe", ( char * )"hold",     NULL,
    };

    pid_t pid = start_cage(argv);

    if (pid < 0)
        goto error_rootfs;

    usleep(200000);

    if (kill(pid, SIGKILL) < 0) {
        wait_process(pid);
        goto error_rootfs;
    }

    wait_process(pid);

    usleep(200000);

    unlink(config);
    remove_tree(rootfs);

    pass("container dies when cage supervisor dies");
    return 0;

error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_configured_writable_mount(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];
    char mount_dir[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (make_temp_dir(TEST_MOUNT, mount_dir, sizeof(mount_dir)) < 0)
        goto error_rootfs;

    if (create_config(config, rootfs,
                      "\n[[mounts]]\n"
                      "source = \"") < 0)
        goto error_mount;

    FILE *fp = fopen(config, "a");

    if (!fp)
        goto error_mount;

    fprintf(fp, "%s\"\n", mount_dir);
    fprintf(fp, "target = \"/data\"\n");
    fclose(fp);

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"write-file",
            ( char * )"/data/persistent",
            ( char * )"persistent-data",
            NULL,
    };

    int status = run_process(argv);

    if (status != 0)
        goto error_mount;

    char persistent[PATH_MAX];

    if (snprintf(persistent, sizeof(persistent), "%s/persistent", mount_dir) >=
        ( int )sizeof(persistent))
        goto error_mount;

    if (!file_exists(persistent))
        goto error_mount;

    char contents[128];

    if (read_file(persistent, contents, sizeof(contents)) < 0)
        goto error_mount;

    if (strcmp(contents, "persistent-data") != 0)
        goto error_mount;

    unlink(config);
    remove_tree(rootfs);
    remove_tree(mount_dir);

    pass("configured writable mount persists");
    return 0;

error_mount:
    remove_tree(mount_dir);
error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_configured_readonly_mount(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];
    char mount_dir[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (make_temp_dir(TEST_MOUNT, mount_dir, sizeof(mount_dir)) < 0)
        goto error_rootfs;

    char source_file[PATH_MAX];

    if (snprintf(source_file, sizeof(source_file), "%s/original", mount_dir) >=
        ( int )sizeof(source_file))
        goto error_mount;

    if (write_file(source_file, "original") < 0)
        goto error_mount;

    FILE *fp = fopen(config, "w");

    if (!fp)
        goto error_mount;

    fprintf(fp, "rootfs = \"%s\"\n\n", rootfs);
    fprintf(fp, "[[mounts]]\n");
    fprintf(fp, "source = \"%s\"\n", mount_dir);
    fprintf(fp, "target = \"/data\"\n");
    fprintf(fp, "readonly = true\n");

    if (fclose(fp) != 0)
        goto error_mount;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"write-file",
            ( char * )"/data/should-fail",
            ( char * )"must-not-write",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (file_exists(source_file)) {
        char contents[128];

        if (read_file(source_file, contents, sizeof(contents)) == 0 &&
            strcmp(contents, "original") == 0) {
            remove_tree(mount_dir);

            if (status != 0) {
                pass("configured read-only mount rejects writes");
                return 0;
            }
        }
    }

    remove_tree(mount_dir);
    return -1;

error_mount:
    remove_tree(mount_dir);
error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_configured_mount_failure_cleanup(void)
{
    char config[] = "/tmp/cage-test-config-XXXXXX";
    int  fd       = mkstemp(config);

    if (fd < 0)
        return -1;

    close(fd);

    char rootfs[PATH_MAX];
    char runtime[PATH_MAX];

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        goto error_config;

    if (make_temp_dir(TEST_RUNTIME, runtime, sizeof(runtime)) < 0)
        goto error_rootfs;

    FILE *fp = fopen(config, "w");

    if (!fp)
        goto error_runtime;

    fprintf(fp, "rootfs = \"%s\"\n\n", rootfs);
    fprintf(fp, "[[mounts]]\n");
    fprintf(fp, "source = \"%s/missing-mount-source\"\n", runtime);
    fprintf(fp, "target = \"/data\"\n");

    if (fclose(fp) != 0)
        goto error_runtime;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            "0",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status == 0) {
        remove_tree(runtime);
        return -1;
    }

    remove_tree(runtime);

    pass("configured mount failure cleans up");
    return 0;

error_runtime:
    remove_tree(runtime);
error_rootfs:
    remove_tree(rootfs);
error_config:
    unlink(config);
    return -1;
}

static int test_default_config(void)
{
    char cwd[PATH_MAX];
    char rootfs[PATH_MAX];
    char config[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        return -1;

    if (create_rootfs(rootfs, sizeof(rootfs)) < 0)
        return -1;

    if (snprintf(config, sizeof(config), "%s/cage.toml", cwd) >=
        ( int )sizeof(config)) {
        remove_tree(rootfs);
        return -1;
    }

    bool had_config = file_exists(config);

    char backup[PATH_MAX];
    backup[0] = '\0';

    if (had_config) {
        if (snprintf(backup, sizeof(backup), "%s/cage.toml.cage-test-backup",
                     cwd) >= ( int )sizeof(backup)) {
            remove_tree(rootfs);
            return -1;
        }

        unlink(backup);

        if (rename(config, backup) < 0) {
            remove_tree(rootfs);
            return -1;
        }
    }

    if (create_config(config, rootfs, NULL) < 0)
        goto error;

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"23",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);

    if (had_config)
        rename(backup, config);

    remove_tree(rootfs);

    if (status != 23)
        return -1;

    pass("default configuration is loaded");
    return 0;

error:
    unlink(config);

    if (had_config)
        rename(backup, config);

    remove_tree(rootfs);
    return -1;
}

static int test_missing_config_argument(void)
{
    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            NULL,
    };

    int status = run_process(argv);

    if (status == 0)
        return -1;

    pass("missing --config argument is rejected");
    return 0;
}

static int test_duplicate_config_argument(void)
{
    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            ( char * )"/tmp/cage-config-one",
            ( char * )"--config",
            ( char * )"/tmp/cage-config-two",
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"0",
            NULL,
    };

    int status = run_process(argv);

    if (status == 0)
        return -1;

    pass("duplicate --config is rejected");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cage> <container_probe>\n", argv[0]);
        return 2;
    }

    cage_path  = argv[1];
    probe_path = argv[2];

    if (test_exit_status() < 0)
        failures++;

    if (test_signal(SIGTERM, "SIGTERM") < 0)
        failures++;

    if (test_signal(SIGINT, "SIGINT") < 0)
        failures++;

    if (test_signal(SIGHUP, "SIGHUP") < 0)
        failures++;

    if (test_signal(SIGQUIT, "SIGQUIT") < 0)
        failures++;

    if (test_namespace("pid", "PID namespace isolates process IDs") < 0)
        failures++;

    if (test_namespace("user", "user namespace isolates user identity") < 0)
        failures++;

    if (test_uid_mapping() < 0)
        failures++;

    if (test_namespace("mnt", "mount namespace isolates mounts") < 0)
        failures++;

    if (test_tmpfs() < 0)
        failures++;

    if (test_proc() < 0)
        failures++;

    if (test_dev() < 0)
        failures++;

    if (test_namespace("net", "network namespace isolates networking") < 0)
        failures++;

    if (test_namespace("ipc", "IPC namespace isolates IPC objects") < 0)
        failures++;

    if (test_capabilities() < 0)
        failures++;

    if (test_no_new_privs() < 0)
        failures++;

    if (test_fd_inheritance() < 0)
        failures++;

    if (test_exec_failure() < 0)
        failures++;

    if (test_invalid_rootfs() < 0)
        failures++;

    if (test_parent_death() < 0)
        failures++;

    if (test_configured_writable_mount() < 0)
        failures++;

    if (test_configured_readonly_mount() < 0)
        failures++;

    if (test_configured_mount_failure_cleanup() < 0)
        failures++;

    if (test_default_config() < 0)
        failures++;

    if (test_missing_config_argument() < 0)
        failures++;

    if (test_duplicate_config_argument() < 0)
        failures++;

    printf("\n28 tests, %d failures\n", failures);

    return failures ? 1 : 0;
}
