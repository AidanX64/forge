#ifndef FORGE_COMPILER_H
#define FORGE_COMPILER_H

#include <stddef.h>

#define FORGE_COMPILER_VALUE_MAX 256U
#define FORGE_COMMAND_MAX 8192U

typedef enum ForgeHostOs {
    FORGE_HOST_OS_WINDOWS,
    FORGE_HOST_OS_LINUX,
    FORGE_HOST_OS_MACOS,
    FORGE_HOST_OS_UNKNOWN
} ForgeHostOs;

typedef enum ForgeCompilerKind {
    FORGE_COMPILER_MSVC,
    FORGE_COMPILER_GCC,
    FORGE_COMPILER_CLANG
} ForgeCompilerKind;

typedef enum ForgeSourceLanguage {
    FORGE_SOURCE_C,
    FORGE_SOURCE_CPP,
    FORGE_SOURCE_ASM
} ForgeSourceLanguage;

typedef struct ForgeHostInfo {
    ForgeHostOs os;
    char os_name[FORGE_COMPILER_VALUE_MAX];
    char version[FORGE_COMPILER_VALUE_MAX];
} ForgeHostInfo;

typedef struct ForgeCompiler {
    ForgeCompilerKind kind;
    char program[FORGE_COMPILER_VALUE_MAX];
    int used_fallback;
    char selection_note[FORGE_COMPILER_VALUE_MAX];
} ForgeCompiler;

/* Identifies the operating system on which Forge is building. */
int forge_detect_host(ForgeHostInfo *host, char *error, size_t error_size);

/*
 * Selects a locally invocable compiler. An explicit override is honored first.
 * For an unknown host Forge selects clang deliberately and reports that choice
 * through selection_note; an unavailable fallback is reported as an error.
 */
int forge_compiler_select(const ForgeHostInfo *host, const char *override_program,
                          ForgeCompiler *compiler, char *error, size_t error_size);

/* Builds a shell command for compiling one source file to one object file. */
int forge_compiler_make_compile_command(const ForgeCompiler *compiler,
                                        ForgeSourceLanguage language,
                                        const char *source_path,
                                        const char *object_path,
                                        const char *target_os,
                                        const char *target_arch,
                                        const char *const *flags, size_t flag_count,
                                        char *command, size_t command_size,
                                        char *error, size_t error_size);

/* Builds a shell command for linking object files into an executable. */
int forge_compiler_make_link_command(const ForgeCompiler *compiler,
                                     int has_cpp_source,
                                     const char *const *object_paths,
                                     size_t object_count,
                                     const char *output_path,
                                     const char *const *flags, size_t flag_count,
                                     char *command, size_t command_size,
                                     char *error, size_t error_size);

const char *forge_host_os_name(ForgeHostOs os);
const char *forge_compiler_kind_name(ForgeCompilerKind kind);

#endif
