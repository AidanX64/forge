#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/sources.h"
#include "forge/platform.h"
#include "forge_util.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

static int source_matches_language(const char *path, ForgeSourceLanguage language)
{
    switch (language) {
    case FORGE_SOURCE_C:
        return forge_util_has_suffix(path, ".c");
    case FORGE_SOURCE_CPP:
        return forge_util_has_suffix(path, ".cc") || forge_util_has_suffix(path, ".cpp") ||
               forge_util_has_suffix(path, ".cxx");
    case FORGE_SOURCE_ASM:
        return forge_util_has_suffix(path, ".s") || forge_util_has_suffix(path, ".S") ||
               forge_util_has_suffix(path, ".asm");
    }
    return 0;
}

static int source_list_add(ForgeSourceList *sources, const char *path,
                           ForgeSourceLanguage language, char *error, size_t error_size)
{
    ForgeSourceFile *expanded;
    size_t new_capacity;

    if (sources->count == FORGE_MAX_SOURCE_FILES) {
        forge_util_set_error(error, error_size,
                  "more than %u source files are not supported in one invocation",
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
            forge_util_set_error(error, error_size,
                                 "out of memory while collecting source files");
            return -1;
        }
        sources->items = expanded;
        sources->capacity = new_capacity;
    }
    if (snprintf(sources->items[sources->count].path,
                 sizeof(sources->items[sources->count].path), "%s", path) < 0 ||
        strlen(path) >= sizeof(sources->items[sources->count].path)) {
        forge_util_set_error(error, error_size, "source path is too long: %s", path);
        return -1;
    }
    sources->items[sources->count].language = language;
    ++sources->count;
    return 0;
}

#if FORGE_PLATFORM_WINDOWS
static int collect_sources_recursive(const char *directory, ForgeSourceLanguage language,
                                     ForgeSourceList *sources, char *error, size_t error_size)
{
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];

    if (forge_paths_join(pattern, sizeof(pattern), directory, "*") != 0) {
        forge_util_set_error(error, error_size,
                             "source directory path is too long: %s", directory);
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        forge_util_set_error(error, error_size,
                  "could not read source directory '%s' (error %lu)", directory,
                  (unsigned long)GetLastError());
        return -1;
    }
    do {
        char path[FORGE_PATH_MAX];
        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (forge_paths_join(path, sizeof(path), directory, entry.cFileName) != 0) {
            forge_util_set_error(error, error_size,
                                 "source path is too long under '%s'", directory);
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (collect_sources_recursive(path, language, sources, error, error_size) != 0) {
                (void)FindClose(handle);
                return -1;
            }
        } else if (source_matches_language(path, language) &&
                   source_list_add(sources, path, language, error, error_size) != 0) {
            (void)FindClose(handle);
            return -1;
        }
    } while (FindNextFileA(handle, &entry) != 0);

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        forge_util_set_error(error, error_size,
                  "could not finish reading source directory '%s' (error %lu)",
                  directory, (unsigned long)GetLastError());
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    return 0;
}
#else
static int collect_sources_recursive(const char *directory, ForgeSourceLanguage language,
                                     ForgeSourceList *sources, char *error, size_t error_size)
{
    DIR *stream;
    struct dirent *entry;

    stream = opendir(directory);
    if (stream == NULL) {
        forge_util_set_error(error, error_size,
                  "could not read source directory '%s': %s", directory, strerror(errno));
        return -1;
    }
    while ((entry = readdir(stream)) != NULL) {
        char path[FORGE_PATH_MAX];
        struct stat details;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (forge_paths_join(path, sizeof(path), directory, entry->d_name) != 0) {
            forge_util_set_error(error, error_size,
                                 "source path is too long under '%s'", directory);
            (void)closedir(stream);
            return -1;
        }
        if (stat(path, &details) != 0) {
            forge_util_set_error(error, error_size,
                                 "could not inspect source path '%s': %s", path,
                                 strerror(errno));
            (void)closedir(stream);
            return -1;
        }
        if (S_ISDIR(details.st_mode)) {
            if (collect_sources_recursive(path, language, sources, error, error_size) != 0) {
                (void)closedir(stream);
                return -1;
            }
        } else if (S_ISREG(details.st_mode) && source_matches_language(path, language) &&
                   source_list_add(sources, path, language, error, error_size) != 0) {
            (void)closedir(stream);
            return -1;
        }
    }
    (void)closedir(stream);
    return 0;
}
#endif

int forge_sources_collect(const char *project_root, const ForgeManifest *manifest,
                          ForgeSourceList *sources, char *error, size_t error_size)
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

    if (project_root == NULL || manifest == NULL || sources == NULL ||
        error == NULL || error_size == 0U) {
        return -1;
    }
    *sources = (ForgeSourceList){0};
    for (list_index = 0U; list_index < sizeof(lists) / sizeof(lists[0]); ++list_index) {
        size_t directory_index;
        for (directory_index = 0U; directory_index < lists[list_index]->count;
             ++directory_index) {
            char resolved[FORGE_PATH_MAX];
            if (forge_paths_resolve(project_root, lists[list_index]->items[directory_index],
                                    resolved, sizeof(resolved)) != 0) {
                forge_util_set_error(error, error_size,
                          "source directory path is too long: %s",
                          lists[list_index]->items[directory_index]);
                forge_sources_free(sources);
                return -1;
            }
            if (collect_sources_recursive(resolved, languages[list_index], sources,
                                          error, error_size) != 0) {
                forge_sources_free(sources);
                return -1;
            }
        }
    }
    if (sources->count == 0U) {
        forge_util_set_error(error, error_size,
                  "the manifest source directories contain no C, C++, or assembly files");
        return -1;
    }
    return 0;
}

void forge_sources_free(ForgeSourceList *sources)
{
    free(sources->items);
    *sources = (ForgeSourceList){0};
}