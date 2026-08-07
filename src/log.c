#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <share.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "forge/log.h"

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || error_size == 0U) {
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

static int create_directory(const char *path, char *error, size_t error_size)
{
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        set_error(error, error_size, "could not create output directory '%s' (error %lu)",
                  path, (unsigned long)GetLastError());
        return -1;
    }
#else
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        set_error(error, error_size, "could not create output directory '%s': %s",
                  path, strerror(errno));
        return -1;
    }
#endif
    return 0;
}

static int create_log_directory(const char *root, char *error, size_t error_size)
{
    char path[FORGE_LOG_PATH_MAX];

    if (snprintf(path, sizeof(path), "%s/target", root) < 0 ||
        (size_t)snprintf(path, sizeof(path), "%s/target", root) >= sizeof(path)) {
        set_error(error, error_size, "project root path is too long");
        return -1;
    }
    if (create_directory(path, error, error_size) != 0) {
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/target/logs", root) < 0 ||
        (size_t)snprintf(path, sizeof(path), "%s/target/logs", root) >= sizeof(path)) {
        set_error(error, error_size, "project root path is too long");
        return -1;
    }
    return create_directory(path, error, error_size);
}

static FILE *open_log_for_write(const char *path)
{
#ifdef _WIN32
    return _fsopen(path, "w", _SH_DENYNO);
#else
    return fopen(path, "w");
#endif
}

static FILE *open_log_for_append(const char *path)
{
#ifdef _WIN32
    return _fsopen(path, "a", _SH_DENYNO);
#else
    return fopen(path, "a");
#endif
}

static void timestamp(char *value, size_t value_size)
{
#ifdef _WIN32
    SYSTEMTIME now;
    GetLocalTime(&now);
    (void)snprintf(value, value_size, "%04u%02u%02u-%02u%02u%02u-%03u",
                   (unsigned int)now.wYear, (unsigned int)now.wMonth,
                   (unsigned int)now.wDay, (unsigned int)now.wHour,
                   (unsigned int)now.wMinute, (unsigned int)now.wSecond,
                   (unsigned int)now.wMilliseconds);
#else
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if (local == NULL) {
        (void)snprintf(value, value_size, "unknown-time");
        return;
    }
    (void)strftime(value, value_size, "%Y%m%d-%H%M%S", local);
#endif
}

int forge_logger_init_in(ForgeLogger *logger, const char *project_root,
                         const char *kind, char *error, size_t error_size)
{
    char stamp[64];
    unsigned int suffix;

    if (logger == NULL || kind == NULL || kind[0] == '\0' ||
        project_root == NULL || project_root[0] == '\0') {
        set_error(error, error_size, "logger, project root, and log kind are required");
        return -1;
    }
    *logger = (ForgeLogger){0};
    if (create_log_directory(project_root, error, error_size) != 0) {
        return -1;
    }
    timestamp(stamp, sizeof(stamp));
    for (suffix = 0U; suffix < 1000U; ++suffix) {
        int written;
        if (suffix == 0U) {
            written = snprintf(logger->path, sizeof(logger->path),
                               "%s/target/logs/%s-%s.log", project_root, kind, stamp);
        } else {
            written = snprintf(logger->path, sizeof(logger->path),
                               "%s/target/logs/%s-%s-%u.log",
                               project_root, kind, stamp, suffix);
        }
        if (written < 0 || (size_t)written >= sizeof(logger->path)) {
            set_error(error, error_size, "log path is too long");
            return -1;
        }
        logger->file = fopen(logger->path, "r");
        if (logger->file == NULL) {
            logger->file = open_log_for_write(logger->path);
            if (logger->file == NULL) {
                set_error(error, error_size, "could not create log '%s': %s",
                          logger->path, strerror(errno));
                return -1;
            }
            return 0;
        }
        (void)fclose(logger->file);
        logger->file = NULL;
    }
    set_error(error, error_size, "could not reserve a unique log file name");
    return -1;
}

int forge_logger_init(ForgeLogger *logger, const char *kind,
                      char *error, size_t error_size)
{
    return forge_logger_init_in(logger, ".", kind, error, error_size);
}

static void write_log(ForgeLogger *logger, FILE *stream, const char *level,
                      const char *stage, const char *format, va_list arguments)
{
    va_list copy;

    (void)fprintf(stream, "[%s] [%s] ", level, stage);
    va_copy(copy, arguments);
    (void)vfprintf(stream, format, copy);
    va_end(copy);
    fputc('\n', stream);
    (void)fflush(stream);

    if (logger != NULL && logger->file != NULL) {
        (void)fprintf(logger->file, "[%s] [%s] ", level, stage);
        (void)vfprintf(logger->file, format, arguments);
        fputc('\n', logger->file);
        (void)fflush(logger->file);
    }
}

void forge_logger_log(ForgeLogger *logger, const char *stage,
                      const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    write_log(logger, stdout, "INFO", stage, format, arguments);
    va_end(arguments);
}

void forge_logger_error(ForgeLogger *logger, const char *stage,
                        const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    write_log(logger, stderr, "ERROR", stage, format, arguments);
    va_end(arguments);
}

void forge_logger_close(ForgeLogger *logger)
{
    if (logger != NULL && logger->file != NULL) {
        (void)fclose(logger->file);
        logger->file = NULL;
    }
}

int forge_logger_suspend(ForgeLogger *logger, char *error, size_t error_size)
{
    if (logger == NULL || logger->file == NULL) {
        return 0;
    }
    if (fflush(logger->file) != 0 || fclose(logger->file) != 0) {
        logger->file = NULL;
        set_error(error, error_size, "could not close log '%s': %s",
                  logger->path, strerror(errno));
        return -1;
    }
    logger->file = NULL;
    return 0;
}

int forge_logger_resume(ForgeLogger *logger, char *error, size_t error_size)
{
    if (logger == NULL || logger->file != NULL) {
        return 0;
    }
    logger->file = open_log_for_append(logger->path);
    if (logger->file == NULL) {
        set_error(error, error_size, "could not reopen log '%s': %s",
                  logger->path, strerror(errno));
        return -1;
    }
    return 0;
}
