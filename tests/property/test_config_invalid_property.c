#include <config.h>

#include <theft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct invalid_config_case {
    size_t kind;
};

static const char *invalid_configs[] = {
        /*
         * Missing rootfs.
         */
        "\n",

        /*
         * Duplicate rootfs.
         */
        "rootfs = \"/tmp/rootfs-a\"\n"
        "rootfs = \"/tmp/rootfs-b\"\n",

        /*
         * Unknown top-level setting.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "unknown = \"value\"\n",

        /*
         * Missing '='.
         */
        "rootfs \"/tmp/rootfs\"\n",

        /*
         * Invalid boolean.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "source = \"/tmp/source\"\n"
        "target = \"/data\"\n"
        "readonly = yes\n",

        /*
         * Missing mount source.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "target = \"/data\"\n",

        /*
         * Missing mount target.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "source = \"/tmp/source\"\n",

        /*
         * Relative mount target.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "source = \"/tmp/source\"\n"
        "target = \"data\"\n",

        /*
         * Unknown mount field.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "source = \"/tmp/source\"\n"
        "target = \"/data\"\n"
        "unknown = true\n",

        /*
         * Duplicate mount field.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "\n"
        "[[mounts]]\n"
        "source = \"/tmp/source-a\"\n"
        "source = \"/tmp/source-b\"\n"
        "target = \"/data\"\n",

        /*
         * Unterminated string.
         */
        "rootfs = \"/tmp/rootfs\n",

        /*
         * Unsupported table.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "[unsupported]\n",

        /*
         * Empty key.
         */
        "rootfs = \"/tmp/rootfs\"\n"
        "= \"value\"\n",

        /*
         * Invalid rootfs string.
         */
        "rootfs = /tmp/rootfs\n",
};

#define INVALID_CONFIG_COUNT                                                   \
    (sizeof(invalid_configs) / sizeof(invalid_configs[0]))

static enum theft_alloc_res alloc_invalid_config(struct theft *t, void *env,
                                                 void **output)
{
    struct invalid_config_case *config;

    ( void )env;

    config = malloc(sizeof(*config));

    if (config == NULL)
        return THEFT_ALLOC_ERROR;

    config->kind = ( size_t )theft_random_choice(t, INVALID_CONFIG_COUNT);

    *output      = config;

    return THEFT_ALLOC_OK;
}

static void free_invalid_config(void *instance, void *env)
{
    ( void )env;

    free(instance);
}

static void print_invalid_config(FILE *file, const void *instance, void *env)
{
    const struct invalid_config_case *config = instance;

    ( void )env;

    fprintf(file, "kind=%zu\n%s", config->kind, invalid_configs[config->kind]);
}

static enum theft_trial_res prop_config_rejects_invalid(struct theft *t,
                                                        void         *arg)
{
    const struct invalid_config_case *test_case = arg;
    struct cage_config                config;
    char  path[] = "/tmp/cage-invalid-property-XXXXXX";
    int   fd;
    FILE *file;
    int   result;

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

    if (fputs(invalid_configs[test_case->kind], file) == EOF) {
        fclose(file);
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    if (fclose(file) != 0) {
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    /*
     * Pre-populate the structure so that a successful cleanup check
     * cannot accidentally pass simply because config_load() happened
     * to receive an already-zeroed structure.
     */
    config = (struct cage_config){
            .path        = strdup("sentinel"),
            .rootfs      = strdup("sentinel"),
            .mounts      = NULL,
            .mount_count = 0,
    };

    if (config.path == NULL || config.rootfs == NULL) {
        config_free(&config);
        unlink(path);
        return THEFT_TRIAL_ERROR;
    }

    result = config_load(&config, path);

    unlink(path);

    /*
     * Every generated input in this test is intentionally invalid.
     */
    if (result != -1) {
        config_free(&config);
        return THEFT_TRIAL_FAIL;
    }

    /*
     * config_load() must leave no partially allocated state after
     * rejecting the configuration.
     */
    if (config.path != NULL || config.rootfs != NULL || config.mounts != NULL ||
        config.mount_count != 0) {
        config_free(&config);
        return THEFT_TRIAL_FAIL;
    }

    return THEFT_TRIAL_PASS;
}

int main(void)
{
    struct theft_type_info type_info = {
            .alloc  = alloc_invalid_config,
            .free   = free_invalid_config,
            .hash   = NULL,
            .print  = print_invalid_config,
            .shrink = NULL,
            .autoshrink_config =
                    {
                                        .enable = false,
                                        },
            .env = NULL,
    };

    struct theft_run_config config = {
            .prop1 = prop_config_rejects_invalid,
            .type_info =
                    {
                                [0] = &type_info,
                                },
            .name   = "config rejects invalid input",
            .trials = 500,
            .seed   = theft_seed_of_time(),
    };

    enum theft_run_res result;

    result = theft_run(&config);

    return result == THEFT_RUN_PASS ? EXIT_SUCCESS : EXIT_FAILURE;
}
