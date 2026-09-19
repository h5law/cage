#include "utils.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_ROOTFS       "/tmp/cage-test-rootfs-XXXXXX"
#define TEST_RUNTIME_DIR  "/tmp"
#define TEST_RUNTIME_PREF "cage-"
#define TEST_MOUNT        "/tmp/cage-test-mount-XXXXXX"

static const char *cage_path;
static const char *probe_path;

static int make_config(char *path, size_t size)
{
    return make_temp_file("/tmp/cage-test-config-XXXXXX", path, size);
}

static int runtime_dir_count(void)
{
    return count_dirs_with_prefix(TEST_RUNTIME_DIR, TEST_RUNTIME_PREF);
}

static int runtime_dirs_unchanged(int before)
{
    int after = runtime_dir_count();

    if (after < 0)
        return -1;

    return after == before ? 0 : 1;
}

static int prepare_rootfs_and_config(char *rootfs, size_t rootfs_size,
                                     char *config, size_t config_size)
{
    if (make_config(config, config_size) < 0)
        return -1;

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, rootfs_size) < 0) {
        unlink(config);
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    return 0;
}

static int test_exit_status(void)
{
    test_begin("command exit status is propagated");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"42",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 42) {
        test_fail("cage did not propagate the command exit status");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_signal(int sig, const char *name)
{
    test_begin(name);

    int runtime_before = runtime_dir_count();

    if (runtime_before < 0) {
        test_fail("failed to inspect runtime directory");
        return -1;
    }

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char signal_string[16];

    snprintf(signal_string, sizeof(signal_string), "%d", sig);

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"wait-signal",
            signal_string,
            NULL,
    };

    pid_t pid = start_process(cage_path, argv);

    if (pid < 0) {
        test_fail("failed to start cage");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    usleep(100000);

    if (kill(pid, sig) < 0) {
        kill(pid, SIGKILL);
        wait_process(pid);

        test_fail("failed to signal cage");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    int status = wait_process(pid);

    unlink(config);
    remove_tree(rootfs);

    if (status != 128 + sig) {
        test_fail("cage did not propagate the signal status");
        return -1;
    }

    if (runtime_dirs_unchanged(runtime_before) != 0) {
        test_fail("runtime directory leaked after signal teardown");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_namespace(const char *namespace_name, const char *name)
{
    test_begin(name);

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "/proc/self/ns/%s", namespace_name) >=
        ( int )sizeof(path)) {
        test_fail("namespace path is too long");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    struct stat st;

    if (stat(path, &st) < 0) {
        test_fail("failed to stat host namespace");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

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
            ( char * )namespace_name,
            dev,
            ino,
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0) {
        test_fail("namespace was not isolated");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_uid_mapping(void)
{
    test_begin("user namespace identity mapping");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char uid_string[32];
    char gid_string[32];

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

    if (status != 0) {
        test_fail("user namespace identity mapping failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_tmpfs(void)
{
    test_begin("/tmp is a private writable tmpfs");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

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

    if (status != 0) {
        test_fail("/tmp tmpfs probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_tmpfs_setup_failure_cleanup(void)
{
    test_begin("tmpfs setup failure cleans up");

    int runtime_before = runtime_dir_count();

    if (runtime_before < 0) {
        test_fail("failed to inspect runtime directory");
        return -1;
    }

    char config[PATH_MAX];
    char rootfs[PATH_MAX];
    char tmp_path[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        test_fail("failed to create rootfs");
        return -1;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", rootfs) >=
                ( int )sizeof(tmp_path) ||
        write_file(tmp_path, "not-a-directory") < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create invalid /tmp");
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create configuration");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"0",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);

    if (status == 0) {
        remove_tree(rootfs);
        test_fail("container accepted invalid /tmp");
        return -1;
    }

    if (remove_tree(rootfs) != 0) {
        test_fail("rootfs could not be cleaned after tmpfs failure");
        return -1;
    }

    if (runtime_dirs_unchanged(runtime_before) != 0) {
        test_fail("runtime directory leaked after tmpfs setup failure");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_proc(void)
{
    test_begin("/proc is a proc filesystem");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

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

    if (status != 0) {
        test_fail("/proc probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_dev(void)
{
    test_begin("/dev is a private tmpfs with device nodes");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

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
        test_fail("/dev probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_capabilities(void)
{
    test_begin("capabilities are dropped");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",     config,
            ( char * )"/bin/probe", ( char * )"capabilities", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0) {
        test_fail("capability probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_no_new_privs(void)
{
    test_begin("no_new_privs is enabled");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",     config,
            ( char * )"/bin/probe", ( char * )"no-new-privs", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0) {
        test_fail("no_new_privs probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_fd_inheritance(void)
{
    test_begin("internal file descriptors are not inherited");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config",       config,
            ( char * )"/bin/probe", ( char * )"fd-inheritance", NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status != 0) {
        test_fail("file descriptor inheritance probe failed");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_exec_failure(void)
{
    test_begin("exec failure is propagated");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

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

    if (status != 127) {
        test_fail("exec failure did not return status 127");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_invalid_rootfs(void)
{
    test_begin("invalid rootfs is rejected");

    char config[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_config(config, "/tmp/cage-rootfs-that-does-not-exist", NULL) <
        0) {
        unlink(config);
        test_fail("failed to create configuration");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"0",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);

    if (status == 0) {
        test_fail("invalid rootfs was accepted");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_parent_death(void)
{
    test_begin("container dies when cage supervisor dies");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];

    if (prepare_rootfs_and_config(rootfs, sizeof(rootfs), config,
                                  sizeof(config)) < 0) {
        test_fail("failed to prepare test rootfs");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,    ( char * )"--config", config,
            ( char * )"/bin/probe", ( char * )"hold",     NULL,
    };

    pid_t pid = start_process(cage_path, argv);

    if (pid < 0) {
        test_fail("failed to start cage");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    usleep(200000);

    if (kill(pid, SIGKILL) < 0) {
        wait_process(pid);
        test_fail("failed to kill cage supervisor");
        unlink(config);
        remove_tree(rootfs);
        return -1;
    }

    wait_process(pid);

    usleep(200000);

    unlink(config);
    remove_tree(rootfs);

    test_pass();
    return 0;
}

static int test_repeated_container_lifecycle(void)
{
    test_begin("repeated container creation and teardown");

    int runtime_before = runtime_dir_count();

    if (runtime_before < 0) {
        test_fail("failed to inspect runtime directory");
        return -1;
    }

    for (int i = 0; i < 10; ++i) {
        char config[PATH_MAX];
        char rootfs[PATH_MAX];

        if (make_config(config, sizeof(config)) < 0) {
            test_fail("failed to create configuration");
            return -1;
        }

        if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) <
            0) {
            unlink(config);
            test_fail("failed to create rootfs");
            return -1;
        }

        if (create_config(config, rootfs, NULL) < 0) {
            unlink(config);
            remove_tree(rootfs);
            test_fail("failed to create configuration");
            return -1;
        }

        char *argv[] = {
                ( char * )cage_path,
                ( char * )"--config",
                config,
                ( char * )"/bin/probe",
                ( char * )"exit",
                ( char * )"0",
                NULL,
        };

        int status = run_process(argv);

        unlink(config);

        if (status != 0) {
            remove_tree(rootfs);
            test_fail("container lifecycle failed during repetition");
            return -1;
        }

        if (remove_tree(rootfs) != 0) {
            test_fail("rootfs cleanup failed after repetition");
            return -1;
        }

        if (runtime_dirs_unchanged(runtime_before) != 0) {
            test_fail("runtime directory leaked during repeated lifecycle");
            return -1;
        }
    }

    test_pass();
    return 0;
}

static int test_configured_writable_mount(void)
{
    test_begin("configured writable mount is writable");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];
    char mount_dir[PATH_MAX];
    char source_file[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        test_fail("failed to create rootfs");
        return -1;
    }

    if (make_temp_dir(TEST_MOUNT, mount_dir, sizeof(mount_dir)) < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create mount source");
        return -1;
    }

    if (snprintf(source_file, sizeof(source_file), "%s/test", mount_dir) >=
                ( int )sizeof(source_file) ||
        write_file(source_file, "before") < 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to create source file");
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to create configuration");
        return -1;
    }

    FILE *fp = fopen(config, "a");

    if (fp == NULL) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to open configuration");
        return -1;
    }

    if (fprintf(fp,
                "\n"
                "[[mounts]]\n"
                "source = \"%s\"\n"
                "target = \"/data\"\n",
                mount_dir) < 0 ||
        fclose(fp) != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to append mount configuration");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"write-file",
            ( char * )"/data/test",
            ( char * )"after",
            NULL,
    };

    int status = run_process(argv);

    if (status != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("cage failed to write through configured mount");
        return -1;
    }

    char contents[128];

    if (read_file(source_file, contents, sizeof(contents)) < 0 ||
        strcmp(contents, "after") != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("configured writable mount did not modify source");
        return -1;
    }

    unlink(config);
    remove_tree(rootfs);
    remove_tree(mount_dir);

    test_pass();
    return 0;
}

static int test_configured_readonly_mount(void)
{
    test_begin("configured read-only mount rejects writes");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];
    char mount_dir[PATH_MAX];
    char source_file[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        test_fail("failed to create rootfs");
        return -1;
    }

    if (make_temp_dir(TEST_MOUNT, mount_dir, sizeof(mount_dir)) < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create mount source");
        return -1;
    }

    if (snprintf(source_file, sizeof(source_file), "%s/test", mount_dir) >=
                ( int )sizeof(source_file) ||
        write_file(source_file, "original") < 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to create source file");
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to create configuration");
        return -1;
    }

    FILE *fp = fopen(config, "a");

    if (fp == NULL) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to open configuration");
        return -1;
    }

    if (fprintf(fp,
                "\n"
                "[[mounts]]\n"
                "source = \"%s\"\n"
                "target = \"/data\"\n"
                "readonly = true\n",
                mount_dir) < 0 ||
        fclose(fp) != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to append mount configuration");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"write-file",
            ( char * )"/data/test",
            ( char * )"must-not-write",
            NULL,
    };

    int status = run_process(argv);

    char contents[128];

    int unchanged = read_file(source_file, contents, sizeof(contents)) == 0 &&
                    strcmp(contents, "original") == 0;

    unlink(config);
    remove_tree(rootfs);
    remove_tree(mount_dir);

    if (status == 0) {
        test_fail("read-only mount allowed the write");
        return -1;
    }

    if (!unchanged) {
        test_fail("read-only mount modified the host source");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_configured_mount_survives_teardown(void)
{
    test_begin("configured mount data survives container teardown");

    char config[PATH_MAX];
    char rootfs[PATH_MAX];
    char mount_dir[PATH_MAX];
    char persistent[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        test_fail("failed to create rootfs");
        return -1;
    }

    if (make_temp_dir(TEST_MOUNT, mount_dir, sizeof(mount_dir)) < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create mount source");
        return -1;
    }

    if (snprintf(persistent, sizeof(persistent), "%s/persistent", mount_dir) >=
        ( int )sizeof(persistent)) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("persistent path is too long");
        return -1;
    }

    if (create_config(config, rootfs, NULL) < 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to create configuration");
        return -1;
    }

    FILE *fp = fopen(config, "a");

    if (fp == NULL) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to open configuration");
        return -1;
    }

    if (fprintf(fp,
                "\n"
                "[[mounts]]\n"
                "source = \"%s\"\n"
                "target = \"/data\"\n",
                mount_dir) < 0 ||
        fclose(fp) != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("failed to append mount configuration");
        return -1;
    }

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

    if (status != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("container failed before teardown");
        return -1;
    }

    char contents[128];

    if (!file_exists(persistent) ||
        read_file(persistent, contents, sizeof(contents)) < 0 ||
        strcmp(contents, "persistent-data") != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(mount_dir);
        test_fail("mounted data did not survive container teardown");
        return -1;
    }

    unlink(config);
    remove_tree(rootfs);
    remove_tree(mount_dir);

    test_pass();
    return 0;
}

static int test_configured_mount_failure_cleanup(void)
{
    test_begin("configured mount failure cleans up");

    int runtime_before = runtime_dir_count();

    if (runtime_before < 0) {
        test_fail("failed to inspect runtime directory");
        return -1;
    }

    char config[PATH_MAX];
    char rootfs[PATH_MAX];
    char first_mount[PATH_MAX];
    char missing_mount[PATH_MAX];

    if (make_config(config, sizeof(config)) < 0) {
        test_fail("failed to create configuration");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        unlink(config);
        test_fail("failed to create rootfs");
        return -1;
    }

    if (make_temp_dir(TEST_MOUNT, first_mount, sizeof(first_mount)) < 0) {
        unlink(config);
        remove_tree(rootfs);
        test_fail("failed to create first mount source");
        return -1;
    }

    if (snprintf(missing_mount, sizeof(missing_mount), "%s/missing-source",
                 first_mount) >= ( int )sizeof(missing_mount)) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(first_mount);
        test_fail("missing mount path is too long");
        return -1;
    }

    FILE *fp = fopen(config, "w");

    if (fp == NULL) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(first_mount);
        test_fail("failed to create configuration");
        return -1;
    }

    if (fprintf(fp,
                "rootfs = \"%s\"\n\n"
                "[[mounts]]\n"
                "source = \"%s\"\n"
                "target = \"/first\"\n\n"
                "[[mounts]]\n"
                "source = \"%s\"\n"
                "target = \"/second\"\n",
                rootfs, first_mount, missing_mount) < 0 ||
        fclose(fp) != 0) {
        unlink(config);
        remove_tree(rootfs);
        remove_tree(first_mount);
        test_fail("failed to write configuration");
        return -1;
    }

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            config,
            ( char * )"/bin/probe",
            ( char * )"exit",
            ( char * )"0",
            NULL,
    };

    int status = run_process(argv);

    unlink(config);
    remove_tree(rootfs);

    if (status == 0) {
        remove_tree(first_mount);
        test_fail("container accepted the invalid mount");
        return -1;
    }

    if (remove_tree(first_mount) != 0) {
        test_fail("configured mount remained after setup failure");
        return -1;
    }

    if (runtime_dirs_unchanged(runtime_before) != 0) {
        test_fail("runtime directory leaked after configured mount failure");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_default_config(void)
{
    test_begin("default configuration is loaded");

    char cwd[PATH_MAX];
    char rootfs[PATH_MAX];
    char config[PATH_MAX];
    char backup[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        test_fail("failed to determine current directory");
        return -1;
    }

    if (create_rootfs(probe_path, TEST_ROOTFS, rootfs, sizeof(rootfs)) < 0) {
        test_fail("failed to create rootfs");
        return -1;
    }

    if (snprintf(config, sizeof(config), "%s/cage.toml", cwd) >=
        ( int )sizeof(config)) {
        remove_tree(rootfs);
        test_fail("configuration path is too long");
        return -1;
    }

    bool had_config = file_exists(config);

    if (had_config) {
        if (snprintf(backup, sizeof(backup), "%s/cage.toml.cage-test-backup",
                     cwd) >= ( int )sizeof(backup)) {
            remove_tree(rootfs);
            test_fail("backup path is too long");
            return -1;
        }

        unlink(backup);

        if (rename(config, backup) < 0) {
            remove_tree(rootfs);
            test_fail("failed to back up existing cage.toml");
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

    if (status != 23) {
        test_fail("default cage.toml was not loaded");
        return -1;
    }

    test_pass();
    return 0;

error:
    unlink(config);

    if (had_config)
        rename(backup, config);

    remove_tree(rootfs);

    test_fail("failed to create default configuration");
    return -1;
}

static int test_missing_config_argument(void)
{
    test_begin("missing --config argument is rejected");

    char *argv[] = {
            ( char * )cage_path,
            ( char * )"--config",
            NULL,
    };

    int status = run_process(argv);

    if (status == 0) {
        test_fail("missing --config argument was accepted");
        return -1;
    }

    test_pass();
    return 0;
}

static int test_duplicate_config_argument(void)
{
    test_begin("duplicate --config is rejected");

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

    if (status == 0) {
        test_fail("duplicate --config argument was accepted");
        return -1;
    }

    test_pass();
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

    test_exit_status();

    test_signal(SIGTERM, "SIGTERM");
    test_signal(SIGINT, "SIGINT");
    test_signal(SIGHUP, "SIGHUP");
    test_signal(SIGQUIT, "SIGQUIT");

    test_namespace("pid", "PID namespace isolates process IDs");
    test_namespace("user", "user namespace isolates user identity");
    test_uid_mapping();
    test_namespace("mnt", "mount namespace isolates mounts");

    test_tmpfs();
    test_proc();
    test_dev();

    test_namespace("net", "network namespace isolates networking");
    test_namespace("ipc", "IPC namespace isolates IPC objects");

    test_capabilities();
    test_no_new_privs();
    test_fd_inheritance();
    test_exec_failure();
    test_invalid_rootfs();
    test_parent_death();

    test_configured_writable_mount();
    test_configured_readonly_mount();
    test_configured_mount_survives_teardown();
    test_configured_mount_failure_cleanup();
    test_tmpfs_setup_failure_cleanup();
    test_repeated_container_lifecycle();

    test_default_config();
    test_missing_config_argument();
    test_duplicate_config_argument();

    return test_run();
}
