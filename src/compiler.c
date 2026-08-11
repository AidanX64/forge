#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/platform.h"

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/*
 * RtlGetVersion is declared in winternl.h on some SDKs but not all (it is
 * missing from the 10.0.26100 Windows SDK). Resolve it at runtime from ntdll
 * instead, so no extra link dependency is introduced for host OS detection.
 */
typedef struct ForgeRtlOsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} ForgeRtlOsVersionInfo;
typedef LONG (WINAPI *ForgeRtlGetVersionFn)(ForgeRtlOsVersionInfo *);

static LONG rtl_get_version(ForgeRtlOsVersionInfo *version)
{
    HMODULE ntdll;
    ForgeRtlGetVersionFn get_version;

    if (version == NULL) {
        return -1;
    }
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        return -1;
    }
    get_version = (ForgeRtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion");
    if (get_version == NULL) {
        return -1;
    }
    return get_version(version);
}
#elif defined(__linux__)
#include <sys/utsname.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#include "forge/compiler.h"
#include "forge_util.h"

static int command_append(char *command, size_t command_size, size_t *length,
                          const char *format, ...)
{
    va_list arguments;
    int written;

    if (*length >= command_size) {
        return -1;
    }
    va_start(arguments, format);
    written = vsnprintf(command + *length, command_size - *length, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= command_size - *length) {
        return -1;
    }
    *length += (size_t)written;
    return 0;
}

static ForgeCompilerKind compiler_kind_from_program(const char *program)
{
    if (strstr(program, "clang") != NULL) {
        return FORGE_COMPILER_CLANG;
    }
    if (strstr(program, "cl") != NULL && strstr(program, "clang") == NULL) {
        return FORGE_COMPILER_MSVC;
    }
    return FORGE_COMPILER_GCC;
}

static int has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

const char *forge_host_os_name(ForgeHostOs os)
{
    switch (os) {
    case FORGE_HOST_OS_WINDOWS:
        return "windows";
    case FORGE_HOST_OS_LINUX:
        return "linux";
    case FORGE_HOST_OS_MACOS:
        return "macos";
    case FORGE_HOST_OS_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

const char *forge_compiler_kind_name(ForgeCompilerKind kind)
{
    switch (kind) {
    case FORGE_COMPILER_MSVC:
        return "msvc";
    case FORGE_COMPILER_GCC:
        return "gcc";
    case FORGE_COMPILER_CLANG:
        return "clang";
    }
    return "unknown";
}

int forge_detect_host(ForgeHostInfo *host, char *error, size_t error_size)
{
    if (host == NULL) {
        forge_util_set_error(error, error_size, "host output is required");
        return -1;
    }
    *host = (ForgeHostInfo){0};

#if FORGE_PLATFORM_WINDOWS
    {
        ForgeRtlOsVersionInfo version = {0};
        version.dwOSVersionInfoSize = sizeof(version);
        host->os = FORGE_HOST_OS_WINDOWS;
        (void)snprintf(host->os_name, sizeof(host->os_name), "windows");
        /*
         * GetVersionExA is deprecated and reports a skewed Windows 10/11
         * build number unless the binary embeds a compatibility manifest.
         * RtlGetVersion returns the real OS version and is not deprecated.
         */
        if (rtl_get_version(&version) != 0 || version.dwMajorVersion == 0U) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        } else {
            (void)snprintf(host->version, sizeof(host->version), "%lu.%lu.%lu",
                           (unsigned long)version.dwMajorVersion,
                           (unsigned long)version.dwMinorVersion,
                           (unsigned long)version.dwBuildNumber);
        }
    }
#elif defined(__linux__)
    {
        struct utsname details;
        host->os = FORGE_HOST_OS_LINUX;
        (void)snprintf(host->os_name, sizeof(host->os_name), "linux");
        if (uname(&details) != 0) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        } else {
            (void)snprintf(host->version, sizeof(host->version), "%s", details.release);
        }
    }
#elif defined(__APPLE__)
    {
        size_t version_size = sizeof(host->version);
        host->os = FORGE_HOST_OS_MACOS;
        (void)snprintf(host->os_name, sizeof(host->os_name), "macos");
        /*
         * sysctlbyname is deprecated in macOS 12+ and would otherwise trip
         * -Werror. Suppress the warning just for this lookup; it still works.
         */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        if (sysctlbyname("kern.osproductversion", host->version, &version_size,
                         NULL, 0U) != 0) {
            (void)snprintf(host->version, sizeof(host->version), "unknown");
        }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    }
#else
    host->os = FORGE_HOST_OS_UNKNOWN;
    (void)snprintf(host->os_name, sizeof(host->os_name), "unknown");
    (void)snprintf(host->version, sizeof(host->version), "unknown");
#endif
    return 0;
}

static int select_program(const char *program, ForgeCompilerKind kind, int fallback,
                          const char *note, ForgeCompiler *compiler,
                          char *error, size_t error_size)
{
    if (!forge_util_program_available(program)) {
        forge_util_set_error(error, error_size, "%s; compiler '%s' is not available on PATH",
                  note, program);
        return -1;
    }
    compiler->kind = kind;
    compiler->used_fallback = fallback;
    (void)snprintf(compiler->program, sizeof(compiler->program), "%s", program);
    (void)snprintf(compiler->selection_note, sizeof(compiler->selection_note), "%s",
                   note);
    return 0;
}

/*
 * MSVC's cl.exe is only usable when the Visual Studio environment has been
 * initialized (INCLUDE/LIB variables must point at the SDK and CRT headers).
 * A bare cl.exe on PATH will error with C1083 on every system header, so
 * treat it as unavailable unless its environment is actually set up.
 */
static int msvc_environment_ready(void)
{
#if FORGE_PLATFORM_WINDOWS
    const char *include = getenv("INCLUDE");
    return include != NULL && include[0] != '\0';
#else
    return 0;
#endif
}

int forge_compiler_select(const ForgeHostInfo *host, const char *override_program,
                          ForgeCompiler *compiler, char *error, size_t error_size)
{
    if (host == NULL || compiler == NULL) {
        forge_util_set_error(error, error_size, "host and compiler output are required");
        return -1;
    }
    *compiler = (ForgeCompiler){0};

    if (override_program != NULL && override_program[0] != '\0') {
        if (forge_util_has_shell_unsafe_chars(override_program)) {
            forge_util_set_error(error, error_size,
                                 "compiler override contains characters unsafe for shell invocation");
            return -1;
        }
        return select_program(override_program, compiler_kind_from_program(override_program),
                              0, "using manifest compiler override", compiler,
                              error, error_size);
    }

    switch (host->os) {
    case FORGE_HOST_OS_WINDOWS:
        if (msvc_environment_ready() && forge_util_program_available("cl")) {
            return select_program("cl", FORGE_COMPILER_MSVC, 0,
                                  "using Windows native compiler", compiler,
                                  error, error_size);
        }
        if (forge_util_program_available("gcc")) {
            return select_program("gcc", FORGE_COMPILER_GCC, 1,
                                  "MSVC was not usable; using gcc fallback", compiler,
                                  error, error_size);
        }
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "MSVC and gcc were not usable; using clang fallback",
                              compiler, error, error_size);
    case FORGE_HOST_OS_LINUX:
        if (forge_util_program_available("gcc")) {
            return select_program("gcc", FORGE_COMPILER_GCC, 0,
                                  "using Linux native compiler", compiler,
                                  error, error_size);
        }
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "gcc was not found; using clang fallback", compiler,
                              error, error_size);
    case FORGE_HOST_OS_MACOS:
        return select_program("clang", FORGE_COMPILER_CLANG, 0,
                              "using macOS native compiler", compiler,
                              error, error_size);
    case FORGE_HOST_OS_UNKNOWN:
        return select_program("clang", FORGE_COMPILER_CLANG, 1,
                              "unsupported host OS; explicitly using clang fallback",
                              compiler, error, error_size);
    }

    forge_util_set_error(error, error_size, "invalid detected host OS");
    return -1;
}

static const char *cpp_driver(const ForgeCompiler *compiler)
{
    if (compiler->kind == FORGE_COMPILER_GCC && strcmp(compiler->program, "gcc") == 0) {
        return "g++";
    }
    if (compiler->kind == FORGE_COMPILER_CLANG &&
        strcmp(compiler->program, "clang") == 0) {
        return "clang++";
    }
    return compiler->program;
}

int forge_compiler_make_compile_command(const ForgeCompiler *compiler,
                                        ForgeSourceLanguage language,
                                        const char *source_path,
                                        const char *object_path,
                                        const char *project_root,
                                        const char *target_os,
                                        const char *target_arch,
                                        const char *const *flags, size_t flag_count,
                                        char *command, size_t command_size,
                                        char *error, size_t error_size)
{
    const char *program;
    size_t index;
    size_t length = 0U;

    if (compiler == NULL || source_path == NULL || object_path == NULL ||
        project_root == NULL || target_os == NULL || target_arch == NULL ||
        command == NULL || command_size == 0U) {
        forge_util_set_error(error, error_size, "compiler, paths, target, and command output are required");
        return -1;
    }
    if (source_path[0] == '\0' || object_path[0] == '\0' || project_root[0] == '\0' ||
        target_os[0] == '\0' || target_arch[0] == '\0') {
        forge_util_set_error(error, error_size, "source, object, project root, target OS, and target architecture are required");
        return -1;
    }
    command[0] = '\0';

    if (forge_util_has_shell_unsafe_chars(compiler->program) ||
        forge_util_has_shell_unsafe_chars(project_root) ||
        forge_util_has_shell_unsafe_chars(source_path) ||
        forge_util_has_shell_unsafe_chars(object_path)) {
        forge_util_set_error(error, error_size,
                             "compiler or paths contain characters unsafe for shell invocation");
        return -1;
    }

    if (compiler->kind == FORGE_COMPILER_MSVC && language == FORGE_SOURCE_ASM) {
        program = strcmp(target_arch, "x86_64") == 0 ? "ml64" : "ml";
        if (!forge_util_program_available(program)) {
            forge_util_set_error(error, error_size, "MSVC assembly requires '%s' on PATH", program);
            return -1;
        }
        if (command_append(command, command_size, &length,
                           "%s /nologo /c /Fo\"%s\" \"%s\"",
                           program, object_path, source_path) != 0) {
            forge_util_set_error(error, error_size, "compile command is too long");
            return -1;
        }
    } else if (language == FORGE_SOURCE_ASM && has_suffix(source_path, ".asm")) {
        /*
         * GCC/Clang drive the source with their own assembler, which
         * understands .s/.S but not the MSVC-flavoured .asm dialect.
         */
        forge_util_set_error(error, error_size,
                             "the '%s' compiler cannot assemble '.asm' files; "
                             "use .s/.S with GCC/Clang, or the MSVC toolchain for .asm",
                             compiler->program);
        return -1;
    } else if (compiler->kind == FORGE_COMPILER_MSVC) {
        if (command_append(command, command_size, &length,
                           "%s /nologo /I\"%s/include\" /c /Fo\"%s\" \"%s\"",
                           compiler->program, project_root, object_path,
                           source_path) != 0) {
            forge_util_set_error(error, error_size, "compile command is too long");
            return -1;
        }
    } else {
        program = language == FORGE_SOURCE_CPP ? cpp_driver(compiler) : compiler->program;
        if (command_append(command, command_size, &length,
                           "%s -I\"%s/include\" -c \"%s\" -o \"%s\"",
                           program, project_root, source_path, object_path) != 0) {
            forge_util_set_error(error, error_size, "compile command is too long");
            return -1;
        }
    }

    for (index = 0U; index < flag_count; ++index) {
        if (flags == NULL || flags[index] == NULL || flags[index][0] == '\0' ||
            forge_util_has_shell_unsafe_chars(flags[index]) ||
            command_append(command, command_size, &length, " %s", flags[index]) != 0) {
            forge_util_set_error(error, error_size, "invalid or unsafe compiler flags");
            return -1;
        }
    }
    return 0;
}

int forge_compiler_make_link_command(const ForgeCompiler *compiler,
                                     int has_cpp_source,
                                     const char *const *object_paths,
                                     size_t object_count,
                                     const char *output_path,
                                     const char *const *flags, size_t flag_count,
                                     char *command, size_t command_size,
                                     char *error, size_t error_size)
{
    const char *program;
    size_t index;
    size_t length = 0U;

    if (compiler == NULL || object_paths == NULL || object_count == 0U ||
        output_path == NULL || output_path[0] == '\0' ||
        command == NULL || command_size == 0U) {
        forge_util_set_error(error, error_size, "compiler, object files, output, and command output are required");
        return -1;
    }
    command[0] = '\0';
    if (forge_util_has_shell_unsafe_chars(compiler->program) ||
        forge_util_has_shell_unsafe_chars(output_path)) {
        forge_util_set_error(error, error_size,
                             "compiler or output path contains characters unsafe for shell invocation");
        return -1;
    }
    program = has_cpp_source ? cpp_driver(compiler) : compiler->program;
    if (compiler->kind == FORGE_COMPILER_MSVC) {
        program = compiler->program;
        if (command_append(command, command_size, &length, "%s /nologo", program) != 0) {
            forge_util_set_error(error, error_size, "link command is too long");
            return -1;
        }
    } else if (command_append(command, command_size, &length, "%s", program) != 0) {
        forge_util_set_error(error, error_size, "link command is too long");
        return -1;
    }

    for (index = 0U; index < object_count; ++index) {
        if (object_paths[index] == NULL || object_paths[index][0] == '\0' ||
            forge_util_has_shell_unsafe_chars(object_paths[index]) ||
            command_append(command, command_size, &length, " \"%s\"", object_paths[index]) != 0) {
            forge_util_set_error(error, error_size, "invalid or unsafe object file list");
            return -1;
        }
    }
    if (compiler->kind == FORGE_COMPILER_MSVC) {
        if (command_append(command, command_size, &length, " /Fe\"%s\"", output_path) != 0) {
            forge_util_set_error(error, error_size, "link command is too long");
            return -1;
        }
    } else if (command_append(command, command_size, &length, " -o \"%s\"", output_path) != 0) {
        forge_util_set_error(error, error_size, "link command is too long");
        return -1;
    }
    for (index = 0U; index < flag_count; ++index) {
        if (flags == NULL || flags[index] == NULL || flags[index][0] == '\0' ||
            forge_util_has_shell_unsafe_chars(flags[index]) ||
            command_append(command, command_size, &length, " %s", flags[index]) != 0) {
            forge_util_set_error(error, error_size, "invalid or unsafe linker flags");
            return -1;
        }
    }
    return 0;
}
