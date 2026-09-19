#include <container.h>
#include <config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct cli_options {
    const char *config_path;
    int         config_explicit;
};

static int validate_rootfs(const char *rootfs)
{
    struct stat st;

    if (stat(rootfs, &st) == -1) {
        perror("rootfs");
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cage: rootfs '%s' is not a directory\n", rootfs);

        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: cage [options] <executable> [arguments...]\n"
                    "\n"
                    "options:\n"
                    "  --config <path>  use the specified configuration file\n"
                    "  -h, --help       show this help message\n");
}

static int parse_options(int argc, char **argv, struct cli_options *options,
                         int *command_index)
{
    int i;

    *options = (struct cli_options){
            .config_path     = NULL,
            .config_explicit = 0,
    };

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            i++;

            if (i >= argc) {
                fprintf(stderr, "cage: missing executable\n");
                return -1;
            }

            *command_index = i;
            return 0;
        }

        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr,
                        "cage: option '--config' requires an argument\n");
                return -1;
            }

            if (options->config_explicit) {
                fprintf(stderr,
                        "cage: option '--config' specified more than once\n");
                return -1;
            }

            options->config_path     = argv[++i];
            options->config_explicit = 1;
            continue;
        }

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            exit(EXIT_SUCCESS);
        }

        if (argv[i][0] == '-') {
            fprintf(stderr, "cage: unknown option '%s'\n", argv[i]);
            return -1;
        }

        *command_index = i;
        return 0;
    }

    fprintf(stderr, "cage: missing executable\n");
    return -1;
}

int main(int argc, char **argv)
{
    struct cage_config config;
    struct cli_options options;
    struct container   container;
    int                command_index;
    int                status;

    if (parse_options(argc, argv, &options, &command_index) == -1) {
        usage();
        return EXIT_FAILURE;
    }

    if (options.config_explicit) {
        if (config_load(&config, options.config_path) == -1)
            return EXIT_FAILURE;
    } else {
        if (config_load_default(&config) == -1)
            return EXIT_FAILURE;
    }

    if (validate_rootfs(config.rootfs) == -1) {
        config_free(&config);
        return EXIT_FAILURE;
    }

    container = (struct container){
            .config           = &config,
            .argv             = &argv[command_index],
            .pid              = -1,
            .private_dir      = {0},
            .overlay_upper    = {0},
            .overlay_work     = {0},
            .overlay_root     = {0},
            .overlay_old_root = {0},
            .private_dir_fd   = -1,
    };

    status = container_run(&container);

    config_free(&config);

    if (status < 0)
        return EXIT_FAILURE;

    return status;
}
