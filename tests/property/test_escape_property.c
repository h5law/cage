#include <theft.h>

#include "../escape/escape_utils.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_ROOTFS       "/tmp/cage-fuzz-rootfs-XXXXXX"
#define TEST_CONFIG       "/tmp/cage-fuzz-config-XXXXXX"
#define TEST_HOST         "/tmp/cage-fuzz-host-XXXXXX"

#define FUZZ_TRIALS       1000
#define ATTACK_TIMEOUT_MS 3000

#define ARRAY_SIZE(x)     (sizeof(x) / sizeof((x)[0]))

static const char *cage_path;
static const char *probe_path;

static const char canary_contents[] = "CAGE_ESCAPE_FUZZ_CANARY_DO_NOT_TOUCH\n";

static const char *const path_prefixes[] = {
        "/tmp",          "//tmp",        "/./tmp",   "/tmp/.",
        "/tmp/..",       "/tmp//",       "/tmp/./.", "/tmp/../tmp",
        "/tmp/./../tmp", "/tmp//../tmp", "///tmp",   "////tmp",
};

static const char *const traversals[] = {
        "",        "/.",      "/..",       "/./..",    "/../.",
        "//.././", "/../../", "/./../../", "//../../",
};

static const char *const path_components[] = {
        "",
        ".",
        "..",
        "/",
        "//",
        "///",
        "tmp",
        "cage-fuzz-host",
        "cage-fuzz-host/",
        "./tmp",
        "../tmp",
        "...",
        "....",
        "tmp/.",
        "tmp/..",
        "tmp//",
};

static const char *const namespace_names[] = {
        "pid", "mnt", "net", "ipc", "uts", "user",
};

static const char *const proc_paths[] = {
        "/proc/1/status",
        "/proc/1/root",
        "/proc/1/cwd",
        "/proc/1/exe",
        "/proc/self/root",
        "/proc/self/cwd",
        "/proc/self/exe",
        "/proc/thread-self/root",
        "/proc/thread-self/cwd",
        "/proc/thread-self/exe",
        "/proc/sys",
        "/proc/kcore",
};

static const char *const sys_paths[] = {
        "/sys", "/sys/kernel", "/sys/fs", "/sys/class", "/sys/devices",
};

enum escape_operation {
    ESCAPE_PROC_ROOT,
    ESCAPE_OPENAT,
    ESCAPE_PROC_ROOT_TRAVERSAL,
    ESCAPE_SELF_ROOT,
    ESCAPE_THREAD_ROOT,
    ESCAPE_SYMLINK,
    ESCAPE_RELATIVE,

    ESCAPE_DOUBLE_SLASH,
    ESCAPE_DOT_COMPONENTS,
    ESCAPE_PARENT_COMPONENTS,
    ESCAPE_SYMLINK_PARENT,

    ESCAPE_PROC_MAGICLINK,
    ESCAPE_PROC_CWD,
    ESCAPE_PROC_FD,

    ESCAPE_MOUNT,
    ESCAPE_UMOUNT,
    ESCAPE_PIVOT_ROOT,
    ESCAPE_MOVE_MOUNT,
    ESCAPE_OPEN_TREE,
    ESCAPE_MOUNT_SETATTR,
    ESCAPE_CHROOT,

    ESCAPE_NAMESPACE,
    ESCAPE_SETNS,
    ESCAPE_UNSHARE,
    ESCAPE_CLONE,

    ESCAPE_MKNOD,
    ESCAPE_RAW_SOCKET,
    ESCAPE_DEV_MEM,
    ESCAPE_DEV_KMEM,
    ESCAPE_DEV_KMSG,

    ESCAPE_CAPSET,
    ESCAPE_PTRACE,
    ESCAPE_KILL,

    ESCAPE_REBOOT,
    ESCAPE_KEXEC,
    ESCAPE_SYS_MODULE,

    ESCAPE_PROCFS,
    ESCAPE_DEVFS,
    ESCAPE_SYSFS,

    ESCAPE_PROC_PATH,
    ESCAPE_SYS_PATH,

    ESCAPE_HOST_PID,
    ESCAPE_HOST_PID_ROOT,
};

static const char *const operation_names[] = {
        "proc-root",
        "proc-root-openat",
        "proc-root-traversal",
        "self-root",
        "thread-root",
        "symlink-escape",
        "relative-escape",

        "double-slash",
        "dot-components",
        "parent-components",
        "symlink-parent",

        "proc-magiclink",
        "proc-cwd",
        "proc-fd",

        "mount",
        "umount",
        "pivot-root",
        "move-mount",
        "open-tree",
        "mount-setattr",
        "chroot",

        "namespace",
        "setns",
        "unshare",
        "clone",

        "mknod",
        "raw-socket",
        "dev-mem",
        "dev-kmem",
        "dev-kmsg",

        "capset",
        "ptrace",
        "kill",

        "reboot",
        "kexec",
        "sys-module",

        "procfs",
        "devfs",
        "sysfs",

        "proc-path",
        "sys-path",

        "host-pid",
        "host-pid-root",
};

enum {
    ESCAPE_OPERATION_COUNT = ARRAY_SIZE(operation_names),
};

static unsigned long operation_counts[ESCAPE_OPERATION_COUNT];

struct escape_case {
    size_t operation;

    size_t prefix;
    size_t traversal;

    size_t component_a;
    size_t component_b;

    size_t namespace_name;

    size_t proc_path;
    size_t sys_path;
};

static enum theft_alloc_res escape_case_alloc(struct theft *t, void *env,
                                              void **output)
{
    struct escape_case *test;

    ( void )env;

    test = calloc(1, sizeof(*test));

    if (test == NULL)
        return THEFT_ALLOC_ERROR;

    test->operation      = theft_random_choice(t, ESCAPE_OPERATION_COUNT);

    test->prefix         = theft_random_choice(t, ARRAY_SIZE(path_prefixes));

    test->traversal      = theft_random_choice(t, ARRAY_SIZE(traversals));

    test->component_a    = theft_random_choice(t, ARRAY_SIZE(path_components));

    test->component_b    = theft_random_choice(t, ARRAY_SIZE(path_components));

    test->namespace_name = theft_random_choice(t, ARRAY_SIZE(namespace_names));

    test->proc_path      = theft_random_choice(t, ARRAY_SIZE(proc_paths));

    test->sys_path       = theft_random_choice(t, ARRAY_SIZE(sys_paths));

    *output              = test;

    return THEFT_ALLOC_OK;
}

static void escape_case_free(void *instance, void *env)
{
    ( void )env;

    free(instance);
}

static void escape_case_print(FILE *f, const void *instance, void *env)
{
    const struct escape_case *test = instance;

    ( void )env;

    fprintf(f,
            "{operation=\"%s\", "
            "prefix=\"%s\", "
            "traversal=\"%s\", "
            "component_a=\"%s\", "
            "component_b=\"%s\", "
            "namespace=\"%s\", "
            "proc=\"%s\", "
            "sys=\"%s\"}",
            operation_names[test->operation], path_prefixes[test->prefix],
            traversals[test->traversal], path_components[test->component_a],
            path_components[test->component_b],
            namespace_names[test->namespace_name], proc_paths[test->proc_path],
            sys_paths[test->sys_path]);
}

static struct theft_type_info escape_case_type = {
        .alloc = escape_case_alloc,
        .free  = escape_case_free,
        .print = escape_case_print,
};

static int build_attack_path(const struct escape_case *test, const char *host,
                             char *path, size_t size)
{
    const char *basename;
    int         written;

    basename = strrchr(host, '/');

    if (basename == NULL)
        basename = host;
    else
        ++basename;

    written = snprintf(path, size, "%s/%s/%s/%s%s", path_prefixes[test->prefix],
                       path_components[test->component_a],
                       path_components[test->component_b], basename,
                       traversals[test->traversal]);

    if (written < 0 || ( size_t )written >= size)
        return -1;

    return 0;
}

static int create_test_environment(char *rootfs, size_t rootfs_size,
                                   char *config, size_t config_size, char *host,
                                   size_t host_size)
{
    char rootfs_template[] = TEST_ROOTFS;
    char config_template[] = TEST_CONFIG;
    char host_template[]   = TEST_HOST;

    if (escape_make_temp_file(config_template, config, config_size) == -1) {
        return -1;
    }

    if (escape_make_temp_file(host_template, host, host_size) == -1) {
        unlink(config);
        return -1;
    }

    if (escape_write_file(host, canary_contents) == -1) {
        unlink(host);
        unlink(config);
        return -1;
    }

    if (escape_create_rootfs(probe_path, rootfs_template, rootfs,
                             rootfs_size) == -1) {
        unlink(host);
        unlink(config);
        return -1;
    }

    if (escape_create_config(config, rootfs, NULL) == -1) {
        unlink(host);
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    return 0;
}

static int
build_probe_argv(const struct escape_case *test, const char *attack_path,
                 const char *host_pid_string, unsigned long namespace_dev,
                 unsigned long namespace_ino, char *dev_string, size_t dev_size,
                 char *ino_string, size_t ino_size, char *namespace_path,
                 size_t namespace_path_size, char *argv[10])
{
    const char *namespace_name;

    namespace_name = namespace_names[test->namespace_name];

    for (size_t i = 0; i < 10; ++i)
        argv[i] = NULL;

    argv[0] = ( char * )"/bin/escape-probe";

    switch (test->operation) {
    case ESCAPE_PROC_ROOT:
        argv[1] = ( char * )"proc-root";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_OPENAT:
        argv[1] = ( char * )"proc-root-openat";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_PROC_ROOT_TRAVERSAL:
        argv[1] = ( char * )"proc-root-traversal";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_SELF_ROOT:
        argv[1] = ( char * )"self-root";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_THREAD_ROOT:
        argv[1] = ( char * )"thread-root";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_SYMLINK:
        argv[1] = ( char * )"symlink-escape";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_RELATIVE:
        argv[1] = ( char * )"relative-escape";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_DOUBLE_SLASH:
        argv[1] = ( char * )"path-resolution";
        argv[2] = ( char * )"double-slash";
        argv[3] = ( char * )attack_path;
        argv[4] = ( char * )canary_contents;
        break;

    case ESCAPE_DOT_COMPONENTS:
        argv[1] = ( char * )"path-resolution";
        argv[2] = ( char * )"dot";
        argv[3] = ( char * )attack_path;
        argv[4] = ( char * )canary_contents;
        break;

    case ESCAPE_PARENT_COMPONENTS:
        argv[1] = ( char * )"path-resolution";
        argv[2] = ( char * )"parent";
        argv[3] = ( char * )attack_path;
        argv[4] = ( char * )canary_contents;
        break;

    case ESCAPE_SYMLINK_PARENT:
        argv[1] = ( char * )"path-resolution";
        argv[2] = ( char * )"symlink";
        argv[3] = ( char * )attack_path;
        argv[4] = ( char * )canary_contents;
        break;

    case ESCAPE_PROC_MAGICLINK:
        argv[1] = ( char * )"proc-magiclink";
        argv[2] = ( char * )proc_paths[test->proc_path];
        argv[3] = ( char * )canary_contents;
        break;

    case ESCAPE_PROC_CWD:
        argv[1] = ( char * )"proc-cwd";
        argv[2] = ( char * )canary_contents;
        break;

    case ESCAPE_PROC_FD:
        argv[1] = ( char * )"proc-fd";
        argv[2] = ( char * )canary_contents;
        break;

    case ESCAPE_MOUNT:
        argv[1] = ( char * )"mount";
        break;

    case ESCAPE_UMOUNT:
        argv[1] = ( char * )"umount";
        break;

    case ESCAPE_PIVOT_ROOT:
        argv[1] = ( char * )"pivot-root";
        break;

    case ESCAPE_MOVE_MOUNT:
        argv[1] = ( char * )"move-mount";
        break;

    case ESCAPE_OPEN_TREE:
        argv[1] = ( char * )"open-tree";
        break;

    case ESCAPE_MOUNT_SETATTR:
        argv[1] = ( char * )"mount-setattr";
        break;

    case ESCAPE_CHROOT:
        argv[1] = ( char * )"chroot";
        break;

    case ESCAPE_NAMESPACE:
        if (snprintf(dev_string, dev_size, "%lu", namespace_dev) < 0)
            return -1;

        if (snprintf(ino_string, ino_size, "%lu", namespace_ino) < 0)
            return -1;

        argv[1] = ( char * )"namespace";
        argv[2] = ( char * )namespace_name;
        argv[3] = dev_string;
        argv[4] = ino_string;
        break;

    case ESCAPE_SETNS:
        if (snprintf(namespace_path, namespace_path_size, "/proc/1/ns/%s",
                     namespace_name) < 0)
            return -1;

        argv[1] = ( char * )"setns";
        argv[2] = ( char * )namespace_name;
        argv[3] = namespace_path;
        break;

    case ESCAPE_UNSHARE:
        argv[1] = ( char * )"unshare";
        argv[2] = ( char * )namespace_name;
        break;

    case ESCAPE_CLONE:
        argv[1] = ( char * )"clone";
        argv[2] = ( char * )namespace_name;
        break;

    case ESCAPE_MKNOD:
        argv[1] = ( char * )"mknod";
        break;

    case ESCAPE_RAW_SOCKET:
        argv[1] = ( char * )"raw-socket";
        break;

    case ESCAPE_DEV_MEM:
        argv[1] = ( char * )"device";
        argv[2] = ( char * )"/dev/mem";
        break;

    case ESCAPE_DEV_KMEM:
        argv[1] = ( char * )"device";
        argv[2] = ( char * )"/dev/kmem";
        break;

    case ESCAPE_DEV_KMSG:
        argv[1] = ( char * )"device";
        argv[2] = ( char * )"/dev/kmsg";
        break;

    case ESCAPE_CAPSET:
        argv[1] = ( char * )"capset";
        break;

    case ESCAPE_PTRACE:
        argv[1] = ( char * )"ptrace";
        break;

    case ESCAPE_KILL:
        argv[1] = ( char * )"kill";
        argv[2] = ( char * )host_pid_string;
        break;

    case ESCAPE_REBOOT:
        argv[1] = ( char * )"reboot";
        break;

    case ESCAPE_KEXEC:
        argv[1] = ( char * )"kexec";
        break;

    case ESCAPE_SYS_MODULE:
        argv[1] = ( char * )"sys-module";
        break;

    case ESCAPE_PROCFS:
        argv[1] = ( char * )"procfs-private";
        break;

    case ESCAPE_DEVFS:
        argv[1] = ( char * )"devfs-private";
        break;

    case ESCAPE_SYSFS:
        argv[1] = ( char * )"sysfs-private";
        break;

    case ESCAPE_PROC_PATH:
        argv[1] = ( char * )"proc-path";
        argv[2] = ( char * )proc_paths[test->proc_path];
        break;

    case ESCAPE_SYS_PATH:
        argv[1] = ( char * )"sys-path";
        argv[2] = ( char * )sys_paths[test->sys_path];
        break;

    case ESCAPE_HOST_PID:
        argv[1] = ( char * )"host-pid";
        break;

    case ESCAPE_HOST_PID_ROOT:
        argv[1] = ( char * )"host-pid-root";
        argv[2] = ( char * )attack_path;
        argv[3] = ( char * )canary_contents;
        break;

    default:
        return -1;
    }

    return 0;
}

static int run_process_timeout(char *const argv[], unsigned long timeout_ms)
{
    pid_t         pid;
    unsigned long elapsed = 0;

    pid                   = fork();

    if (pid == -1)
        return -1;

    if (pid == 0) {
        ( void )setpgid(0, 0);
        execv(argv[0], argv);
        _exit(127);
    }

    ( void )setpgid(pid, pid);

    while (elapsed < timeout_ms) {
        int   status;
        pid_t result;

        result = waitpid(pid, &status, WNOHANG);

        if (result == pid) {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);

            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);

            return -1;
        }

        if (result == -1) {
            if (errno == EINTR)
                continue;

            ( void )kill(-pid, SIGKILL);
            ( void )waitpid(pid, NULL, 0);
            return -1;
        }

        {
            struct timespec delay = {
                    .tv_sec  = 0,
                    .tv_nsec = 10000000L,
            };

            while (nanosleep(&delay, &delay) == -1 && errno == EINTR)
                ;
        }

        elapsed += 10;
    }

    fprintf(stderr, "\nprobe timed out after %lums: %s\n", timeout_ms,
            argv[3] != NULL ? argv[3] : "<unknown>");

    ( void )kill(-pid, SIGKILL);

    while (waitpid(pid, NULL, 0) == -1) {
        if (errno != EINTR)
            break;
    }

    return -1;
}

static int run_attack(const struct escape_case *test)
{
    char rootfs[PATH_MAX];
    char config[PATH_MAX];
    char host[PATH_MAX];
    char attack_path[PATH_MAX];

    char dev_string[32];
    char ino_string[32];
    char host_pid_string[32];
    char namespace_path[PATH_MAX];

    char *probe_argv[10];
    char *cage_argv[20];

    unsigned long namespace_dev = 0;
    unsigned long namespace_ino = 0;

    if (snprintf(host_pid_string, sizeof(host_pid_string), "%ld",
                 ( long )getpid()) >= ( int )sizeof(host_pid_string)) {
        unlink(host);
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    size_t argc = 0;
    int    status;

    if (create_test_environment(rootfs, sizeof(rootfs), config, sizeof(config),
                                host, sizeof(host)) == -1)
        return -1;

    if (build_attack_path(test, host, attack_path, sizeof(attack_path)) == -1) {
        unlink(host);
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    if (test->operation == ESCAPE_NAMESPACE) {
        if (escape_namespace_identity(namespace_names[test->namespace_name],
                                      &namespace_dev, &namespace_ino) == -1) {
            unlink(host);
            unlink(config);
            escape_remove_tree(rootfs);
            return -1;
        }
    }

    if (build_probe_argv(test, attack_path, host_pid_string, namespace_dev,
                         namespace_ino, dev_string, sizeof(dev_string),
                         ino_string, sizeof(ino_string), namespace_path,
                         sizeof(namespace_path), probe_argv) == -1) {
        unlink(host);
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    cage_argv[argc++] = ( char * )cage_path;
    cage_argv[argc++] = ( char * )"--config";
    cage_argv[argc++] = config;

    for (size_t i = 0; i < ARRAY_SIZE(probe_argv) && probe_argv[i] != NULL;
         ++i) {
        if (argc + 1 >= ARRAY_SIZE(cage_argv)) {
            unlink(host);
            unlink(config);
            escape_remove_tree(rootfs);
            return -1;
        }

        cage_argv[argc++] = probe_argv[i];
    }

    cage_argv[argc] = NULL;

    status          = run_process_timeout(cage_argv, ATTACK_TIMEOUT_MS);

    if (status == -1) {
        fprintf(stderr, "operation timed out: %s\n",
                operation_names[test->operation]);
    }

    if (status == 0) {
        char contents[sizeof(canary_contents)];

        if (escape_read_file(host, contents, sizeof(contents)) == -1) {
            unlink(host);
            unlink(config);
            escape_remove_tree(rootfs);
            return -1;
        }

        if (strcmp(contents, canary_contents) != 0) {
            fprintf(stderr, "host canary changed despite "
                            "successful containment\n");

            status = 1;
        }
    }

    unlink(host);
    unlink(config);
    escape_remove_tree(rootfs);

    return status;
}

static void print_escape_coverage(void)
{
    printf("\nEscape surface coverage:\n");

    for (size_t i = 0; i < ESCAPE_OPERATION_COUNT; ++i) {
        printf("  %-24s %lu\n", operation_names[i], operation_counts[i]);
    }
}

static enum theft_trial_res prop_no_escape(struct theft *t, void *input)
{
    const struct escape_case *test = input;
    int                       status;

    ( void )t;

    if (test->operation < ESCAPE_OPERATION_COUNT)
        operation_counts[test->operation]++;

    status = run_attack(test);

    if (status == -1)
        return THEFT_TRIAL_ERROR;

    if (status == 1) {
        fprintf(stderr, "\nESCAPE PROPERTY FAILED:\n");

        escape_case_print(stderr, test, NULL);
        fputc('\n', stderr);

        return THEFT_TRIAL_FAIL;
    }

    if (status != 0)
        return THEFT_TRIAL_ERROR;

    return THEFT_TRIAL_PASS;
}

int main(int argc, char **argv)
{
    struct theft_run_config config;
    enum theft_run_res      result;
    unsigned long           trials = FUZZ_TRIALS;

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <cage> <escape-probe> [trials]\n", argv[0]);

        return EXIT_FAILURE;
    }

    cage_path  = argv[1];
    probe_path = argv[2];

    if (argc == 4) {
        char *end;

        errno  = 0;

        trials = strtoul(argv[3], &end, 10);

        if (errno != 0 || *end != '\0' || trials == 0) {
            fprintf(stderr, "invalid trial count: %s\n", argv[3]);

            return EXIT_FAILURE;
        }
    }

    memset(operation_counts, 0, sizeof(operation_counts));

    config = (struct theft_run_config){
            .name  = "generated container escape surfaces remain contained",

            .prop1 = prop_no_escape,

            .type_info =
                    {
                                &escape_case_type,
                                },

            .seed   = theft_seed_of_time(),
            .trials = trials,
    };

    result = theft_run(&config);

    print_escape_coverage();

    return result == THEFT_RUN_PASS ? EXIT_SUCCESS : EXIT_FAILURE;
}
