#include <config.h>

#include <theft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *rootfs_values[] = {
        "/tmp/rootfs",
        "/var/lib/cage/rootfs",
        "/srv/cage/rootfs",
};

static const char *source_values[] = {
        "/tmp/source",
        "/var/lib/data",
        "/opt/application",
};

static const char *target_values[] = {
        "/mnt/data",
        "/home/data",
        "/var/cache/data",
};

static const char *whitespace_values[] = {
        "", " ", "  ", "\t", "\t ", " \t", "\t\t",
};

struct config_case {
    size_t rootfs;
    size_t source;
    size_t target;
    size_t ws_key;
    size_t ws_before_equals;
    size_t ws_after_equals;
    size_t ws_section;
    size_t ws_mount_key;
    size_t ws_mount_value;
    int    readonly;
};

static enum theft_alloc_res alloc_config_case(struct theft *t, void *env,
                                              void **output)
{
    struct config_case *config;

    ( void )env;

    config = malloc(sizeof(*config));

    if (config == NULL)
        return THEFT_ALLOC_ERROR;

    config->rootfs = ( size_t )theft_random_choice(
            t, sizeof(rootfs_values) / sizeof(rootfs_values[0]));

    config->source = ( size_t )theft_random_choice(
            t, sizeof(source_values) / sizeof(source_values[0]));

    config->target = ( size_t )theft_random_choice(
            t, sizeof(target_values) / sizeof(target_values[0]));

    config->ws_key = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->ws_before_equals = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->ws_after_equals = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->ws_section = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->ws_mount_key = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->ws_mount_value = ( size_t )theft_random_choice(
            t, sizeof(whitespace_values) / sizeof(whitespace_values[0]));

    config->readonly = ( int )theft_random_choice(t, 2);

    *output          = config;

    return THEFT_ALLOC_OK;
}

static void free_config_case(void *instance, void *env)
{
    ( void )env;

    free(instance);
}

static void print_config_case(FILE *file, const void *instance, void *env)
{
    const struct config_case *config = instance;

    ( void )env;

    fprintf(file,
            "rootfs=\"%s\" "
            "source=\"%s\" "
            "target=\"%s\" "
            "ws_key=%zu "
            "ws_before_equals=%zu "
            "ws_after_equals=%zu "
            "ws_section=%zu "
            "ws_mount_key=%zu "
            "ws_mount_value=%zu "
            "readonly=%d",
            rootfs_values[config->rootfs], source_values[config->source],
            target_values[config->target], config->ws_key,
            config->ws_before_equals, config->ws_after_equals,
            config->ws_section, config->ws_mount_key, config->ws_mount_value,
            config->readonly);
}

static enum theft_trial_res prop_whitespace_is_ignored(struct theft *t,
                                                       void         *arg)
{
    const struct config_case *expected = arg;
    struct cage_config        config;
    char                      path[] = "/tmp/cage-config-whitespace-XXXXXX";
    int                       fd;
    FILE                     *file;

    ( void )t;

    fd = mkstemp(path);

    if (fd == -1)
        return THEFT_TRIAL_ERROR;

    file = fdopen(fd, "w");

    if (file == NULL) {
        close(fd);
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    if (fprintf(file,
                "%srootfs%s = %s\"%s\"%s\n"
                "\n"
                "%s[[mounts]]%s\n"
                "%ssource%s = %s\"%s\"%s\n"
                "%starget%s = %s\"%s\"%s\n"
                "%sreadonly%s = %s%s%s\n",
                whitespace_values[expected->ws_key],
                whitespace_values[expected->ws_before_equals],
                whitespace_values[expected->ws_after_equals],
                rootfs_values[expected->rootfs],
                whitespace_values[expected->ws_after_equals],

                whitespace_values[expected->ws_section],
                whitespace_values[expected->ws_section],

                whitespace_values[expected->ws_mount_key],
                whitespace_values[expected->ws_before_equals],
                whitespace_values[expected->ws_after_equals],
                source_values[expected->source],
                whitespace_values[expected->ws_mount_value],

                whitespace_values[expected->ws_mount_key],
                whitespace_values[expected->ws_before_equals],
                whitespace_values[expected->ws_after_equals],
                target_values[expected->target],
                whitespace_values[expected->ws_mount_value],

                whitespace_values[expected->ws_mount_key],
                whitespace_values[expected->ws_before_equals],
                whitespace_values[expected->ws_after_equals],
                expected->readonly ? "true" : "false",
                whitespace_values[expected->ws_mount_value]) < 0) {
        fclose(file);
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    if (fclose(file) != 0) {
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    if (config_load(&config, path) == -1) {
        unlink(path);
        return THEFT_TRIAL_FAIL;
    }

    unlink(path);

    if (config.rootfs == NULL ||
        strcmp(config.rootfs, rootfs_values[expected->rootfs]) != 0)
        goto fail;

    if (config.mount_count != 1)
        goto fail;

    if (config.mounts[0].source == NULL ||
        strcmp(config.mounts[0].source, source_values[expected->source]) != 0)
        goto fail;

    if (config.mounts[0].target == NULL ||
        strcmp(config.mounts[0].target, target_values[expected->target]) != 0)
        goto fail;

    if (config.mounts[0].readonly != expected->readonly)
        goto fail;

    config_free(&config);

    return THEFT_TRIAL_PASS;

fail:
    config_free(&config);
    return THEFT_TRIAL_FAIL;
}

int main(void)
{
    struct theft_type_info type_info = {
            .alloc  = alloc_config_case,
            .free   = free_config_case,
            .hash   = NULL,
            .print  = print_config_case,
            .shrink = NULL,
            .autoshrink_config =
                    {
                                        .enable = false,
                                        },
            .env = NULL,
    };

    struct theft_run_config config = {
            .prop1 = prop_whitespace_is_ignored,
            .type_info =
                    {
                                [0] = &type_info,
                                },
            .name   = "config ignores surrounding whitespace",
            .trials = 500,
            .seed   = theft_seed_of_time(),
    };

    enum theft_run_res result;

    result = theft_run(&config);

    return result == THEFT_RUN_PASS ? EXIT_SUCCESS : EXIT_FAILURE;
}
