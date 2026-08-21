#include <stdio.h>

#include "forge/build.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/deps.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"
#include "forge/scaffold.h"

#define FORGE_PATH_MAX 1024U

static int load_invocation(const char *manifest_path, const char *kind, ForgeLogger *logger,
                           ForgeManifest *manifest, char *root, size_t root_size)
{
    char error[FORGE_COMMAND_MAX];

    if (forge_build_project_root(manifest_path, root, root_size) != 0 ||
        forge_logger_init_in(logger, root, kind, error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return -1;
    }
    forge_build_set_logger(logger);
    forge_logger_log(logger, kind, "log: %s", logger->path);
    forge_logger_log(logger, "parse", "----- parse manifest %s -----", manifest_path);
    if (forge_manifest_load(manifest_path, manifest, error, sizeof(error)) != 0) {
        forge_logger_error(logger, "parse", "%s", error);
        return -1;
    }
    return 0;
}

static void finish_invocation(ForgeLogger *logger)
{
    forge_logger_close(logger);
    forge_build_set_logger(NULL);
}

int forge_orchestrate_run(const char *manifest_path, int release, int max_jobs,
                          const char *const *program_arguments,
                          size_t program_argument_count)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char root[FORGE_PATH_MAX];
    int child_exit_code = 0;
    int status;

    if (load_invocation(manifest_path, "build", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    status = forge_build_project(root, &manifest, FORGE_BUILD_MODE_RUN, release, max_jobs,
                                 program_arguments, program_argument_count,
                                 NULL, 0U, &child_exit_code);
    forge_logger_log(&logger, "build", "result: %s", status == 0 ? "success" : "failed");
    finish_invocation(&logger);
    /* A successful pipeline propagates the program's own exit code. */
    return status != 0 ? 1 : child_exit_code;
}

int forge_orchestrate_build(const char *manifest_path, int release, int max_jobs)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char root[FORGE_PATH_MAX];
    char executable[FORGE_PATH_MAX];
    int result;

    if (load_invocation(manifest_path, "build", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, &manifest, FORGE_BUILD_MODE_LINK, release, max_jobs,
                                 NULL, 0U, executable, sizeof(executable), NULL) == 0 ? 0 : 1;
    forge_logger_log(&logger, "build", "result: %s", result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    return result;
}

int forge_orchestrate_check(const char *manifest_path, int release, int max_jobs)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char root[FORGE_PATH_MAX];
    int result;

    if (load_invocation(manifest_path, "check", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, &manifest, FORGE_BUILD_MODE_COMPILE_ONLY, release,
                                 max_jobs, NULL, 0U, NULL, 0U, NULL) == 0 ? 0 : 1;
    forge_logger_log(&logger, "check", "result: %s", result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    return result;
}

int forge_orchestrate_test(const char *manifest_path, int release, int max_jobs)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char root[FORGE_PATH_MAX];
    int result;

    if (load_invocation(manifest_path, "test", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_tests(root, &manifest, release, max_jobs);
    forge_logger_log(&logger, "test", "result: %s",
                     result == 0 ? "success" : (result < 0 ? "failed" : "tests failed"));
    finish_invocation(&logger);
    return result < 0 ? 1 : result;
}

int forge_orchestrate_update(const char *manifest_path)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    ForgeDepGraph graph = {0};
    char root[FORGE_PATH_MAX];
    char error[FORGE_COMMAND_MAX] = {0};
    int result;

    if (load_invocation(manifest_path, "update", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    if (manifest.dependencies.count == 0U) {
        forge_logger_log(&logger, "update", "no [dependencies] to update");
        finish_invocation(&logger);
        return 0;
    }
    result = forge_deps_resolve(root, &manifest, 1, &graph, &logger, error,
                                sizeof(error)) == 0 ? 0 : 1;
    if (result != 0 && error[0] != '\0') {
        forge_logger_error(&logger, "update", "%s", error);
    } else {
        forge_logger_log(&logger, "update", "resolved %zu dependencies",
                         graph.count);
    }
    forge_deps_free_graph(&graph);
    forge_logger_log(&logger, "update", "result: %s", result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    return result;
}

int forge_orchestrate_debug(const char *manifest_path, int release, int max_jobs)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char error[FORGE_COMMAND_MAX] = {0};
    char root[FORGE_PATH_MAX];
    char executable[FORGE_PATH_MAX];
    int result;

    if (load_invocation(manifest_path, "debug", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, &manifest, FORGE_BUILD_MODE_LINK, release, max_jobs,
                                 NULL, 0U, executable, sizeof(executable), NULL) == 0 &&
             forge_debug_launch(executable, &logger, error, sizeof(error)) == 0 ? 0 : 1;
    if (result != 0 && error[0] != '\0') {
        forge_logger_error(&logger, "debug", "%s", error);
    }
    forge_logger_log(&logger, "debug", "result: %s", result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    return result;
}

int forge_orchestrate_clean(const char *manifest_path)
{
    return forge_build_clean(manifest_path);
}

int forge_orchestrate_new(const char *name)
{
    char error[FORGE_COMMAND_MAX] = {0};

    if (forge_scaffold_new_project(name, error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    printf("Created new project '%s' (Forge.toml, src/main.c)\n"
           "Run it with: cd %s && forge run\n", name, name);
    return 0;
}

int forge_orchestrate_init(void)
{
    char error[FORGE_COMMAND_MAX] = {0};

    if (forge_scaffold_init_project(error, sizeof(error)) != 0) {
        fprintf(stderr, "forge: %s\n", error);
        return 1;
    }
    printf("Initialized forge project in the current directory\n"
           "Run it with: forge run\n");
    return 0;
}
