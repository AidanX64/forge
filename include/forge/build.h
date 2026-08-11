#ifndef FORGE_BUILD_H
#define FORGE_BUILD_H

#include <stddef.h>

#include "forge/log.h"
#include "forge/manifest.h"

/* The build engine operates on a parsed manifest and an invocation logger. */
void forge_build_set_logger(ForgeLogger *logger);
int forge_build_project_root(const char *manifest_path, char *root, size_t root_size);
int forge_build_project(const char *project_root, const ForgeManifest *manifest,
                        int release, int should_run,
                        char *built_executable, size_t built_executable_size);
int forge_build_clean(const char *manifest_path);

#endif
