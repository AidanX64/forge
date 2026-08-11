#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/platform.h"
#include "forge_util.h"

#define FORGE_DEBUG_COMMAND_MAX 8192U
#define FORGE_DEBUG_LINE_MAX 4096U

static int select_debugger(const ForgeHostInfo *host, char *program, size_t program_size,
                           char *error, size_t error_size)
{
    const char *override = getenv("FORGE_DEBUGGER");

    if (override != NULL && override[0] != '\0') {
        if (forge_util_has_shell_unsafe_chars(override)) {
            forge_util_set_error(error, error_size,
                                 "FORGE_DEBUGGER contains characters unsafe for shell invocation");
            return -1;
        }
        if (!forge_util_program_available(override)) {
            forge_util_set_error(error, error_size,
                      "FORGE_DEBUGGER='%s' is not available on PATH", override);
            return -1;
        }
        (void)snprintf(program, program_size, "%s", override);
        return 0;
    }
    if (host->os == FORGE_HOST_OS_WINDOWS && forge_util_program_available("cdb")) {
        (void)snprintf(program, program_size, "cdb");
        return 0;
    }
    if ((host->os == FORGE_HOST_OS_WINDOWS || host->os == FORGE_HOST_OS_LINUX ||
         host->os == FORGE_HOST_OS_UNKNOWN) && forge_util_program_available("gdb")) {
        (void)snprintf(program, program_size, "gdb");
        return 0;
    }
    if ((host->os == FORGE_HOST_OS_MACOS || host->os == FORGE_HOST_OS_UNKNOWN) &&
        forge_util_program_available("lldb")) {
        (void)snprintf(program, program_size, "lldb");
        return 0;
    }
    forge_util_set_error(error, error_size,
              "no supported debugger was found; install cdb, gdb, or lldb, "
              "or set FORGE_DEBUGGER to its executable name");
    return -1;
}

static int make_debug_command(const char *program, const char *executable_path,
                              char *command, size_t command_size,
                              char *error, size_t error_size)
{
    int written;

    if (strcmp(program, "cdb") == 0) {
        written = snprintf(command, command_size,
                           "cdb -lines -c \"g; k; u @rip L20; q\" \"%s\"",
                           executable_path);
    } else if (strcmp(program, "lldb") == 0) {
        written = snprintf(command, command_size,
                           "lldb --batch -o \"run\" -o \"bt\" "
                           "-o \"disassemble -n main\" \"%s\"",
                           executable_path);
    } else {
        written = snprintf(command, command_size,
                           "%s --batch -q -ex \"run\" -ex \"bt\" "
                           "-ex \"disassemble /m main\" \"%s\"",
                           program, executable_path);
    }
    if (written < 0 || (size_t)written >= command_size) {
        forge_util_set_error(error, error_size, "debugger command is too long");
        return -1;
    }
    return 0;
}

static int is_stack_line(const char *line)
{
    return line[0] == '#' || strstr(line, "frame #") != NULL ||
           strstr(line, "Call Site") != NULL || strstr(line, " at ") != NULL;
}

static int is_disassembly_line(const char *line)
{
    return strstr(line, "Dump of assembler") != NULL ||
           strstr(line, "disassembly") != NULL || strstr(line, "Disassembly") != NULL ||
           strncmp(line, "=>", 2U) == 0 ||
           (isxdigit((unsigned char)line[0]) && strstr(line, "<+") != NULL);
}

static void postprocess_debug_output(const char *raw_path, ForgeLogger *logger)
{
    FILE *raw = fopen(raw_path, "r");
    char line[FORGE_DEBUG_LINE_MAX];
    size_t stack_lines = 0U;
    size_t disassembly_lines = 0U;

    if (raw == NULL) {
        forge_logger_error(logger, "debug", "could not read debugger transcript: %s", raw_path);
        return;
    }
    forge_logger_log(logger, "debug", "----- lifter-style stack and disassembly view -----");
    while (fgets(line, sizeof(line), raw) != NULL) {
        char *text = forge_util_trim(line);

        if (is_stack_line(text) && stack_lines < 64U) {
            forge_logger_log(logger, "stack", "%s", text);
            ++stack_lines;
        } else if (is_disassembly_line(text) && disassembly_lines < 64U) {
            forge_logger_log(logger, "disassembly", "%s", text);
            ++disassembly_lines;
        }
    }
    if (stack_lines == 0U && disassembly_lines == 0U) {
        forge_logger_log(logger, "debug",
                         "no stack or disassembly lines were recognized; "
                         "raw debugger transcript retained at %s", raw_path);
    }
    (void)fclose(raw);
}

int forge_debug_launch(const char *executable_path, ForgeLogger *logger,
                       char *error, size_t error_size)
{
    ForgeHostInfo host;
    char debugger[FORGE_COMPILER_VALUE_MAX];
    char command[FORGE_DEBUG_COMMAND_MAX];
    char raw_path[FORGE_LOG_PATH_MAX + 8U];
    char captured_command[FORGE_DEBUG_COMMAND_MAX + FORGE_LOG_PATH_MAX + 32U];
    int written;

    if (executable_path == NULL || executable_path[0] == '\0' || logger == NULL) {
        forge_util_set_error(error, error_size, "executable path and active logger are required");
        return -1;
    }
    if (forge_detect_host(&host, error, error_size) != 0 ||
        select_debugger(&host, debugger, sizeof(debugger), error, error_size) != 0 ||
        make_debug_command(debugger, executable_path, command, sizeof(command),
                           error, error_size) != 0) {
        return -1;
    }
    written = snprintf(raw_path, sizeof(raw_path), "%s.raw", logger->path);
    if (written < 0 || (size_t)written >= sizeof(raw_path)) {
        forge_util_set_error(error, error_size, "raw debugger transcript path is too long");
        return -1;
    }
    forge_logger_log(logger, "debug", "debugger=%s executable=%s", debugger, executable_path);
    forge_logger_log(logger, "debug", "command: %s", command);
    written = snprintf(captured_command, sizeof(captured_command),
                       "%s > \"%s\" 2>&1", command, raw_path);
    if (written < 0 || (size_t)written >= sizeof(captured_command)) {
        forge_util_set_error(error, error_size, "debugger command is too long to capture");
        return -1;
    }
    if (system(captured_command) != 0) {
        forge_logger_error(logger, "debug",
                           "debugger exited with an error; processing its transcript");
    }
    postprocess_debug_output(raw_path, logger);
    return 0;
}
