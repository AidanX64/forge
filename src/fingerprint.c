#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <string.h>

#include "forge/fingerprint.h"
#include "forge/compiler.h"
#include "forge/platform.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/*
 * Stable FNV-1a hash of a source path. Object files are named after this hash
 * instead of their compile index, so adding/removing/reordering sources never
 * makes an object file "belong" to a different source than the one stored in
 * it (which an index-based scheme would allow once we skip fresh files).
 */
static unsigned int source_hash(const char *text)
{
    unsigned int hash = 2166136261U;

    while (*text != '\0') {
        hash ^= (unsigned char)*text++;
        hash *= 16777619U;
    }
    return hash;
}

/*
 * Returns 1 when `path` is missing or its mtime is strictly newer than the
 * object file, so the object is stale with respect to that dependency. A
 * missing dependency is treated as stale (conservative): the recompile that
 * follows either fails loudly if the file is still needed, or regenerates a
 * correct dependency list if it is not.
 */
static int dependency_is_stale(const char *path, const char *object_path)
{
#if FORGE_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA path_data;
    WIN32_FILE_ATTRIBUTE_DATA object_data;

    if (GetFileAttributesExA(path, GetFileExInfoStandard, &path_data) == 0 ||
        GetFileAttributesExA(object_path, GetFileExInfoStandard, &object_data) == 0) {
        return 1;
    }
    return CompareFileTime(&path_data.ftLastWriteTime, &object_data.ftLastWriteTime) > 0;
#else
    struct stat path_stat;
    struct stat object_stat;

    if (stat(path, &path_stat) != 0 || stat(object_path, &object_stat) != 0) {
        return 1;
    }
    return path_stat.st_mtime > object_stat.st_mtime;
#endif
}

/*
 * Walks a gcc/clang dependency file (Makefile syntax: "target: dep dep \"
 * "...") and returns 1 if any listed file is stale relative to the object.
 * The paths are what the compiler itself resolved during compilation, so this
 * tracks exactly the headers each translation unit used, including ones found
 * through user-provided -I flags.
 */
static int depfile_has_stale_dependency(FILE *stream, const char *object_path)
{
    char token[FORGE_PATH_MAX];
    size_t token_length = 0U;
    int character;

    while ((character = fgetc(stream)) != EOF) {
        if (character == ':') {
            /*
             * The target/rule separator is the colon on the target line, which
             * is followed by a space or a continuation backslash then a newline
             * (e.g. `C:\...\obj.o: \` or `obj.o: dep`). A drive-letter colon
             * (`C:\...`) is followed by more path text, so reading one char
             * ahead tells the two apart; this matters because GCC/Clang emit
             * drive letters here on Windows.
             */
            int next = fgetc(stream);
            int separator = 0;

            if (next == ' ' || next == '\t' || next == '\r' ||
                next == '\n' || next == EOF) {
                separator = 1;
            } else if (next == '\\') {
                int after_backslash = fgetc(stream);
                if (after_backslash == '\r') {
                    after_backslash = fgetc(stream);
                }
                if (after_backslash == '\n') {
                    separator = 1;
                } else if (after_backslash != EOF) {
                    (void)ungetc(after_backslash, stream);
                    (void)ungetc(next, stream);
                    if (token_length + 1U < sizeof(token)) {
                        token[token_length++] = (char)character;
                    }
                    continue;
                }
            } else {
                (void)ungetc(next, stream);
                if (token_length + 1U < sizeof(token)) {
                    token[token_length++] = (char)character;
                }
                continue;
            }
            /* Target/rule separator colon: skip it; the dependencies follow. */
            if (separator) {
                continue;
            }
        }
        /*
         * A backslash at a line end is make's continuation marker; treat it as
         * a separator rather than glue so a depfile without a space before the
         * backslash never joins two paths into one bogus token.
         */
        if (character == '\\') {
            int next = fgetc(stream);
            if (next == '\r') {
                next = fgetc(stream);
            }
            if (next == '\n') {
                if (token_length != 0U) {
                    token[token_length] = '\0';
                    if (dependency_is_stale(token, object_path)) {
                        return 1;
                    }
                    token_length = 0U;
                }
                continue;
            }
            if (next != EOF && token_length + 1U < sizeof(token)) {
                token[token_length++] = (char)character;
            }
            if (next != EOF && ungetc(next, stream) == EOF) {
                return 0;
            }
            continue;
        }
        if (character == ' ' || character == '\t' ||
            character == '\r' || character == '\n') {
            if (token_length != 0U) {
                token[token_length] = '\0';
                if (dependency_is_stale(token, object_path)) {
                    return 1;
                }
                token_length = 0U;
            }
            continue;
        }
        if (token_length + 1U < sizeof(token)) {
            token[token_length++] = (char)character;
        }
    }
    if (token_length != 0U) {
        token[token_length] = '\0';
        return dependency_is_stale(token, object_path);
    }
    return 0;
}

int forge_fingerprint_object_fresh(const char *object_path, const char *source_path,
                                   int track_headers)
{
    char depfile_path[FORGE_PATH_MAX];
    FILE *stream;
    int fresh;

#if FORGE_PLATFORM_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA object_data;
    WIN32_FILE_ATTRIBUTE_DATA source_data;

    if (GetFileAttributesExA(object_path, GetFileExInfoStandard, &object_data) == 0 ||
        GetFileAttributesExA(source_path, GetFileExInfoStandard, &source_data) == 0) {
        return 0;
    }
    if (CompareFileTime(&object_data.ftLastWriteTime, &source_data.ftLastWriteTime) < 0) {
        return 0;
    }
#else
    struct stat object_stat;
    struct stat source_stat;

    if (stat(object_path, &object_stat) != 0 || stat(source_path, &source_stat) != 0) {
        return 0;
    }
    if (object_stat.st_mtime < source_stat.st_mtime) {
        return 0;
    }
#endif
    if (!track_headers) {
        return 1;
    }
    if (forge_compiler_depfile_path(object_path, depfile_path,
                                    sizeof(depfile_path)) != 0) {
        return 0;
    }
    stream = fopen(depfile_path, "rb");
    if (stream == NULL) {
        return 0;
    }
    fresh = !depfile_has_stale_dependency(stream, object_path);
    (void)fclose(stream);
    return fresh;
}

int forge_fingerprint_object_name(const char *object_directory, const char *source_path,
                                  char (*used)[FORGE_PATH_MAX], size_t used_count,
                                  char *object_path, size_t object_path_size)
{
    unsigned int hash = source_hash(source_path);
    unsigned int attempt = 0U;
    size_t index;

    for (;;) {
        char name[FORGE_PATH_MAX];
        int written;

        if (attempt == 0U) {
            written = snprintf(name, sizeof(name), "%08x.o", hash);
        } else {
            written = snprintf(name, sizeof(name), "%08x-%u.o", hash, attempt);
        }
        if (written < 0 || (size_t)written >= sizeof(name) ||
            snprintf(object_path, object_path_size, "%s/%s", object_directory, name) < 0 ||
            strlen(object_directory) + strlen(name) + 1U >= object_path_size) {
            return -1;
        }
        for (index = 0U; index < used_count; ++index) {
            if (strcmp(used[index], object_path) == 0) {
                break;
            }
        }
        if (index == used_count) {
            break;
        }
        ++attempt;
        if (attempt > 256U) {
            return -1;
        }
    }
    return 0;
}