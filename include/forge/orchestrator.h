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
/* Builds and runs every standalone test source in tests/. When `test_filter`
 * is non-NULL only the test whose binary name matches it runs. */
int forge_orchestrate_test(const char *manifest_path, int release, int max_jobs,
                           const char *test_filter);
/* Re-resolves [dependencies] refs and rewrites Forge.lock. When `only_name`
 * is non-NULL only that dependency is re-resolved; other lock pins stay. */
int forge_orchestrate_update(const char *manifest_path, const char *only_name);
/* Adds a [dependencies] entry (git or path source) and re-resolves pins. */
int forge_orchestrate_add(const char *manifest_path, const char *name,
                          const char *git_url, const char *ref_kind,
                          const char *ref_value, const char *dep_path);
/* Removes a [dependencies] entry and prunes its pin from Forge.lock. */
int forge_orchestrate_remove(const char *manifest_path, const char *name);
int forge_orchestrate_debug(const char *manifest_path, int release, int max_jobs);
int forge_orchestrate_clean(const char *manifest_path);
/* Scaffolds a new project directory named `name` / initializes the cwd. */
int forge_orchestrate_new(const char *name);
int forge_orchestrate_init(void);

#endif