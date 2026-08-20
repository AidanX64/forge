#ifndef FORGE_FINGERPRINT_H
#define FORGE_FINGERPRINT_H

#include <stddef.h>

#include "forge/paths.h"

/*
 * Returns 1 when the object file is at least as new as the source and, when
 * `track_headers` is set, at least as new as every header the compiler
 * recorded via -MMD -MF; 0 when the object is missing or stale. Toolchains
 * with no dependency file (MSVC, assembly sources) compare sources only.
 */
int forge_fingerprint_object_fresh(const char *object_path, const char *source_path,
                                   int track_headers);

/*
 * Picks a collision-free object path under `object_directory` for `source_path`.
 * Objects are named after a stable hash of the source path; if two sources hash
 * identically a numeric suffix is appended until the name is unique against the
 * already-claimed `used` names. Returns 0 on success.
 */
int forge_fingerprint_object_name(const char *object_directory, const char *source_path,
                                  char (*used)[FORGE_PATH_MAX], size_t used_count,
                                  char *object_path, size_t object_path_size);

#endif