#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

static int tests_run;
static int tests_failed;

static void test_pass(const char *name)
{
    ++tests_run;
    printf("PASS: %s\n", name);
}

static void test_fail(const char *name, const char *reason)
{
    ++tests_run;
    ++tests_failed;

    printf("FAIL: %s: %s\n", name, reason);
}

static int write_config(const char *content, char *path, size_t size)
{
    int     fd;
    ssize_t written;
    size_t  length;

    if (size < sizeof("/tmp/cage-config-test-XXXXXX")) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(path, "/tmp/cage-config-test-XXXXXX");

    fd = mkstemp(path);

    if (fd == -1)
        return -1;

    length  = strlen(content);
    written = write(fd, content, length);

    if (written != ( ssize_t )length) {
        int saved_errno = errno;

        close(fd);
        unlink(path);

        if (written >= 0)
            errno = EIO;
        else
            errno = saved_errno;

        return -1;
    }

    if (close(fd) == -1) {
        int saved_errno = errno;

        unlink(path);
        errno = saved_errno;

        return -1;
    }

    return 0;
}

static int load_config(const char *content, struct cage_config *config)
{
    char path[64];
    int  result;

    if (write_config(content, path, sizeof(path)) == -1)
        return -1;

    result = config_load(config, path);

    unlink(path);

    return result;
}

static void test_rootfs(void)
{
    struct cage_config config = {0};

    if (load_config("rootfs = \"/tmp/rootfs\"\n", &config) == 0 &&
        config.rootfs != NULL && strcmp(config.rootfs, "/tmp/rootfs") == 0 &&
        config.mount_count == 0) {
        test_pass("parses rootfs");
    } else {
        test_fail("parses rootfs", "rootfs was not parsed correctly");
    }

    config_free(&config);
}

static void test_mount(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n";

    if (load_config(content, &config) == 0 && config.mount_count == 1 &&
        strcmp(config.mounts[0].source, "/tmp/source") == 0 &&
        strcmp(config.mounts[0].target, "/data") == 0 &&
        config.mounts[0].readonly == 0) {
        test_pass("parses writable mount");
    } else {
        test_fail("parses writable mount",
                  "mount fields were not parsed correctly");
    }

    config_free(&config);
}

static void test_readonly_mount(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "readonly = true\n";

    if (load_config(content, &config) == 0 && config.mount_count == 1 &&
        config.mounts[0].readonly == 1) {
        test_pass("parses read-only mount");
    } else {
        test_fail("parses read-only mount",
                  "readonly=true was not parsed correctly");
    }

    config_free(&config);
}

static void test_multiple_mounts(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source-a\"\n"
                                "target = \"/data\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source-b\"\n"
                                "target = \"/workspace\"\n"
                                "readonly = true\n";

    if (load_config(content, &config) == 0 && config.mount_count == 2 &&
        strcmp(config.mounts[0].source, "/tmp/source-a") == 0 &&
        strcmp(config.mounts[0].target, "/data") == 0 &&
        config.mounts[0].readonly == 0 &&
        strcmp(config.mounts[1].source, "/tmp/source-b") == 0 &&
        strcmp(config.mounts[1].target, "/workspace") == 0 &&
        config.mounts[1].readonly == 1) {
        test_pass("parses multiple mounts");
    } else {
        test_fail("parses multiple mounts",
                  "multiple mount entries were not parsed correctly");
    }

    config_free(&config);
}

static void test_missing_rootfs(void)
{
    struct cage_config config = {0};

    if (load_config("\n", &config) == -1) {
        test_pass("rejects missing rootfs");
    } else {
        test_fail("rejects missing rootfs",
                  "configuration without rootfs was accepted");
        config_free(&config);
    }
}

static void test_duplicate_rootfs(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs-a\"\n"
                                "rootfs = \"/tmp/rootfs-b\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects duplicate rootfs");
    } else {
        test_fail("rejects duplicate rootfs",
                  "duplicate rootfs setting was accepted");
        config_free(&config);
    }
}

static void test_duplicate_mount_field(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source-a\"\n"
                                "source = \"/tmp/source-b\"\n"
                                "target = \"/data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects duplicate mount field");
    } else {
        test_fail("rejects duplicate mount field",
                  "duplicate source setting was accepted");
        config_free(&config);
    }
}

static void test_unknown_setting(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "unknown = \"value\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects unknown setting");
    } else {
        test_fail("rejects unknown setting",
                  "unknown configuration setting was accepted");
        config_free(&config);
    }
}

static void test_unknown_mount_field(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "unknown = true\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects unknown mount field");
    } else {
        test_fail("rejects unknown mount field",
                  "unknown mount setting was accepted");
        config_free(&config);
    }
}

static void test_relative_mount_target(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects relative mount target");
    } else {
        test_fail("rejects relative mount target",
                  "relative mount target was accepted");
        config_free(&config);
    }
}

static void test_invalid_readonly(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "readonly = yes\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects invalid readonly value");
    } else {
        test_fail("rejects invalid readonly value",
                  "invalid readonly value was accepted");
        config_free(&config);
    }
}

static void test_missing_mount_source(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "target = \"/data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects mount without source");
    } else {
        test_fail("rejects mount without source",
                  "mount without source was accepted");
        config_free(&config);
    }
}

static void test_missing_mount_target(void)
{
    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n";

    if (load_config(content, &config) == -1) {
        test_pass("rejects mount without target");
    } else {
        test_fail("rejects mount without target",
                  "mount without target was accepted");
        config_free(&config);
    }
}

static void test_invalid_syntax(void)
{
    struct cage_config config = {0};

    if (load_config("rootfs \"/tmp/rootfs\"\n", &config) == -1) {
        test_pass("rejects malformed setting");
    } else {
        test_fail("rejects malformed setting",
                  "malformed setting was accepted");
        config_free(&config);
    }
}

int main(void)
{
    test_rootfs();
    test_mount();
    test_readonly_mount();
    test_multiple_mounts();

    test_missing_rootfs();
    test_duplicate_rootfs();
    test_duplicate_mount_field();
    test_unknown_setting();
    test_unknown_mount_field();
    test_relative_mount_target();
    test_invalid_readonly();
    test_missing_mount_source();
    test_missing_mount_target();
    test_invalid_syntax();

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);

    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
