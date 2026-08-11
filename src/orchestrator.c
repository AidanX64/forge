#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/build.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"

#define FORGE_PATH_MAX 1024U
#define FORGE_MAX_SOURCE_FILES 1024U

typedef struct ForgeSourceFile {
    char path[FORGE_PATH_MAX];
    ForgeSourceLanguage language;
} ForgeSourceFile;

typedef struct ForgeSourceList {
    ForgeSourceFile *items;
    size_t count;
    size_t capacity;
} ForgeSourceList;

static ForgeLogger *active_logger;

static void print_error(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    if (active_logger != NULL) {
        char message[FORGE_COMMAND_MAX];
        (void)vsnprintf(message, sizeof(message), format, arguments);
        forge_logger_error(active_logger, "build", "%s", message);
    } else {
        fprintf(stderr, "forge: ");
        (void)vfprintf(stderr, format, arguments);
        fputc('\n', stderr);
    }
    va_end(arguments);
}

static int run_external_command(const char *stage, const char *command, int capture_output)
{
    char captured_command[FORGE_COMMAND_MAX + FORGE_LOG_PATH_MAX + 32U];
    char error[FORGE_COMMAND_MAX];
    int written;
    int status;

    forge_logger_log(active_logger, stage, "command: %s", command);
    if (active_logger == NULL || active_logger->file == NULL || !capture_output) {
        return system(command);
    }
    written = snprintf(captured_command, sizeof(captured_command),
                       "%s >> \"%s\" 2>&1", command, active_logger->path);
    if (written < 0 || (size_t)written >= sizeof(captured_command)) {
        print_error("command is too long to capture in the build log");
        return -1;
    }
    if (forge_logger_suspend(active_logger, error, sizeof(error)) != 0) {
        print_error("%s", error);
        return -1;
    }
    status = system(captured_command);
    if (forge_logger_resume(active_logger, error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return -1;
    }
    return status;
}

/*
 * Emits the tail of the invocation log to stderr. Stage output is appended to
 * the log file during a build, so on a failed stage the terminal should show
 * the real diagnostic instead of only a terse "failed" line.
 */
static void print_log_tail(size_t max_lines)
{
    FILE *file;
    char *contents;
    char *start;
    char *cursor;
    size_t size;
    size_t lines = 0U;

    if (active_logger == NULL || active_logger->path[0] == '\0') {
        return;
    }
    file = fopen(active_logger->path, "rb");
    if (file == NULL) {
        fprintf(stderr, "forge: [error] could not read the build log at %s\n",
                active_logger->path);
        return;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return;
    }
    {
        long length = ftell(file);
        if (length <= 0L) {
            (void)fclose(file);
            return;
        }
        contents = malloc((size_t)length + 1U);
        if (contents == NULL) {
            (void)fclose(file);
            return;
        }
        rewind(file);
        size = fread(contents, 1U, (size_t)length, file);
        (void)fclose(file);
        contents[size] = '\0';
    }
    cursor = contents + size;
    while (cursor > contents && lines < max_lines) {
        char *previous = cursor - 1;
        while (previous > contents && previous[-1] != '\n') {
            --previous;
        }
        if (previous == contents) {
            start = previous;
            ++lines;
            break;
        }
        start = previous;
        ++lines;
        cursor = previous - 1;
    }
    fprintf(stderr, "forge: [error] last %zu log line(s):\n", lines);
    {
        char *line = start;
        while (line != NULL && line < contents + size) {
            char *end = strchr(line, '\n');
            size_t length = end == NULL ? (size_t)(contents + size - line)
                                        : (size_t)(end - line);
            while (length > 0U && (line[length - 1U] == '\r' ||
                                   line[length - 1U] == '\n')) {
                --length;
            }
            fprintf(stderr, "  %.*s\n", (int)length, line);
            line = end == NULL ? NULL : end + 1;
        }
    }
    free(contents);
}

static int path_join(char *destination, size_t destination_size,
                     const char *left, const char *right)
{
    int written = snprintf(destination, destination_size, "%s/%s", left, right);
    return written < 0 || (size_t)written >= destination_size ? -1 : 0;
}

static int is_absolute_path(const char *path)
{
#ifdef _WIN32
    return (isalpha((unsigned char)path[0]) && path[1] == ':') ||
           path[0] == '/' || path[0] == '\\';
#else
    return path[0] == '/';
#endif
}

/*
 * Resolves a manifest-relative path against the project root. Paths that are
 * already absolute are passed through untouched; relative paths are anchored to
 * the directory that contains the manifest so a build is correct no matter
 * which directory Forge is invoked from.
 */
static int resolve_project_path(const char *root, const char *relative,
                                char *destination, size_t destination_size)
{
    if (is_absolute_path(relative)) {
        return snprintf(destination, destination_size, "%s", relative) < 0 ||
                       (size_t)snprintf(destination, destination_size, "%s", relative)
                           >= destination_size
                   ? -1
                   : 0;
    }
    return path_join(destination, destination_size, root, relative);
}

/*
 * Computes the absolute project root (the directory holding the manifest), so
 * source directories, build output, and logs stay anchored to the project.
 * Returns 0 on success with the root stored in `root`.
 */
int forge_build_project_root(const char *manifest_path, char *root,
                             size_t root_size)
{
    char full[FORGE_PATH_MAX];
    char *separator;

#if defined(_WIN32)
    {
        DWORD length = GetFullPathNameA(manifest_path, sizeof(full), full, NULL);
        if (length == 0U || length >= sizeof(full)) {
            print_error("could not resolve manifest path '%s'", manifest_path);
            return -1;
        }
    }
#else
    if (realpath(manifest_path, full) == NULL) {
        if (snprintf(full, sizeof(full), "%s", manifest_path) < 0 ||
            strlen(manifest_path) >= sizeof(full)) {
            print_error("could not resolve manifest path '%s'", manifest_path);
            return -1;
        }
    }
#endif
    if (strlen(full) >= root_size) {
        print_error("manifest path is too long");
        return -1;
    }
    separator = strrchr(full, '/');
#if defined(_WIN32)
    {
        char *backslash = strrchr(full, '\\');
        if (backslash != NULL && backslash > separator) {
            separator = backslash;
        }
    }
#endif
    if (separator == NULL) {
        (void)snprintf(root, root_size, ".");
        return 0;
    }
    *separator = '\0';
    (void)snprintf(root, root_size, "%s", full);
    return 0;
}

static int has_suffix(const char *path, const char *suffix)
{
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);

    return path_length >= suffix_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

static int source_matches_language(const char *path, ForgeSourceLanguage language)
{
    switch (language) {
    case FORGE_SOURCE_C:
        return has_suffix(path, ".c");
    case FORGE_SOURCE_CPP:
        return has_suffix(path, ".cc") || has_suffix(path, ".cpp") ||
               has_suffix(path, ".cxx");
    case FORGE_SOURCE_ASM:
        return has_suffix(path, ".s") || has_suffix(path, ".S") ||
               has_suffix(path, ".asm");
    }
    return 0;
}

static int source_list_add(ForgeSourceList *sources, const char *path,
                           ForgeSourceLanguage language)
{
    ForgeSourceFile *expanded;
    size_t new_capacity;

    if (sources->count == FORGE_MAX_SOURCE_FILES) {
        print_error("more than %u source files are not supported in one invocation",
                    FORGE_MAX_SOURCE_FILES);
        return -1;
    }
    if (sources->count == sources->capacity) {
        new_capacity = sources->capacity == 0U ? 32U : sources->capacity * 2U;
        if (new_capacity > FORGE_MAX_SOURCE_FILES) {
            new_capacity = FORGE_MAX_SOURCE_FILES;
        }
        expanded = realloc(sources->items, new_capacity * sizeof(*sources->items));
        if (expanded == NULL) {
            print_error("out of memory while collecting source files");
            return -1;
        }
        sources->items = expanded;
        sources->capacity = new_capacity;
    }
    if (snprintf(sources->items[sources->count].path,
                 sizeof(sources->items[sources->count].path), "%s", path) < 0 ||
        strlen(path) >= sizeof(sources->items[sources->count].path)) {
        print_error("source path is too long: %s", path);
        return -1;
    }
    sources->items[sources->count].language = language;
    ++sources->count;
    return 0;
}

static void source_list_free(ForgeSourceList *sources)
{
    free(sources->items);
    *sources = (ForgeSourceList){0};
}

#ifdef _WIN32
static int collect_sources_recursive(const char *directory, ForgeSourceLanguage language,
                                     ForgeSourceList *sources)
{
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];

    if (path_join(pattern, sizeof(pattern), directory, "*") != 0) {
        print_error("source directory path is too long: %s", directory);
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        print_error("could not read source directory '%s' (error %lu)", directory,
                    (unsigned long)GetLastError());
        return -1;
    }
    do {
        char path[FORGE_PATH_MAX];
        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (path_join(path, sizeof(path), directory, entry.cFileName) != 0) {
            print_error("source path is too long under '%s'", directory);
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (collect_sources_recursive(path, language, sources) != 0) {
                (void)FindClose(handle);
                return -1;
            }
        } else if (source_matches_language(path, language) &&
                   source_list_add(sources, path, language) != 0) {
            (void)FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &entry) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        print_error("could not finish reading source directory '%s' (error %lu)",
                    directory, (unsigned long)GetLastError());
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    return 0;
}
#else
static int collect_sources_recursive(const char *directory, ForgeSourceLanguage language,
                                     ForgeSourceList *sources)
{
    DIR *stream;
    struct dirent *entry;

    stream = opendir(directory);
    if (stream == NULL) {
        print_error("could not read source directory '%s': %s", directory, strerror(errno));
        return -1;
    }
    while ((entry = readdir(stream)) != NULL) {
        char path[FORGE_PATH_MAX];
        struct stat details;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (path_join(path, sizeof(path), directory, entry->d_name) != 0) {
            print_error("source path is too long under '%s'", directory);
            (void)closedir(stream);
            return -1;
        }
        if (stat(path, &details) != 0) {
            print_error("could not inspect source path '%s': %s", path, strerror(errno));
            (void)closedir(stream);
            return -1;
        }
        if (S_ISDIR(details.st_mode)) {
            if (collect_sources_recursive(path, language, sources) != 0) {
                (void)closedir(stream);
                return -1;
            }
        } else if (S_ISREG(details.st_mode) && source_matches_language(path, language) &&
                   source_list_add(sources, path, language) != 0) {
            (void)closedir(stream);
            return -1;
        }
    }
    (void)closedir(stream);
    return 0;
}
#endif

static int collect_sources(const char *project_root, const ForgeManifest *manifest,
                           ForgeSourceList *sources)
{
    const ForgeStringList *lists[] = {
        &manifest->c_source_dirs,
        &manifest->cpp_source_dirs,
        &manifest->asm_source_dirs
    };
    const ForgeSourceLanguage languages[] = {
        FORGE_SOURCE_C,
        FORGE_SOURCE_CPP,
        FORGE_SOURCE_ASM
    };
    size_t list_index;

    *sources = (ForgeSourceList){0};
    for (list_index = 0U; list_index < sizeof(lists) / sizeof(lists[0]); ++list_index) {
        size_t directory_index;
        for (directory_index = 0U; directory_index < lists[list_index]->count;
             ++directory_index) {
            char resolved[FORGE_PATH_MAX];
            if (resolve_project_path(project_root, lists[list_index]->items[directory_index],
                                     resolved, sizeof(resolved)) != 0) {
                print_error("source directory path is too long: %s",
                            lists[list_index]->items[directory_index]);
                source_list_free(sources);
                return -1;
            }
            if (collect_sources_recursive(resolved, languages[list_index], sources) != 0) {
                source_list_free(sources);
                return -1;
            }
        }
    }
    if (sources->count == 0U) {
        print_error("the manifest source directories contain no C, C++, or assembly files");
        return -1;
    }
    return 0;
}

static int ensure_directory(const char *path)
{
    char current[FORGE_PATH_MAX];
    size_t index;
    size_t length = strlen(path);

    if (length == 0U || length >= sizeof(current)) {
        print_error("invalid output directory path");
        return -1;
    }
    (void)snprintf(current, sizeof(current), "%s", path);
    for (index = 1U; index <= length; ++index) {
        if (current[index] != '/' && current[index] != '\\' && current[index] != '\0') {
            continue;
        }
        {
            char saved = current[index];
            current[index] = '\0';
#ifdef _WIN32
            if (CreateDirectoryA(current, NULL) == 0 &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                print_error("could not create output directory '%s' (error %lu)", current,
                            (unsigned long)GetLastError());
                return -1;
            }
#else
            if (mkdir(current, 0777) != 0 && errno != EEXIST) {
                print_error("could not create output directory '%s': %s", current,
                            strerror(errno));
                return -1;
            }
#endif
            current[index] = saved;
        }
    }
    return 0;
}

#ifdef _WIN32
static int remove_tree(const char *path)
{
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];

    if (path_join(pattern, sizeof(pattern), path, "*") != 0) {
        print_error("clean path is too long: %s", path);
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            return 0;
        }
        print_error("could not open directory to clean '%s' (error %lu)", path,
                    (unsigned long)GetLastError());
        return -1;
    }
    do {
        char child[FORGE_PATH_MAX];

        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (path_join(child, sizeof(child), path, entry.cFileName) != 0) {
            print_error("clean path is too long under '%s'", path);
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (remove_tree(child) != 0) {
                (void)FindClose(handle);
                return -1;
            }
        } else if (DeleteFileA(child) == 0) {
            print_error("could not delete file '%s' (error %lu)", child,
                        (unsigned long)GetLastError());
            (void)FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &entry) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        print_error("could not finish cleaning '%s' (error %lu)", path,
                    (unsigned long)GetLastError());
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    if (RemoveDirectoryA(path) == 0) {
        print_error("could not remove directory '%s' (error %lu)", path,
                    (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}
#else
static int remove_tree(const char *path)
{
    DIR *stream = opendir(path);
    struct dirent *entry;

    if (stream == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        print_error("could not open directory to clean '%s': %s", path, strerror(errno));
        return -1;
    }
    while ((entry = readdir(stream)) != NULL) {
        char child[FORGE_PATH_MAX];
        struct stat details;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (path_join(child, sizeof(child), path, entry->d_name) != 0) {
            print_error("clean path is too long under '%s'", path);
            (void)closedir(stream);
            return -1;
        }
        if (stat(child, &details) != 0) {
            print_error("could not inspect '%s': %s", child, strerror(errno));
            (void)closedir(stream);
            return -1;
        }
        if (S_ISDIR(details.st_mode)) {
            if (remove_tree(child) != 0) {
                (void)closedir(stream);
                return -1;
            }
        } else if (unlink(child) != 0) {
            print_error("could not delete '%s': %s", child, strerror(errno));
            (void)closedir(stream);
            return -1;
        }
    }
    (void)closedir(stream);
    if (rmdir(path) != 0) {
        print_error("could not remove directory '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}
#endif

static const char *host_architecture(void)
{
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

static int list_contains(const ForgeStringList *list, const char *value)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int select_host_target(const ForgeManifest *manifest, const ForgeHostInfo *host,
                              const char **target_os, const char **target_arch)
{
    const char *architecture = host_architecture();
    const char *os = forge_host_os_name(host->os);

    if (!list_contains(&manifest->target_os, os) ||
        !list_contains(&manifest->target_arch, architecture)) {
        print_error("manifest does not include this host target (%s/%s); "
                    "cross-compilation needs an explicit toolchain and is not configured",
                    os, architecture);
        return -1;
    }
    *target_os = os;
    *target_arch = architecture;
    return 0;
}

static void safe_output_name(const char *project_name, char *output, size_t output_size)
{
    size_t index;

    for (index = 0U; index + 1U < output_size && project_name[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)project_name[index];
        output[index] = isalnum(character) || character == '-' || character == '_' ||
                        character == '.' ? (char)character : '-';
    }
    output[index] = '\0';
}

int forge_build_project(const char *project_root, const ForgeManifest *manifest,
                        int release, int should_run,
                        char *built_executable, size_t built_executable_size)
{
    ForgeHostInfo host;
    ForgeCompiler compiler;
    ForgeSourceList sources = {0};
    const ForgeBuildProfile *profile;
    const char *target_os;
    const char *target_arch;
    char error[FORGE_COMMAND_MAX] = {0};
    char profile_directory[FORGE_PATH_MAX];
    char object_directory[FORGE_PATH_MAX];
    char executable_name[FORGE_MANIFEST_VALUE_MAX];
    char executable_path[FORGE_PATH_MAX];
    char command[FORGE_COMMAND_MAX];
    char (*object_paths)[FORGE_PATH_MAX] = NULL;
    const char **object_references = NULL;
    const char *flags[FORGE_MANIFEST_MAX_ITEMS];
    size_t source_index;
    size_t flag_index;
    int has_cpp_source = 0;
    int result = -1;

    if (forge_detect_host(&host, error, sizeof(error)) != 0) {
        print_error("%s", error);
        goto cleanup;
    }
    if (select_host_target(manifest, &host, &target_os, &target_arch) != 0 ||
        forge_compiler_select(&host, manifest->compiler_override, &compiler,
                              error, sizeof(error)) != 0 ||
        collect_sources(project_root, manifest, &sources) != 0) {
        if (error[0] != '\0') {
            print_error("%s", error);
        }
        goto cleanup;
    }

    profile = release ? &manifest->release_profile : &manifest->debug_profile;
    for (flag_index = 0U; flag_index < profile->cflags.count; ++flag_index) {
        flags[flag_index] = profile->cflags.items[flag_index];
    }
    if (resolve_project_path(project_root, "target", profile_directory,
                             sizeof(profile_directory)) != 0 ||
        path_join(profile_directory, sizeof(profile_directory), profile_directory,
                  release ? "release" : "debug") != 0 ||
        path_join(object_directory, sizeof(object_directory), profile_directory, "obj") != 0) {
        print_error("output path is too long");
        goto cleanup;
    }
    if (ensure_directory(object_directory) != 0) {
        goto cleanup;
    }
    safe_output_name(manifest->project_name, executable_name, sizeof(executable_name));
#ifdef _WIN32
    if (snprintf(executable_path, sizeof(executable_path), "%s/%s.exe",
                 profile_directory, executable_name) < 0 ||
        strlen(profile_directory) + strlen(executable_name) + 5U >= sizeof(executable_path)) {
#else
    if (path_join(executable_path, sizeof(executable_path), profile_directory,
                  executable_name) != 0) {
#endif
        print_error("executable output path is too long");
        goto cleanup;
    }
    object_paths = calloc(sources.count, sizeof(*object_paths));
    object_references = calloc(sources.count, sizeof(*object_references));
    if (object_paths == NULL || object_references == NULL) {
        print_error("out of memory while preparing build outputs");
        goto cleanup;
    }

    forge_logger_log(active_logger, "dispatch", "host=%s version=%s target=%s/%s",
                     host.os_name, host.version, target_os, target_arch);
    forge_logger_log(active_logger, "dispatch", "%s (%s)", compiler.selection_note,
                     forge_compiler_kind_name(compiler.kind));
    forge_logger_log(active_logger, "compile", "----- compile %zu source file(s) -----",
                     sources.count);
    for (source_index = 0U; source_index < sources.count; ++source_index) {
        int written = snprintf(object_paths[source_index], sizeof(object_paths[source_index]),
                               "%s/%04zu.o", object_directory, source_index);
        if (written < 0 || (size_t)written >= sizeof(object_paths[source_index]) ||
            forge_compiler_make_compile_command(&compiler, sources.items[source_index].language,
                                                sources.items[source_index].path,
                                                object_paths[source_index], target_os,
                                                target_arch, flags, profile->cflags.count,
                                                command, sizeof(command), error,
                                                sizeof(error)) != 0) {
            print_error("%s", error);
            goto cleanup;
        }
        forge_logger_log(active_logger, "compile", "source: %s",
                         sources.items[source_index].path);
        if (run_external_command("compile", command, 1) != 0) {
            print_error("compiler failed for %s", sources.items[source_index].path);
            print_log_tail(24U);
            goto cleanup;
        }
        object_references[source_index] = object_paths[source_index];
        if (sources.items[source_index].language == FORGE_SOURCE_CPP) {
            has_cpp_source = 1;
        }
    }

    if (forge_compiler_make_link_command(&compiler, has_cpp_source, object_references,
                                         sources.count, executable_path, flags,
                                         profile->cflags.count, command, sizeof(command),
                                         error, sizeof(error)) != 0) {
        print_error("%s", error);
        goto cleanup;
    }
    forge_logger_log(active_logger, "link", "----- link %s -----", executable_path);
    if (run_external_command("link", command, 1) != 0) {
        print_error("linker failed");
        print_log_tail(24U);
        goto cleanup;
    }
    if (built_executable != NULL && built_executable_size != 0U) {
        if (snprintf(built_executable, built_executable_size, "%s", executable_path) < 0 ||
            strlen(executable_path) >= built_executable_size) {
            print_error("built executable path is too long");
            goto cleanup;
        }
    }
    if (!should_run) {
        result = 0;
        goto cleanup;
    }
    forge_logger_log(active_logger, "run", "----- run %s -----", executable_path);
#ifdef _WIN32
    if (snprintf(command, sizeof(command), "cmd /C \"\"%s\"\"", executable_path) < 0 ||
        strlen(executable_path) + 13U >= sizeof(command)) {
#else
    if (snprintf(command, sizeof(command), "\"%s\"", executable_path) < 0 ||
        strlen(executable_path) + 3U >= sizeof(command)) {
#endif
        print_error("run command is too long");
        goto cleanup;
    }
    if (run_external_command("run", command, 0) != 0) {
        print_error("program failed");
        print_log_tail(24U);
        goto cleanup;
    }
    result = 0;

cleanup:
    free(object_references);
    free(object_paths);
    source_list_free(&sources);
    return result;
}

void forge_build_set_logger(ForgeLogger *logger)
{
    active_logger = logger;
}

/* Removes the project's target/ directory, anchored to its manifest. */
int forge_build_clean(const char *manifest_path)
{
    char project_root[FORGE_PATH_MAX];
    char target_path[FORGE_PATH_MAX];

    if (forge_build_project_root(manifest_path, project_root, sizeof(project_root)) != 0) {
        return 1;
    }
    if (resolve_project_path(project_root, "target", target_path, sizeof(target_path)) != 0) {
        print_error("clean path is too long");
        return 1;
    }
    printf("forge: removing %s\n", target_path);
    if (remove_tree(target_path) != 0) {
        return 1;
    }
    printf("forge: clean\n");
    return 0;
}
