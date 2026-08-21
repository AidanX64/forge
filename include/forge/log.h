#ifndef FORGE_LOG_H
#define FORGE_LOG_H

#include <stddef.h>
#include <stdio.h>

#define FORGE_LOG_PATH_MAX 1024U

typedef struct ForgeLogger {
    FILE *file;
    char path[FORGE_LOG_PATH_MAX];
} ForgeLogger;

/*
 * Creates <project_root>/target/logs/<kind>-<timestamp>.log. The logs directory
 * is placed inside the project's own target directory so output is anchored to
 * the manifest location rather than the invoking process's working directory.
 */
int forge_logger_init_in(ForgeLogger *logger, const char *project_root,
                         const char *kind, char *error, size_t error_size);

void forge_logger_close(ForgeLogger *logger);
void forge_logger_log(ForgeLogger *logger, const char *stage,
                      const char *format, ...);
void forge_logger_error(ForgeLogger *logger, const char *stage,
                        const char *format, ...);

#endif
