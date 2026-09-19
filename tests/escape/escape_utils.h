#ifndef CAGE_ESCAPE_UTILS_H
#define CAGE_ESCAPE_UTILS_H

#include <stddef.h>
#include <sys/types.h>

void escape_test_begin(const char *name);
void escape_test_pass(void);
void escape_test_fail(const char *message);

int escape_test_run(void);

int   escape_run_process(char *const argv[]);
pid_t escape_start_process(const char *path, char *const argv[]);
int   escape_wait_process(pid_t pid);

int escape_make_temp_dir(const char *template, char *out, size_t size);

int escape_make_temp_file(const char *template, char *out, size_t size);

int escape_write_file(const char *path, const char *contents);
int escape_read_file(const char *path, char *buf, size_t size);

int escape_file_exists(const char *path);
int escape_remove_tree(const char *path);

int escape_create_rootfs(const char *probe_path, const char *template,
                         char *rootfs, size_t size);

int escape_create_config(const char *path, const char *rootfs,
                         const char *extra);

int escape_namespace_identity(const char *name, unsigned long *dev,
                              unsigned long *ino);

#endif
