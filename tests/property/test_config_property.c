#include <config.h>

#include <theft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_MOUNTS 4

static const char *rootfs_values[] = {
        "/",
        "/rootfs",
        "/srv/rootfs",
        "/tmp/rootfs",
};

static const char *source_values[] = {
        "/tmp/source",
        "/home",
        "/var",
        "/opt",
};

static const char *target_values[] = {
        "/mnt/source",
        "/home",
        "/var",
        "/opt",
};

struct config_case {
    size_t rootfs;
    size_t mount_count;

    size_t source[MAX_MOUNTS];
    size_t target[MAX_MOUNTS];
    int    readonly[MAX_MOUNTS];
};

static enum theft_alloc_res alloc_config_case(struct theft *t, void *env,
                                              void **output)
{
    struct config_case *config;
    size_t              i;

    ( void )env;

    config = calloc(1, sizeof(*config));

    if (config == NULL)
        return THEFT_ALLOC_ERROR;

    config->rootfs = ( size_t )theft_random_choice(
            t, sizeof(rootfs_values) / sizeof(rootfs_values[0]));

    config->mount_count = ( size_t )theft_random_choice(t, MAX_MOUNTS + 1);

    for (i = 0; i < config->mount_count; ++i) {
        config->source[i] = ( size_t )theft_random_choice(
                t, sizeof(source_values) / sizeof(source_values[0]));

        config->target[i] = ( size_t )theft_random_choice(
                t, sizeof(target_values) / sizeof(target_values[0]));

        config->readonly[i] = ( int )theft_random_choice(t, 2);
    }

    *output = config;

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
    size_t                    i;

    ( void )env;

    fprintf(file, "rootfs=%s mounts=%zu", rootfs_values[config->rootfs],
            config->mount_count);

    for (i = 0; i < config->mount_count; ++i) {
        fprintf(file, " [%s -> %s readonly=%d]",
                source_values[config->source[i]],
                target_values[config->target[i]], config->readonly[i]);
    }
}

static enum theft_trial_res prop_config_round_trip(struct theft *t, void *arg)
{
    const struct config_case *expected = arg;
    struct cage_config        config;
    char                      path[] = "/tmp/cage-property-XXXXXX";
    int                       fd;
    FILE                     *file;
    size_t                    i;
    int                       result;

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

    if (fprintf(file, "rootfs = \"%s\"\n", rootfs_values[expected->rootfs]) <
        0) {
        fclose(file);
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    for (i = 0; i < expected->mount_count; ++i) {
        if (fprintf(file,
                    "\n"
                    "[[mounts]]\n"
                    "source = \"%s\"\n"
                    "target = \"%s\"\n"
                    "readonly = %s\n",
                    source_values[expected->source[i]],
                    target_values[expected->target[i]],
                    expected->readonly[i] ? "true" : "false") < 0) {
            fclose(file);
            unlink(path);
            return THEFT_TRIAL_ERROR;
        }
    }

    if (fclose(file) != 0) {
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    result = config_load(&config, path);

    unlink(path);

    if (result != 0)
        return THEFT_TRIAL_FAIL;

    if (config.path == NULL || strcmp(config.path, path) != 0)
        goto fail;

    if (config.rootfs == NULL ||
        strcmp(config.rootfs, rootfs_values[expected->rootfs]) != 0)
        goto fail;

    if (config.mount_count != expected->mount_count)
        goto fail;

    for (i = 0; i < expected->mount_count; ++i) {
        const struct mount_config *mount = &config.mounts[i];

        if (mount->source == NULL ||
            strcmp(mount->source, source_values[expected->source[i]]) != 0)
            goto fail;

        if (mount->target == NULL ||
            strcmp(mount->target, target_values[expected->target[i]]) != 0)
            goto fail;

        if (mount->readonly != expected->readonly[i])
            goto fail;
    }

    config_free(&config);

    if (config.path != NULL || config.rootfs != NULL || config.mounts != NULL ||
        config.mount_count != 0)
        return THEFT_TRIAL_FAIL;

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
            .prop1 = prop_config_round_trip,
            .type_info =
                    {
                                [0] = &type_info,
                                },
            .name   = "config round-trip",
            .trials = 500,
            .seed   = theft_seed_of_time(),
    };

    enum theft_run_res result;

    result = theft_run(&config);

    return result == THEFT_RUN_PASS ? EXIT_SUCCESS : EXIT_FAILURE;
}
