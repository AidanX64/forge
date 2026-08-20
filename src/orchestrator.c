#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FORGE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "forge/argv.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/build.h"
#include "forge/fingerprint.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"
#include "forge/paths.h"
#include "forge/process.h"
#include "forge/sources.h"
#include "forge/thread.h"
#include "forge_util.h"

static ForgeLogger *active_logger;

static const char *log_capture_path(void)
{
    return (active_logger != NULL && active_logger->path[0] != '\0')
               ? active_logger->path
               : NULL;
}

static void print_error(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    if (active_logger != NULL) {
        char message[FORGE_COMMAND_MAX];
        (void)vsnprintf(message, sizeof(message), format, arguments);
        forge_logger_error(active_logger, "build", "%s", message);
    } else {
        fprintf(stderr, "forge: ");
        (void)vfprintf(stderr, format, arguments);
        fputc('\n', stderr);
    }
    va_end(arguments);
}

/*
 * Emits the tail of the invocation log to stderr. Stage output is appended to
 * the log file during a build, so on a failed stage the terminal should show
 * the real diagnostic instead of only a terse "failed" line.
 */
static void print_log_tail(size_t max_lines)
{
    FILE *file;
    char *contents;
    char *start;
    char *cursor;
    size_t size;
    size_t lines = 0U;

    if (active_logger == NULL || active_logger->path[0] == '\0') {
        return;
    }
    file = fopen(active_logger->path, "rb");
    if (file == NULL) {
        fprintf(stderr, "forge: [error] could not read the build log at %s\n",
                active_logger->path);
        return;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        return;
    }
    {
        long length = ftell(file);
        if (length <= 0L) {
            (void)fclose(file);
            return;
        }
        contents = malloc((size_t)length + 1U);
        if (contents == NULL) {
            (void)fclose(file);
            return;
        }
        rewind(file);
        size = fread(contents, 1U, (size_t)length, file);
        (void)fclose(file);
        contents[size] = '\0';
    }
    cursor = contents + size;
    while (cursor > contents && lines < max_lines) {
        char *previous = cursor - 1;
        while (previous > contents && previous[-1] != '\n') {
            --previous;
        }
        if (previous == contents) {
            start = previous;
            ++lines;
            break;
        }
        start = previous;
        ++lines;
        cursor = previous - 1;
    }
    fprintf(stderr, "forge: [error] last %zu log line(s):\n", lines);
    {
        char *line = start;
        while (line != NULL && line < contents + size) {
            char *end = strchr(line, '\n');
            size_t length = end == NULL ? (size_t)(contents + size - line)
                                        : (size_t)(end - line);
            while (length > 0U && (line[length - 1U] == '\r' ||
                                   line[length - 1U] == '\n')) {
                --length;
            }
            fprintf(stderr, "  %.*s\n", (int)length, line);
            line = end == NULL ? NULL : end + 1;
        }
    }
    free(contents);
}

int forge_build_project_root(const char *manifest_path, char *root, size_t root_size)
{
    char error[FORGE_COMMAND_MAX] = {0};
    int status = forge_paths_project_root(manifest_path, root, root_size,
                                          error, sizeof(error));

    if (status != 0 && error[0] != '\0') {
        print_error("%s", error);
    }
    return status;
}

static const char *host_architecture(void)
{
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "aarch64";
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

static int list_contains(const ForgeStringList *list, const char *value)
{
    size_t index;
    for (index = 0U; index < list->count; ++index) {
        if (strcmp(list->items[index], value) == 0) {
            return 1;
        }
    }
    return 0;
}

static int select_host_target(const ForgeManifest *manifest, const ForgeHostInfo *host,
                              const char **target_os, const char **target_arch)
{
    const char *architecture = host_architecture();
    const char *os = forge_host_os_name(host->os);

    if (!list_contains(&manifest->target_os, os) ||
        !list_contains(&manifest->target_arch, architecture)) {
        print_error("manifest does not include this host target (%s/%s); "
                    "cross-compilation needs an explicit toolchain and is not configured",
                    os, architecture);
        return -1;
    }
    *target_os = os;
    *target_arch = architecture;
    return 0;
}

typedef struct ForgeCompileContext {
    const ForgeCompiler *compiler;
    const ForgeBuildProfile *profile;
    const ForgeSourceList *sources;
    char (*object_paths)[FORGE_PATH_MAX];
    const char **object_references;
    const char *project_root;
    const char *target_os;
    const char *target_arch;
    size_t count;
    size_t next;
    int has_cpp_source;
    int failed;
    char error[FORGE_COMMAND_MAX];
    ForgeMutex job_mutex;
    ForgeMutex log_mutex;
} ForgeCompileContext;

static void compile_log(ForgeCompileContext *context, const char *stage,
                        const char *format, ...)
{
    va_list arguments;
    char message[FORGE_COMMAND_MAX];

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    forge_mutex_lock(&context->log_mutex);
    forge_logger_log(active_logger, stage, "%s", message);
    forge_mutex_unlock(&context->log_mutex);
}

static void compile_fail(ForgeCompileContext *context, const char *message)
{
    forge_mutex_lock(&context->job_mutex);
    if (!context->failed && message != NULL && message[0] != '\0') {
        (void)snprintf(context->error, sizeof(context->error), "%s", message);
    }
    context->failed = 1;
    forge_mutex_unlock(&context->job_mutex);
}

static void compile_source_job(ForgeCompileContext *context, size_t source_index)
{
    const char *source_path = context->sources->items[source_index].path;
    const char *object_path = context->object_paths[source_index];
    ForgeArgv argv = {0};
    char display[FORGE_COMMAND_MAX];
    char error[FORGE_COMMAND_MAX] = {0};
    int exit_code;
    int track_headers;

    track_headers = context->compiler->kind != FORGE_COMPILER_MSVC &&
                    context->sources->items[source_index].language != FORGE_SOURCE_ASM;
    if (forge_fingerprint_object_fresh(object_path, source_path, track_headers)) {
        compile_log(context, "compile", "up-to-date: %s", source_path);
        context->object_references[source_index] = object_path;
        if (context->sources->items[source_index].language == FORGE_SOURCE_CPP) {
            context->has_cpp_source = 1;
        }
        return;
    }
    if (forge_compiler_make_compile_argv(context->compiler,
                                         context->sources->items[source_index].language,
                                         source_path, object_path, context->project_root,
                                         context->target_os, context->target_arch,
                                         context->profile, &argv, error, sizeof(error)) != 0) {
        compile_fail(context, error);
        return;
    }
    compile_log(context, "compile", "source: %s", source_path);
    (void)forge_argv_finalize(&argv);
    if (forge_argv_join(display, sizeof(display), &argv) == 0) {
        compile_log(context, "compile", "command: %s", display);
    }
    if (forge_process_run(argv.items, log_capture_path(), 1,
                          &exit_code, error, sizeof(error)) != 0) {
        forge_argv_free(&argv);
        compile_fail(context, error);
        return;
    }
    forge_argv_free(&argv);
    if (exit_code != 0) {
        char message[FORGE_COMMAND_MAX];
        (void)snprintf(message, sizeof(message), "compiler failed for %s", source_path);
        compile_fail(context, message);
        return;
    }
    context->object_references[source_index] = object_path;
    if (context->sources->items[source_index].language == FORGE_SOURCE_CPP) {
        context->has_cpp_source = 1;
    }
}

static void compile_worker(void *argument)
{
    ForgeCompileContext *context = (ForgeCompileContext *)argument;

    for (;;) {
        size_t index;

        forge_mutex_lock(&context->job_mutex);
        if (context->failed) {
            forge_mutex_unlock(&context->job_mutex);
            return;
        }
        if (context->next >= context->count) {
            forge_mutex_unlock(&context->job_mutex);
            return;
        }
        index = context->next++;
        forge_mutex_unlock(&context->job_mutex);

        compile_source_job(context, index);
    }
}

/* Returns 0 on success, -1 when the compile phase failed. */
static int run_compile_pool(ForgeCompileContext *context, int max_jobs)
{
    ForgeThread *workers;
    int worker_count;
    int workers_started = 0;
    int index;

    if (context->count == 0U) {
        return 0;
    }
    worker_count = max_jobs <= 0 ? forge_thread_processor_count() : max_jobs;
    if (worker_count > (int)context->count) {
        worker_count = (int)context->count;
    }
    if (worker_count < 1) {
        worker_count = 1;
    }
    workers = calloc((size_t)worker_count, sizeof(*workers));
    if (workers == NULL) {
        print_error("out of memory while preparing build workers");
        return -1;
    }
    for (index = 0; index < worker_count; ++index) {
        if (forge_thread_spawn(&workers[index], compile_worker, context) != 0) {
            break;
        }
        ++workers_started;
    }
    for (index = 0; index < workers_started; ++index) {
        (void)forge_thread_join(&workers[index]);
    }
    free(workers);
    if (workers_started < worker_count) {
        print_error("could not start all build workers");
        return -1;
    }
    return context->failed ? -1 : 0;
}

int forge_build_project(const char *project_root, const ForgeManifest *manifest,
                        int release, int should_run, int max_jobs,
                        char *built_executable, size_t built_executable_size)
{
    ForgeHostInfo host;
    ForgeCompiler compiler;
    ForgeSourceList sources = {0};
    const ForgeBuildProfile *profile;
    const char *target_os;
    const char *target_arch;
    ForgeCompileContext context = {0};
    ForgeArgv argv = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char target_directory[FORGE_PATH_MAX];
    char profile_directory[FORGE_PATH_MAX];
    char object_directory[FORGE_PATH_MAX];
    char executable_name[FORGE_MANIFEST_VALUE_MAX];
    char executable_path[FORGE_PATH_MAX];
    char display[FORGE_COMMAND_MAX];
    char (*object_paths)[FORGE_PATH_MAX] = NULL;
    char (*used_object_names)[FORGE_PATH_MAX] = NULL;
    const char **object_references = NULL;
    size_t source_index;
    size_t used_count = 0U;
    int used_response_file = 0;
    int exit_code;
    int result = -1;

    if (forge_detect_host(&host, error, sizeof(error)) != 0) {
        print_error("%s", error);
        goto cleanup;
    }
    if (select_host_target(manifest, &host, &target_os, &target_arch) != 0 ||
        forge_compiler_select(&host, manifest->compiler_override, &compiler,
                              error, sizeof(error)) != 0 ||
        forge_sources_collect(project_root, manifest, &sources, error,
                        sizeof(error)) != 0) {
        if (error[0] != '\0') {
            print_error("%s", error);
        }
        goto cleanup;
    }

    profile = release ? &manifest->release_profile : &manifest->debug_profile;
    if (forge_paths_resolve(project_root, "target", target_directory,
                             sizeof(target_directory)) != 0 ||
        forge_paths_join(profile_directory, sizeof(profile_directory), target_directory,
                         release ? "release" : "debug") != 0 ||
        forge_paths_join(object_directory, sizeof(object_directory), profile_directory,
                         "obj") != 0) {
        print_error("output path is too long");
        goto cleanup;
    }
    if (forge_paths_ensure_directory(object_directory, error, sizeof(error)) != 0) {
        if (error[0] != '\0') {
            print_error("%s", error);
        }
        goto cleanup;
    }
    forge_paths_safe_output_name(manifest->project_name, executable_name,
                                 sizeof(executable_name));
#if FORGE_PLATFORM_WINDOWS
    if (snprintf(executable_path, sizeof(executable_path), "%s/%s.exe",
                 profile_directory, executable_name) < 0 ||
        strlen(profile_directory) + strlen(executable_name) + 5U >= sizeof(executable_path)) {
#else
    if (forge_paths_join(executable_path, sizeof(executable_path), profile_directory,
                         executable_name) != 0) {
#endif
        print_error("executable output path is too long");
        goto cleanup;
    }
    object_paths = calloc(sources.count, sizeof(*object_paths));
    used_object_names = calloc(sources.count, sizeof(*used_object_names));
    object_references = calloc(sources.count, sizeof(*object_references));
    if (object_paths == NULL || used_object_names == NULL || object_references == NULL) {
        print_error("out of memory while preparing build outputs");
        goto cleanup;
    }

    forge_logger_log(active_logger, "dispatch", "host=%s version=%s target=%s/%s",
                     host.os_name, host.version, target_os, target_arch);
    forge_logger_log(active_logger, "dispatch", "%s (%s)", compiler.selection_note,
                     forge_compiler_kind_name(compiler.kind));
    if (compiler.kind == FORGE_COMPILER_MSVC) {
        forge_logger_log(active_logger, "dispatch",
                         "MSVC writes no dependency files; header edits will not trigger recompiles");
    }

    for (source_index = 0U; source_index < sources.count; ++source_index) {
        if (forge_fingerprint_object_name(object_directory,
                                      sources.items[source_index].path,
                                      used_object_names, used_count,
                                      object_paths[source_index],
                                      sizeof(object_paths[source_index])) != 0) {
            print_error("object path is too long or object-name collisions were not resolvable");
            goto cleanup;
        }
        (void)snprintf(used_object_names[used_count], sizeof(used_object_names[used_count]),
                       "%s", object_paths[source_index]);
        ++used_count;
    }

    forge_logger_log(active_logger, "compile", "----- compile %zu source file(s) with %d job(s) -----",
                     sources.count, max_jobs <= 0 ? forge_thread_processor_count() : max_jobs);
    context.compiler = &compiler;
    context.profile = profile;
    context.sources = &sources;
    context.object_paths = object_paths;
    context.object_references = object_references;
    context.project_root = project_root;
    context.target_os = target_os;
    context.target_arch = target_arch;
    context.count = sources.count;
    context.next = 0U;
    forge_mutex_init(&context.job_mutex);
    forge_mutex_init(&context.log_mutex);
    if (run_compile_pool(&context, max_jobs) != 0) {
        forge_mutex_destroy(&context.job_mutex);
        forge_mutex_destroy(&context.log_mutex);
        print_error("%s", context.error[0] != '\0' ? context.error : "compile phase failed");
        print_log_tail(24U);
        goto cleanup;
    }
    forge_mutex_destroy(&context.job_mutex);
    forge_mutex_destroy(&context.log_mutex);

    argv = (ForgeArgv){0};
    if (forge_compiler_make_link_argv(&compiler, context.has_cpp_source, object_references,
                                      sources.count, executable_path, profile_directory,
                                      profile, &argv, &used_response_file, error,
                                      sizeof(error)) != 0) {
        print_error("%s", error);
        goto cleanup;
    }
    forge_logger_log(active_logger, "link", "----- link %s -----", executable_path);
    (void)forge_argv_finalize(&argv);
    if (forge_argv_join(display, sizeof(display), &argv) == 0) {
        forge_logger_log(active_logger, "link", "command: %s", display);
    }
    if (used_response_file) {
        forge_logger_log(active_logger, "link",
                         "long link line: objects and flags spilled to %s/link.rsp",
                         profile_directory);
    }
    exit_code = 0;
    if (forge_process_run(argv.items, log_capture_path(), 1,
                          &exit_code, error, sizeof(error)) != 0) {
        print_error("%s", error);
        forge_argv_free(&argv);
        goto cleanup;
    }
    forge_argv_free(&argv);
    if (exit_code != 0) {
        print_error("linker failed");
        print_log_tail(24U);
        goto cleanup;
    }
    if (built_executable != NULL && built_executable_size != 0U) {
        if (snprintf(built_executable, built_executable_size, "%s", executable_path) < 0 ||
            strlen(executable_path) >= built_executable_size) {
            print_error("built executable path is too long");
            goto cleanup;
        }
    }
    if (!should_run) {
        result = 0;
        goto cleanup;
    }
    forge_logger_log(active_logger, "run", "----- run %s -----", executable_path);
    argv = (ForgeArgv){0};
    if (forge_argv_append(&argv, executable_path) != 0) {
        print_error("out of memory while preparing run command");
        goto cleanup;
    }
    (void)forge_argv_finalize(&argv);
    if (forge_argv_join(display, sizeof(display), &argv) == 0) {
        forge_logger_log(active_logger, "run", "program: %s", display);
    }
    exit_code = 0;
    if (forge_process_run(argv.items, NULL, 0, &exit_code, error, sizeof(error)) != 0) {
        forge_argv_free(&argv);
        print_error("%s", error);
        goto cleanup;
    }
    forge_argv_free(&argv);
    if (exit_code != 0) {
        print_error("program failed");
        print_log_tail(24U);
        goto cleanup;
    }
    result = 0;

cleanup:
    forge_argv_free(&argv);
    free(object_references);
    free(used_object_names);
    free(object_paths);
    forge_sources_free(&sources);
    return result;
}

void forge_build_set_logger(ForgeLogger *logger)
{
    active_logger = logger;
}

/* Removes the project's target/ directory, anchored to its manifest. */
int forge_build_clean(const char *manifest_path)
{
    char project_root[FORGE_PATH_MAX];
    char target_path[FORGE_PATH_MAX];

    if (forge_build_project_root(manifest_path, project_root, sizeof(project_root)) != 0) {
        return 1;
    }
    if (forge_paths_resolve(project_root, "target", target_path,
                            sizeof(target_path)) != 0) {
        print_error("clean path is too long");
        return 1;
    }
    printf("forge: removing %s\n", target_path);
    if (forge_paths_remove_tree(target_path, NULL, 0U) != 0) {
        return 1;
    }
    printf("forge: clean\n");
    return 0;
}
