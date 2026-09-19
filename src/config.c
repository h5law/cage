#include <config.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip_comment(char *line)
{
    int in_string = 0;

    for (; *line != '\0'; ++line) {
        if (*line == '"') {
            in_string = !in_string;
            continue;
        }

        if (*line == '#' && !in_string) {
            *line = '\0';
            return;
        }
    }
}

static char *trim(char *string)
{
    char *end;

    while (isspace(( unsigned char )*string))
        string++;

    if (*string == '\0')
        return string;

    end = string + strlen(string) - 1;

    while (end > string && isspace(( unsigned char )*end))
        end--;

    end[1] = '\0';

    return string;
}

static int parse_string(const char *value, char **result)
{
    const char *start;
    const char *end;
    size_t      length;
    char       *string;

    value = trim(( char * )value);

    if (*value != '"') {
        fprintf(stderr, "cage: expected quoted string\n");
        errno = EINVAL;
        return -1;
    }

    start = value + 1;
    end   = start;

    while (*end != '\0' && *end != '"') {
        if (*end == '\\') {
            fprintf(stderr,
                    "cage: escape sequences are not supported in strings\n");
            errno = EINVAL;
            return -1;
        }

        end++;
    }

    if (*end != '"') {
        fprintf(stderr, "cage: unterminated string\n");
        errno = EINVAL;
        return -1;
    }

    if (*trim(( char * )(end + 1)) != '\0') {
        fprintf(stderr, "cage: unexpected characters after string\n");
        errno = EINVAL;
        return -1;
    }

    length = ( size_t )(end - start);

    string = malloc(length + 1);

    if (string == NULL)
        return -1;

    memcpy(string, start, length);
    string[length] = '\0';

    *result        = string;

    return 0;
}

static int parse_boolean(const char *value, int *result)
{
    value = trim(( char * )value);

    if (strcmp(value, "true") == 0) {
        *result = 1;
        return 0;
    }

    if (strcmp(value, "false") == 0) {
        *result = 0;
        return 0;
    }

    fprintf(stderr, "cage: expected boolean value\n");
    errno = EINVAL;
    return -1;
}

static int add_mount(struct cage_config *config)
{
    struct mount_config *mounts;
    struct mount_config *mount;

    mounts = realloc(config->mounts,
                     (config->mount_count + 1) * sizeof(*config->mounts));

    if (mounts == NULL)
        return -1;

    config->mounts = mounts;
    mount          = &config->mounts[config->mount_count];

    *mount         = (struct mount_config){
            .source   = NULL,
            .target   = NULL,
            .readonly = 0,
    };

    ++config->mount_count;

    return 0;
}

static int parse_rootfs(struct cage_config *config, const char *value)
{
    char *rootfs;

    if (config->rootfs != NULL) {
        fprintf(stderr, "cage: duplicate rootfs setting\n");
        errno = EINVAL;
        return -1;
    }

    if (parse_string(value, &rootfs) == -1)
        return -1;

    config->rootfs = rootfs;

    return 0;
}

static int parse_mount_value(struct mount_config *mount, const char *key,
                             const char *value)
{
    if (strcmp(key, "source") == 0) {
        char *source;

        if (mount->source != NULL) {
            fprintf(stderr, "cage: duplicate mount source\n");
            errno = EINVAL;
            return -1;
        }

        if (parse_string(value, &source) == -1)
            return -1;

        mount->source = source;
        return 0;
    }

    if (strcmp(key, "target") == 0) {
        char *target;

        if (mount->target != NULL) {
            fprintf(stderr, "cage: duplicate mount target\n");
            errno = EINVAL;
            return -1;
        }

        if (parse_string(value, &target) == -1)
            return -1;

        mount->target = target;
        return 0;
    }

    if (strcmp(key, "readonly") == 0)
        return parse_boolean(value, &mount->readonly);

    fprintf(stderr, "cage: unknown mount setting '%s'\n", key);
    errno = EINVAL;
    return -1;
}

static int parse_line(struct cage_config   *config,
                      struct mount_config **current_mount, char *line,
                      size_t line_number)
{
    char *key;
    char *value;
    char *equals;

    line = trim(line);

    if (*line == '\0' || *line == '#')
        return 0;

    strip_comment(line);
    line = trim(line);

    if (*line == '\0')
        return 0;

    if (strcmp(line, "[[mounts]]") == 0) {
        if (add_mount(config) == -1)
            return -1;

        *current_mount = &config->mounts[config->mount_count - 1];

        return 0;
    }

    if (*line == '[') {
        fprintf(stderr, "cage: unsupported table on line %zu: %s\n",
                line_number, line);
        errno = EINVAL;
        return -1;
    }

    equals = strchr(line, '=');

    if (equals == NULL) {
        fprintf(stderr, "cage: expected key/value pair on line %zu\n",
                line_number);
        errno = EINVAL;
        return -1;
    }

    *equals = '\0';

    key     = trim(line);
    value   = trim(equals + 1);

    if (*key == '\0') {
        fprintf(stderr, "cage: empty key on line %zu\n", line_number);
        errno = EINVAL;
        return -1;
    }

    if (*current_mount != NULL)
        return parse_mount_value(*current_mount, key, value);

    if (strcmp(key, "rootfs") == 0)
        return parse_rootfs(config, value);

    fprintf(stderr, "cage: unknown setting '%s' on line %zu\n", key,
            line_number);
    errno = EINVAL;

    return -1;
}

static int validate_config(const struct cage_config *config)
{
    size_t i;

    if (config->rootfs == NULL) {
        fprintf(stderr, "cage: configuration is missing 'rootfs'\n");
        errno = EINVAL;
        return -1;
    }

    for (i = 0; i < config->mount_count; ++i) {
        const struct mount_config *mount = &config->mounts[i];

        if (mount->source == NULL) {
            fprintf(stderr, "cage: mount %zu is missing 'source'\n", i);
            errno = EINVAL;
            return -1;
        }

        if (mount->target == NULL) {
            fprintf(stderr, "cage: mount %zu is missing 'target'\n", i);
            errno = EINVAL;
            return -1;
        }

        if (mount->target[0] != '/') {
            fprintf(stderr, "cage: mount target '%s' is not an absolute path\n",
                    mount->target);
            errno = EINVAL;
            return -1;
        }
    }

    return 0;
}

int config_load(struct cage_config *config, const char *path)
{
    FILE                *file;
    char                *line          = NULL;
    size_t               line_capacity = 0;
    size_t               line_number   = 0;
    struct mount_config *current_mount = NULL;
    int                  result        = -1;

    if (config == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }

    *config = (struct cage_config){
            .path        = NULL,
            .rootfs      = NULL,
            .mounts      = NULL,
            .mount_count = 0,
    };

    config->path = strdup(path);

    if (config->path == NULL)
        return -1;

    file = fopen(path, "r");

    if (file == NULL) {
        fprintf(stderr, "cage: cannot open config '%s': %s\n", path,
                strerror(errno));
        goto fail;
    }

    while (getline(&line, &line_capacity, file) != -1) {
        ++line_number;

        if (parse_line(config, &current_mount, line, line_number) == -1) {
            fprintf(stderr, "cage: invalid configuration '%s'\n", path);
            goto fail;
        }
    }

    if (ferror(file)) {
        fprintf(stderr, "cage: failed reading config '%s': %s\n", path,
                strerror(errno));
        goto fail;
    }

    if (validate_config(config) == -1)
        goto fail;

    result = 0;

fail:
    free(line);
    fclose(file);

    if (result == -1)
        config_free(config);

    return result;
}

int config_load_default(struct cage_config *config)
{
    return config_load(config, DEFAULT_CONFIG);
}

void config_free(struct cage_config *config)
{
    size_t i;

    if (config == NULL)
        return;

    free(config->path);
    free(config->rootfs);

    for (i = 0; i < config->mount_count; i++) {
        free(config->mounts[i].source);
        free(config->mounts[i].target);
    }

    free(config->mounts);

    *config = (struct cage_config){
            .path        = NULL,
            .rootfs      = NULL,
            .mounts      = NULL,
            .mount_count = 0,
    };
}
