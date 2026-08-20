#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/platform.h"
#include "forge_util.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define FORGE_UTIL_PATH_MAX 1024U

void forge_util_set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

char *forge_util_trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

int forge_util_has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int regular_file_exists(const char *path)
{
#if FORGE_PLATFORM_WINDOWS
    DWORD attributes = GetFileAttributesA(path);

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U;
#else
    struct stat details;

    if (stat(path, &details) != 0 || !S_ISREG(details.st_mode)) {
        return 0;
    }
    return access(path, X_OK) == 0;
#endif
}

/*
 * Sweeps PATH looking for a runnable `program`. A name that carries a
 * directory component is checked directly; otherwise each PATH entry is tried,
 * appending the host's executable suffix (.exe) on Windows.
 */
int forge_util_program_available(const char *program)
{
    char candidate[FORGE_UTIL_PATH_MAX];
    const char *path_value;
    const char *separators;
    size_t length;

    if (program == NULL || program[0] == '\0') {
        return 0;
    }
#if FORGE_PLATFORM_WINDOWS
    separators = ";";
#else
    separators = ":";
#endif
    if (strchr(program, '/') != NULL || program[0] == '.' ||
        (program[1] == ':' && (program[2] == '/' || program[2] == '\\')) ||
        program[0] == '\\') {
        return regular_file_exists(program);
    }
    path_value = getenv("PATH");
    if (path_value == NULL) {
        return 0;
    }
    for (;;) {
        const char *end = strpbrk(path_value, separators);
        int needs_separator;

        if (end == NULL) {
            end = path_value + strlen(path_value);
        }
        if (end == path_value) {
            if (*end == '\0') {
                return 0;
            }
            path_value = end + 1;
            continue;
        }
        length = (size_t)(end - path_value);
        if (length >= sizeof(candidate)) {
            length = sizeof(candidate) - 1U;
        }
        (void)memcpy(candidate, path_value, length);
        candidate[length] = '\0';
        needs_separator = candidate[length - 1U] != '/' &&
                          candidate[length - 1U] != '\\';
        if (snprintf(candidate + length, sizeof(candidate) - length,
                     "%s%s", needs_separator ? "/" : "", program) >= 0 &&
            regular_file_exists(candidate)) {
            return 1;
        }
#if FORGE_PLATFORM_WINDOWS
        if (snprintf(candidate + length, sizeof(candidate) - length,
                     "%s%s.exe", needs_separator ? "/" : "", program) >= 0 &&
            regular_file_exists(candidate)) {
            return 1;
        }
#endif
        if (*end == '\0') {
            return 0;
        }
        path_value = end + 1;
    }
}