#include "escape_utils.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_ROOTFS "/tmp/cage-escape-rootfs-XXXXXX"
#define TEST_CONFIG "/tmp/cage-escape-config-XXXXXX"
#define TEST_HOST   "/tmp/cage-escape-host-XXXXXX"

static const char *cage_path;
static const char *probe_path;

static const char *const canary_contents = "CAGE_ESCAPE_CANARY_DO_NOT_TOUCH\n";

static int run_probe_with_config(char *const probe_argv[],
                                 const char *extra_config)
{
    char   rootfs[PATH_MAX];
    char   config[PATH_MAX];
    char  *argv[32];
    size_t argc       = 0;
    size_t probe_argc = 0;
    int    status;

    if (escape_make_temp_file(TEST_CONFIG, config, sizeof(config)) == -1)
        return -1;

    if (escape_create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) ==
        -1) {
        unlink(config);
        return -1;
    }

    if (escape_create_config(config, rootfs, extra_config) == -1) {
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    argv[argc++] = ( char * )cage_path;
    argv[argc++] = ( char * )"--config";
    argv[argc++] = config;

    while (probe_argv[probe_argc] != NULL) {
        if (argc + 1 >= sizeof(argv) / sizeof(argv[0])) {
            unlink(config);
            escape_remove_tree(rootfs);
            return -1;
        }

        argv[argc++] = probe_argv[probe_argc++];
    }

    argv[argc] = NULL;

    status     = escape_run_process(argv);

    unlink(config);

    if (escape_remove_tree(rootfs) == -1 && status == 0)
        status = -1;

    return status;
}

static int run_probe(char *const probe_argv[])
{
    return run_probe_with_config(probe_argv, NULL);
}

static int run_bind_probe(char *const probe_argv[], const char *source,
                          const char *target, int readonly)
{
    char extra[PATH_MAX * 2];
    int  written;

    written = snprintf(extra, sizeof(extra),
                       "\n[[mounts]]\n"
                       "source = \"%s\"\n"
                       "target = \"%s\"\n"
                       "readonly = %s\n",
                       source, target, readonly ? "true" : "false");

    if (written < 0 || ( size_t )written >= sizeof(extra)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return run_probe_with_config(probe_argv, extra);
}

static int prepare_host_canary(char *path, size_t size)
{
    if (escape_make_temp_file(TEST_HOST, path, size) == -1)
        return -1;

    if (escape_write_file(path, canary_contents) == -1) {
        unlink(path);
        return -1;
    }

    return 0;
}

static int test_probe_execution(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"procfs-private",
            NULL,
    };
    int status;

    escape_test_begin("escape probe executes inside container");

    status = run_probe(argv);

    if (status == 127) {
        escape_test_fail(
                "cage could not exec /bin/escape-probe; check static linkage");
        return -1;
    }

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("escape probe did not execute successfully");

    return -1;
}

static int test_proc_root(void)
{
    char  canary[PATH_MAX];
    char *argv[5];
    int   status;

    escape_test_begin("/proc/1/root cannot reach host filesystem");

    if (prepare_host_canary(canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"proc-root";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("/proc/1/root exposed the host filesystem");

    return -1;
}

static int test_proc_root_openat(void)
{
    char  canary[PATH_MAX];
    char *argv[5];
    int   status;

    escape_test_begin("openat through /proc/1/root cannot reach host");

    if (prepare_host_canary(canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"openat-traversal";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("openat reached the host filesystem");

    return -1;
}

static int test_proc_root_traversal(void)
{
    char  canary[PATH_MAX];
    char *argv[5];
    int   status;

    escape_test_begin("path traversal through /proc/1/root stays contained");

    if (prepare_host_canary(canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"proc-root-traversal";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("path traversal reached the host filesystem");

    return -1;
}

static int test_namespace(const char *name)
{
    unsigned long dev;
    unsigned long ino;
    char          dev_string[32];
    char          ino_string[32];
    char          description[128];
    char         *argv[6];
    int           status;

    if (snprintf(description, sizeof(description),
                 "container %s namespace differs from host",
                 name) >= ( int )sizeof(description)) {
        return -1;
    }

    escape_test_begin(description);

    if (escape_namespace_identity(name, &dev, &ino) == -1) {
        escape_test_fail("failed to inspect host namespace");
        return -1;
    }

    snprintf(dev_string, sizeof(dev_string), "%lu", dev);
    snprintf(ino_string, sizeof(ino_string), "%lu", ino);

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"namespace";
    argv[2] = ( char * )name;
    argv[3] = dev_string;
    argv[4] = ino_string;
    argv[5] = NULL;

    status  = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container shares host namespace");

    return -1;
}

static int test_procfs(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"procfs-private",
            NULL,
    };
    int status;

    escape_test_begin("container /proc is private procfs");

    status = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container /proc is not private procfs");

    return -1;
}

static int test_devfs(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"devfs-private",
            NULL,
    };
    int status;

    escape_test_begin("container /dev is private tmpfs");

    status = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container /dev is not private tmpfs");

    return -1;
}

static int test_mount_denied(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"mount",
            NULL,
    };
    int status;

    escape_test_begin("container cannot mount");

    status = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container successfully mounted a filesystem");

    return -1;
}

static int test_mknod_denied(void)
{
    char  path[] = "/tmp/cage-escape-device";
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"mknod",
            path,
            NULL,
    };
    int status;

    escape_test_begin("container cannot mknod");

    unlink(path);

    status = run_probe(argv);

    unlink(path);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container successfully created a device node");

    return -1;
}

static int test_raw_socket_denied(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"raw-socket",
            NULL,
    };
    int status;

    escape_test_begin("container cannot create raw sockets");

    status = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container successfully created a raw socket");

    return -1;
}

static int test_setns_denied(const char *name)
{
    char  namespace_path[PATH_MAX];
    char  description[128];
    char *argv[5];
    int   status;

    if (snprintf(description, sizeof(description),
                 "container cannot setns into host %s namespace",
                 name) >= ( int )sizeof(description)) {
        return -1;
    }

    escape_test_begin(description);

    if (snprintf(namespace_path, sizeof(namespace_path), "/proc/1/ns/%s",
                 name) >= ( int )sizeof(namespace_path)) {
        escape_test_fail("namespace path is too long");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"setns";
    argv[2] = ( char * )name;
    argv[3] = namespace_path;
    argv[4] = NULL;

    status  = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container successfully entered a host namespace");

    return -1;
}

static int test_chroot_denied(void)
{
    char *argv[] = {
            ( char * )"/bin/escape-probe",
            ( char * )"chroot",
            NULL,
    };
    int status;

    escape_test_begin("container cannot chroot");

    status = run_probe(argv);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("container successfully used chroot");

    return -1;
}

static int prepare_bind_fixture(char *host_root, size_t host_root_size,
                                char *allowed, size_t allowed_size,
                                char *sibling, size_t sibling_size)
{
    char allowed_file[PATH_MAX];
    char sibling_file[PATH_MAX];

    if (escape_make_temp_dir(TEST_HOST, host_root, host_root_size) == -1)
        return -1;

    if (snprintf(allowed, allowed_size, "%s/allowed", host_root) >=
        ( int )allowed_size)
        goto error;

    if (snprintf(sibling, sibling_size, "%s/sibling", host_root) >=
        ( int )sibling_size)
        goto error;

    if (mkdir(allowed, 0755) == -1)
        goto error;

    if (mkdir(sibling, 0755) == -1)
        goto error;

    if (snprintf(allowed_file, sizeof(allowed_file), "%s/canary", allowed) >=
        ( int )sizeof(allowed_file))
        goto error;

    if (snprintf(sibling_file, sizeof(sibling_file), "%s/canary", sibling) >=
        ( int )sizeof(sibling_file))
        goto error;

    if (escape_write_file(allowed_file, canary_contents) == -1)
        goto error;

    if (escape_write_file(sibling_file, canary_contents) == -1)
        goto error;

    return 0;

error: {
    int saved_errno = errno;

    escape_remove_tree(host_root);
    errno = saved_errno;
}

    return -1;
}

static int test_configured_bind_mount(void)
{
    char  host_root[PATH_MAX];
    char  allowed[PATH_MAX];
    char  sibling[PATH_MAX];
    char *argv[4];
    int   status;

    escape_test_begin("configured bind mount is accessible");

    if (prepare_bind_fixture(host_root, sizeof(host_root), allowed,
                             sizeof(allowed), sibling, sizeof(sibling)) == -1) {
        escape_test_fail("failed to create bind fixture");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"read-file";
    argv[2] = ( char * )"/mnt/allowed/canary";
    argv[3] = NULL;

    status  = run_bind_probe(argv, allowed, "/mnt/allowed", 1);

    escape_remove_tree(host_root);

    if (status == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("configured bind mount was not accessible");

    return -1;
}

static int test_configured_bind_sibling(void)
{
    char  host_root[PATH_MAX];
    char  allowed[PATH_MAX];
    char  sibling[PATH_MAX];
    char *argv[4];
    int   status;

    escape_test_begin("configured bind mount does not expose sibling");

    if (prepare_bind_fixture(host_root, sizeof(host_root), allowed,
                             sizeof(allowed), sibling, sizeof(sibling)) == -1) {
        escape_test_fail("failed to create bind fixture");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"read-file";
    argv[2] = ( char * )"/mnt/allowed/../sibling/canary";
    argv[3] = NULL;

    status  = run_bind_probe(argv, allowed, "/mnt/allowed", 1);

    escape_remove_tree(host_root);

    if (status == 1) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("configured bind mount exposed sibling data");

    return -1;
}

static int test_configured_bind_readonly(void)
{
    char  host_root[PATH_MAX];
    char  allowed[PATH_MAX];
    char  sibling[PATH_MAX];
    char  canary[PATH_MAX];
    char  contents[128];
    char *argv[5];
    int   status;

    escape_test_begin("configured read-only bind cannot be written");

    if (prepare_bind_fixture(host_root, sizeof(host_root), allowed,
                             sizeof(allowed), sibling, sizeof(sibling)) == -1) {
        escape_test_fail("failed to create bind fixture");
        return -1;
    }

    if (snprintf(canary, sizeof(canary), "%s/canary", allowed) >=
        ( int )sizeof(canary)) {
        escape_remove_tree(host_root);
        escape_test_fail("bind canary path is too long");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"write-file";
    argv[2] = ( char * )"/mnt/allowed/canary";
    argv[3] = ( char * )"MUST_NOT_WRITE\n";
    argv[4] = NULL;

    status  = run_bind_probe(argv, allowed, "/mnt/allowed", 1);

    if (escape_read_file(canary, contents, sizeof(contents)) == -1) {
        escape_remove_tree(host_root);
        escape_test_fail("failed to read bind canary");
        return -1;
    }

    escape_remove_tree(host_root);

    /*
     * write-file returns:
     *
     *     0 -> write succeeded
     *     1 -> write failed
     *
     * The host-side source must remain untouched.
     */
    if (status == 1 && strcmp(contents, canary_contents) == 0) {
        escape_test_pass();
        return 0;
    }

    escape_test_fail("read-only bind was writable");

    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cage> <escape_probe>\n", argv[0]);
        return 2;
    }

    cage_path  = argv[1];
    probe_path = argv[2];

    test_probe_execution();

    test_proc_root();
    test_proc_root_openat();
    test_proc_root_traversal();

    test_namespace("pid");
    test_namespace("mnt");
    test_namespace("net");
    test_namespace("ipc");
    test_namespace("uts");
    test_namespace("user");

    test_procfs();
    test_devfs();

    test_mount_denied();
    test_mknod_denied();
    test_raw_socket_denied();

    test_setns_denied("mnt");
    test_setns_denied("pid");
    test_setns_denied("net");
    test_setns_denied("ipc");
    test_setns_denied("uts");
    test_setns_denied("user");

    test_chroot_denied();

    test_configured_bind_mount();
    test_configured_bind_sibling();
    test_configured_bind_readonly();

    return escape_test_run();
}
