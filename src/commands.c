#include <stdio.h>

#include "forge/build.h"
#include "forge/compiler.h"
#include "forge/debug.h"
#include "forge/log.h"
#include "forge/manifest.h"
#include "forge/orchestrator.h"

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

int forge_orchestrate_run(const char *manifest_path, int release, int max_jobs)
{
    ForgeManifest manifest;
    ForgeLogger logger = {0};
    char root[FORGE_PATH_MAX];
    int result;

    if (load_invocation(manifest_path, "build", &logger, &manifest, root, sizeof(root)) != 0) {
        finish_invocation(&logger);
        return 1;
    }
    result = forge_build_project(root, &manifest, release, 1, max_jobs, NULL, 0U) == 0 ? 0 : 1;
    forge_logger_log(&logger, "build", "result: %s", result == 0 ? "success" : "failed");
    finish_invocation(&logger);
    return result;
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
    result = forge_build_project(root, &manifest, release, 0, max_jobs,
                                 executable, sizeof(executable)) == 0 ? 0 : 1;
    forge_logger_log(&logger, "build", "result: %s", result == 0 ? "success" : "failed");
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
    result = forge_build_project(root, &manifest, release, 0, max_jobs,
                                 executable, sizeof(executable)) == 0 &&
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
