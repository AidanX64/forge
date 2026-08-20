#ifndef FORGE_PATHS_H
#define FORGE_PATHS_H

#include <stddef.h>

/* Canonical path buffer size used across Forge. */
#define FORGE_PATH_MAX 1024U

/* Joins `left` + "/" + `right` into `destination`; -1 if it cannot fit. */
int forge_paths_join(char *destination, size_t destination_size,
                     const char *left, const char *right);

/* Resolves a manifest-relative path against the project root. Absolute paths
 * pass through unchanged; relative paths are anchored to `root`. Returns 0 on
 * success. */
int forge_paths_resolve(const char *root, const char *relative,
                        char *destination, size_t destination_size);

/* Creates `path` and all missing parents (like mkdir -p). Returns 0 on
 * success, -1 with a message in `error` otherwise. */
int forge_paths_ensure_directory(const char *path, char *error, size_t error_size);

/* Removes the directory tree at `path` recursively. A missing root is not an
 * error. Returns 0 on success, -1 with a message in `error` otherwise. */
int forge_paths_remove_tree(const char *path, char *error, size_t error_size);

/* Computes the absolute directory containing `manifest_path`. Returns 0 on
 * success with the result in `root`. */
int forge_paths_project_root(const char *manifest_path, char *root,
                             size_t root_size, char *error, size_t error_size);

/* Maps `project_name` to a portable executable base name: all characters that
 * are not alphanumeric, '-', '_', or '.' become '-'. */
void forge_paths_safe_output_name(const char *project_name, char *output,
                                  size_t output_size);

#endif