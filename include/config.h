#ifndef CAGE_CONFIG_H
#define CAGE_CONFIG_H

#define DEFAULT_CONFIG "cage.toml"

#include <stddef.h>

struct mount_config {
    char *source;
    char *target;
    int   readonly;
};

struct cage_config {
    char                *path;
    char                *rootfs;
    struct mount_config *mounts;
    size_t               mount_count;
};

int config_load(struct cage_config *config, const char *path);

int config_load_default(struct cage_config *config);

void config_free(struct cage_config *config);

#endif
