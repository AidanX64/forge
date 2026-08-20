#ifndef FORGE_UTIL_H
#define FORGE_UTIL_H

#include <stddef.h>

/* Fills `error` (if there is room) with a formatted message. */
void forge_util_set_error(char *error, size_t error_size, const char *format, ...);

/* Strips leading and trailing whitespace from *text in place, returning text. */
char *forge_util_trim(char *text);

/* Returns 1 if `text` ends with `suffix`. */
int forge_util_has_suffix(const char *text, const char *suffix);

/*
 * Returns 1 if `program` is invocable from PATH, 0 otherwise. Uses a real PATH
 * sweep rather than a shell so no command interpreter is ever consulted.
 */
int forge_util_program_available(const char *program);

#endif