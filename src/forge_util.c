#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/compiler.h"
#include "forge/platform.h"
#include "forge_util.h"

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

int forge_util_program_available(const char *program)
{
    char command[FORGE_COMPILER_VALUE_MAX * 2U];
    int written;

#if FORGE_PLATFORM_WINDOWS
    written = snprintf(command, sizeof(command), "where \"%s\" >NUL 2>&1", program);
#else
    written = snprintf(command, sizeof(command), "command -v \"%s\" >/dev/null 2>&1",
                       program);
#endif
    return written >= 0 && (size_t)written < sizeof(command) && system(command) == 0;
}

int forge_util_has_shell_unsafe_chars(const char *text)
{
    const char *cursor;

    if (text == NULL) {
        return 1;
    }
    for (cursor = text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"':
        case '`':
        case '$':
        case '!':
        case '\n':
        case '\r':
            return 1;
        default:
            break;
        }
    }
    return 0;
}