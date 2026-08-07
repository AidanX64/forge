#ifndef FORGE_MANIFEST_H
#define FORGE_MANIFEST_H

#include <stddef.h>

#define FORGE_MANIFEST_MAX_ITEMS 32U
#define FORGE_MANIFEST_VALUE_MAX 256U

typedef struct ForgeStringList {
    char items[FORGE_MANIFEST_MAX_ITEMS][FORGE_MANIFEST_VALUE_MAX];
    size_t count;
} ForgeStringList;

typedef struct ForgeBuildProfile {
    ForgeStringList cflags;
} ForgeBuildProfile;

typedef struct ForgeManifest {
    char project_name[FORGE_MANIFEST_VALUE_MAX];
    ForgeStringList c_source_dirs;
    ForgeStringList cpp_source_dirs;
    ForgeStringList asm_source_dirs;
    ForgeStringList target_os;
    ForgeStringList target_arch;
    char compiler_override[FORGE_MANIFEST_VALUE_MAX];
    ForgeBuildProfile debug_profile;
    ForgeBuildProfile release_profile;
} ForgeManifest;

/* Loads the supported Forge.toml subset. Returns 0 on success. */
int forge_manifest_load(const char *path, ForgeManifest *manifest,
                        char *error, size_t error_size);

#endif
