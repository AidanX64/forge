#ifndef FORGE_ORCHESTRATOR_H
#define FORGE_ORCHESTRATOR_H

#include <stddef.h>

/* Builds the current project's manifest target without running it. */
int forge_orchestrate_build(const char *manifest_path, int release, int max_jobs);
/* Compiles every translation unit without linking (fast validation). */
int forge_orchestrate_check(const char *manifest_path, int release, int max_jobs);
/* Builds then executes the current project's manifest target; the trailing
 * arguments are passed to the program and its exit code becomes the return
 * value. */
int forge_orchestrate_run(const char *manifest_path, int release, int max_jobs,
                          const char *const *program_arguments,
                          size_t program_argument_count);
/* Builds and runs every standalone test source in tests/. */
int forge_orchestrate_test(const char *manifest_path, int release, int max_jobs);
int forge_orchestrate_debug(const char *manifest_path, int release, int max_jobs);
int forge_orchestrate_clean(const char *manifest_path);
/* Scaffolds a new project directory named `name` / initializes the cwd. */
int forge_orchestrate_new(const char *name);
int forge_orchestrate_init(void);

#endif