#ifndef FORGE_ORCHESTRATOR_H
#define FORGE_ORCHESTRATOR_H

/* Builds the current project's manifest target without running it. */
int forge_orchestrate_build(const char *manifest_path, int release);
/* Builds then executes the current project's manifest target. */
int forge_orchestrate_run(const char *manifest_path, int release);
int forge_orchestrate_debug(const char *manifest_path, int release);
int forge_orchestrate_clean(const char *manifest_path);

#endif
