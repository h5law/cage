#include "escape_utils.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_ROOTFS  "/tmp/cage-escape-rootfs-XXXXXX"
#define TEST_RUNTIME "/tmp/cage-escape-runtime-XXXXXX"
#define TEST_CANARY  "/tmp/cage-escape-canary-XXXXXX"

static const char *cage_path;
static const char *probe_path;

static const char *const canary_contents = "CAGE_ESCAPE_CANARY_DO_NOT_TOUCH\n";

static int prepare_rootfs_and_config(char *rootfs, size_t rootfs_size,
                                     char *config, size_t config_size)
{
    if (escape_make_temp_file("/tmp/cage-escape-config-XXXXXX", config,
                              config_size) == -1) {
        return -1;
    }

    if (escape_create_rootfs(probe_path, TEST_ROOTFS, rootfs, rootfs_size) ==
        -1) {
        unlink(config);
        return -1;
    }

    if (escape_create_config(config, rootfs, NULL) == -1) {
        unlink(config);
        escape_remove_tree(rootfs);
        return -1;
    }

    return 0;
}

static int run_probe(char *const probe_argv[])
{
    char   config[PATH_MAX];
    char   rootfs[PATH_MAX];
    char  *argv[32];
    size_t argc       = 0;
    size_t probe_argc = 0;

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) == -1) {
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

    {
        int status = escape_run_process(argv);

        unlink(config);
        escape_remove_tree(rootfs);

        return status;
    }
}

static int test_host_file_hidden(void)
{
    char  canary[PATH_MAX];
    char *argv[8];
    int   status;

    escape_test_begin("host-only file is unreachable");

    if (escape_make_temp_file(TEST_CANARY, canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    if (escape_write_file(canary, canary_contents) == -1) {
        unlink(canary);
        escape_test_fail("failed to initialise host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"host-read";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status != 0) {
        escape_test_fail("container was able to read the host canary");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_host_file_not_writable(void)
{
    char  canary[PATH_MAX];
    char *argv[8];
    int   status;
    char  contents[128];

    escape_test_begin("host-only file is not writable");

    if (escape_make_temp_file(TEST_CANARY, canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    if (escape_write_file(canary, canary_contents) == -1) {
        unlink(canary);
        escape_test_fail("failed to initialise host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"host-write";
    argv[2] = canary;
    argv[3] = NULL;

    status  = run_probe(argv);

    if (escape_read_file(canary, contents, sizeof(contents)) == -1) {
        unlink(canary);
        escape_test_fail("failed to read host canary");
        return -1;
    }

    unlink(canary);

    if (status != 0 || strcmp(contents, canary_contents) != 0) {
        escape_test_fail("container was able to modify the host canary");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_proc_1_root(void)
{
    char  canary[PATH_MAX];
    char *argv[8];
    int   status;

    escape_test_begin("/proc/1/root cannot reach host filesystem");

    if (escape_make_temp_file(TEST_CANARY, canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    if (escape_write_file(canary, canary_contents) == -1) {
        unlink(canary);
        escape_test_fail("failed to initialise host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"proc-1-root-read";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status != 0) {
        escape_test_fail("/proc/1/root exposed the host filesystem");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_proc_self_root(void)
{
    char  canary[PATH_MAX];
    char *argv[8];
    int   status;

    escape_test_begin("/proc/self/root cannot reach host filesystem");

    if (escape_make_temp_file(TEST_CANARY, canary, sizeof(canary)) == -1) {
        escape_test_fail("failed to create host canary");
        return -1;
    }

    if (escape_write_file(canary, canary_contents) == -1) {
        unlink(canary);
        escape_test_fail("failed to initialise host canary");
        return -1;
    }

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"proc-self-root-read";
    argv[2] = canary;
    argv[3] = ( char * )canary_contents;
    argv[4] = NULL;

    status  = run_probe(argv);

    unlink(canary);

    if (status != 0) {
        escape_test_fail("/proc/self/root exposed the host filesystem");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_namespace(const char *name)
{
    unsigned long dev;
    unsigned long ino;
    char          dev_string[32];
    char          ino_string[32];
    char         *argv[16];
    int           status;

    char description[128];

    if (snprintf(description, sizeof(description),
                 "%s namespace differs from host",
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

    if (status != 0) {
        escape_test_fail("container shares host namespace");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_procfs(void)
{
    char *argv[8];
    int   status;

    escape_test_begin("/proc is a private proc filesystem");

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"procfs";
    argv[2] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("/proc is not a proc filesystem");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_devfs(void)
{
    char *argv[8];
    int   status;

    escape_test_begin("/dev is a private tmpfs");

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"devfs";
    argv[2] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("/dev is not a private tmpfs");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_mount_denied(void)
{
    char *argv[8];
    int   status;

    escape_test_begin("container cannot mount filesystems");

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"mount";
    argv[2] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("container successfully mounted a filesystem");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_mknod_denied(void)
{
    char  path[PATH_MAX];
    char *argv[8];
    int   status;

    escape_test_begin("container cannot create device nodes");

    if (snprintf(path, sizeof(path), "/tmp/cage-escape-device-XXXXXX") >=
        ( int )sizeof(path)) {
        escape_test_fail("device path is too long");
        return -1;
    }

    /*
     * The probe needs a pathname that doesn't already exist.
     */
    unlink(path);

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"mknod";
    argv[2] = path;
    argv[3] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("container successfully created a device node");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_raw_socket_denied(void)
{
    char *argv[8];
    int   status;

    escape_test_begin("container cannot create raw sockets");

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"raw-socket";
    argv[2] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("container successfully created a raw socket");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_setns_denied(const char *name)
{
    char  namespace_path[PATH_MAX];
    char  description[128];
    char *argv[8];
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

    if (status != 0) {
        escape_test_fail("container successfully entered a host namespace");
        return -1;
    }

    escape_test_pass();

    return 0;
}

static int test_chroot_host_denied(void)
{
    char *argv[8];
    int   status;

    escape_test_begin("container cannot chroot into host root");

    argv[0] = ( char * )"/bin/escape-probe";
    argv[1] = ( char * )"chroot-host";
    argv[2] = NULL;

    status  = run_probe(argv);

    if (status != 0) {
        escape_test_fail("container successfully chrooted into host root");
        return -1;
    }

    escape_test_pass();

    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <cage> <escape_probe>\n", argv[0]);

        return 2;
    }

    cage_path  = argv[1];
    probe_path = argv[2];

    test_host_file_hidden();
    test_host_file_not_writable();

    test_proc_1_root();
    test_proc_self_root();

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

    test_chroot_host_denied();

    return escape_test_run();
}
