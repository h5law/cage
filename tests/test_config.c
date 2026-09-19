#include "config.h"
#include "utils.h"

#include <string.h>
#include <unistd.h>

static int load_config(const char *content, struct cage_config *config)
{
    char path[64];

    if (make_temp_file("/tmp/cage-config-test-XXXXXX", path, sizeof(path)) < 0)
        return -1;

    if (write_file(path, content) < 0) {
        unlink(path);
        return -1;
    }

    int result = config_load(config, path);

    unlink(path);

    return result;
}

static void test_rootfs(void)
{
    test_begin("parses rootfs");

    struct cage_config config = {0};

    if (load_config("rootfs = \"/tmp/rootfs\"\n", &config) == 0 &&
        config.rootfs != NULL && strcmp(config.rootfs, "/tmp/rootfs") == 0 &&
        config.mount_count == 0) {
        config_free(&config);
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("rootfs was not parsed correctly");
}

static void test_mount(void)
{
    test_begin("parses writable mount");

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
        config_free(&config);
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("mount fields were not parsed correctly");
}

static void test_readonly_mount(void)
{
    test_begin("parses read-only mount");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "readonly = true\n";

    if (load_config(content, &config) == 0 && config.mount_count == 1 &&
        config.mounts[0].readonly == 1) {
        config_free(&config);
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("readonly=true was not parsed correctly");
}

static void test_multiple_mounts(void)
{
    test_begin("parses multiple mounts");

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
        config_free(&config);
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("multiple mount entries were not parsed correctly");
}

static void test_missing_rootfs(void)
{
    test_begin("rejects missing rootfs");

    struct cage_config config = {0};

    if (load_config("\n", &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("configuration without rootfs was accepted");
}

static void test_duplicate_rootfs(void)
{
    test_begin("rejects duplicate rootfs");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs-a\"\n"
                                "rootfs = \"/tmp/rootfs-b\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("duplicate rootfs setting was accepted");
}

static void test_duplicate_mount_field(void)
{
    test_begin("rejects duplicate mount field");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source-a\"\n"
                                "source = \"/tmp/source-b\"\n"
                                "target = \"/data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("duplicate source setting was accepted");
}

static void test_unknown_setting(void)
{
    test_begin("rejects unknown setting");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "unknown = \"value\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("unknown configuration setting was accepted");
}

static void test_unknown_mount_field(void)
{
    test_begin("rejects unknown mount field");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "unknown = true\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("unknown mount setting was accepted");
}

static void test_relative_mount_target(void)
{
    test_begin("rejects relative mount target");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("relative mount target was accepted");
}

static void test_invalid_readonly(void)
{
    test_begin("rejects invalid readonly value");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n"
                                "target = \"/data\"\n"
                                "readonly = yes\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("invalid readonly value was accepted");
}

static void test_missing_mount_source(void)
{
    test_begin("rejects mount without source");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "target = \"/data\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("mount without source was accepted");
}

static void test_missing_mount_target(void)
{
    test_begin("rejects mount without target");

    struct cage_config config = {0};

    const char *content       = "rootfs = \"/tmp/rootfs\"\n"
                                "\n"
                                "[[mounts]]\n"
                                "source = \"/tmp/source\"\n";

    if (load_config(content, &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("mount without target was accepted");
}

static void test_invalid_syntax(void)
{
    test_begin("rejects malformed setting");

    struct cage_config config = {0};

    if (load_config("rootfs \"/tmp/rootfs\"\n", &config) == -1) {
        test_pass();
        return;
    }

    config_free(&config);
    test_fail("malformed setting was accepted");
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

    return test_run();
}
