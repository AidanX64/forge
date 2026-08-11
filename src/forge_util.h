#ifndef FORGE_UTIL_H
#define FORGE_UTIL_H

#include <stddef.h>

/* Fills `error` (if there is room) with a formatted message. */
void forge_util_set_error(char *error, size_t error_size, const char *format, ...);

/* Strips leading and trailing whitespace from *text in place, returning text. */
char *forge_util_trim(char *text);

/* Returns 1 if `program` is invocable from PATH, 0 otherwise. */
int forge_util_program_available(const char *program);

/*
 * Returns 1 if `text` contains characters that are dangerous even inside a
 * double-quoted string on both Windows cmd and POSIX sh. Forge shells out via
 * system(), so manifest/env/filesystem-provided strings with these characters
 * must be rejected instead of being trusted.
 */
int forge_util_has_shell_unsafe_chars(const char *text);

#endif