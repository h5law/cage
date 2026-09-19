#ifndef CAGE_TEST_UTILS_H
#define CAGE_TEST_UTILS_H

#include <stddef.h>
#include <sys/types.h>

void test_begin(const char *name);
void test_pass(void);
void test_fail(const char *message);
void test_skip(void);

int test_run(void);

int   run_process(char *const argv[]);
pid_t start_process(const char *path, char *const argv[]);
int   wait_process(pid_t pid);

int make_temp_dir(const char *template, char *out, size_t size);
int make_temp_file(const char *template, char *path, size_t size);

int write_file(const char *path, const char *contents);
int read_file(const char *path, char *buf, size_t size);
int file_exists(const char *path);
int remove_tree(const char *path);
int count_dirs_with_prefix(const char *directory, const char *prefix);

int create_rootfs(const char *probe_path, const char *template, char *rootfs,
                  size_t size);

int create_config(const char *path, const char *rootfs, const char *extra);

#endif
