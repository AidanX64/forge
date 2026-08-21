#include "forge/platform.h"

#if !FORGE_PLATFORM_WINDOWS
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
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
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include "forge/argv.h"
#include "forge/deps.h"
#include "forge/process.h"
#include "forge/thread.h"
#include "forge_util.h"

#define FORGE_DEPS_VALUE_MAX FORGE_MANIFEST_VALUE_MAX
#define FORGE_LOCK_MAX_ENTRIES FORGE_MANIFEST_MAX_DEPS
#define FORGE_SCAN_ENTRY_LIMIT 20000U
#define FORGE_LOCK_LINE_MAX 4096U

/* One pinned git dependency as recorded in Forge.lock. */
typedef struct ForgeLockEntry {
    char name[FORGE_DEPS_VALUE_MAX];
    char commit[FORGE_DEPS_VALUE_MAX];
    char url[FORGE_DEPS_VALUE_MAX];
    char ref[FORGE_DEPS_VALUE_MAX];
} ForgeLockEntry;

typedef struct ForgeLockFile {
    ForgeLockEntry items[FORGE_LOCK_MAX_ENTRIES];
    size_t count;
    int exists;
} ForgeLockFile;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void deps_log(ForgeLogger *logger, const char *stage, const char *format, ...)
{
    va_list arguments;
    char message[FORGE_COMMAND_MAX];

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (logger != NULL) {
        forge_logger_log(logger, stage, "%s", message);
    } else {
        fprintf(stderr, "forge: [%s] %s\n", stage, message);
    }
}

/* The shared dependency cache root: $FORGE_HOME or ~/.forge. */
static int forge_home_root(char *destination, size_t destination_size)
{
    const char *override = getenv("FORGE_HOME");
    const char *home = getenv("HOME");

#if FORGE_PLATFORM_WINDOWS
    if (home == NULL || home[0] == '\0') {
        home = getenv("USERPROFILE");
    }
#endif
    if (override != NULL && override[0] != '\0') {
        return snprintf(destination, destination_size, "%s", override) >= 0 &&
               strlen(override) < destination_size ? 0 : -1;
    }
    if (home == NULL || home[0] == '\0') {
        return -1;
    }
    return snprintf(destination, destination_size, "%s/.forge", home) >= 0 &&
           strlen(home) + 7U < destination_size ? 0 : -1;
}

/* FNV-1a over the URL keeps cache directories unique per source. */
static void url_cache_hash(const char *url, char *hex, size_t hex_size)
{
    unsigned long long hash = 1469598103934665603ULL;
    const char *cursor;

    for (cursor = url; *cursor != '\0'; ++cursor) {
        hash ^= (unsigned char)*cursor;
        hash *= 1099511628211ULL;
    }
    (void)snprintf(hex, hex_size, "%016llx", (unsigned long long)hash);
}

static int file_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0 && S_ISREG(details.st_mode);
}

static int directory_exists(const char *path)
{
    struct stat details;

    return stat(path, &details) == 0 && S_ISDIR(details.st_mode);
}

/* Joins dir + name and reports whether the result is a regular file. */
static int dir_has_file(const char *dir, const char *name)
{
    char path[FORGE_PATH_MAX];

    return forge_paths_join(path, sizeof(path), dir, name) == 0 &&
           file_exists(path);
}

/* Runs a program directly (no shell), echoing the command line through the
 * logger and capturing output into the invocation log like other stages. */
static int run_tool(ForgeLogger *logger, const char *stage, ForgeArgv *argv,
                    const char *capture_path, int truncating_capture,
                    int *exit_code, char *error, size_t error_size)
{
    char display[FORGE_COMMAND_MAX];

    if (forge_argv_join(display, sizeof(display), argv) == 0 && logger != NULL) {
        forge_logger_log(logger, stage, "command: %s", display);
    }
    (void)forge_argv_finalize(argv);
    return forge_process_run(argv->items, capture_path, truncating_capture ? 0 : 1,
                             exit_code, error, error_size);
}

/* ------------------------------------------------------------------ */
/* Git operations (via the git CLI; no library dependency)             */
/* ------------------------------------------------------------------ */

static int git_available(char *error, size_t error_size)
{
    if (!forge_util_program_available("git")) {
        forge_util_set_error(error, error_size,
                  "git was not found on PATH; git dependencies require it");
        return -1;
    }
    return 0;
}

/*
 * Runs `git` with `arguments` (NULL-terminated, without the program name).
 * When `repo_dir` is non-NULL the command is scoped to that repository via
 * the global -C flag, since forge's own working directory is unrelated.
 */
static int git_run(ForgeLogger *logger, const char *repo_dir, char **arguments,
                   const char *capture_path, int truncating_capture,
                   char *error, size_t error_size)
{
    ForgeArgv argv = {0};
    int exit_code = 0;
    int status;

    if (forge_argv_append(&argv, "git") != 0 ||
        (repo_dir != NULL &&
         (forge_argv_append(&argv, "-C") != 0 ||
          forge_argv_append(&argv, repo_dir) != 0))) {
        goto out_of_memory;
    }
    for (char **cursor = arguments; *cursor != NULL; ++cursor) {
        if (forge_argv_append(&argv, *cursor) != 0) {
            goto out_of_memory;
        }
    }
    status = run_tool(logger, "deps", &argv, capture_path, truncating_capture,
                      &exit_code, error, error_size);
    forge_argv_free(&argv);
    if (status != 0) {
        return -1;
    }
    if (exit_code != 0) {
        forge_util_set_error(error, error_size, "git %s failed (exit %d)",
                  arguments[0], exit_code);
        return -1;
    }
    return 0;
out_of_memory:
    forge_argv_free(&argv);
    forge_util_set_error(error, error_size, "out of memory while building a git command");
    return -1;
}

/* Reads the first line of a file (used to capture `git rev-parse` output). */
static int read_first_line(const char *path, char *output, size_t output_size)
{
    FILE *file = fopen(path, "r");
    char line[FORGE_PATH_MAX];

    if (file == NULL) {
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);
    line[strcspn(line, "\r\n")] = '\0';
    if (strlen(line) >= output_size) {
        return -1;
    }
    (void)snprintf(output, output_size, "%s", line);
    return 0;
}

/*
 * Makes sure `cache_dir` holds a clone of `url` with `target` (a ref, or a
 * locked commit SHA) checked out, and reports the resolved commit SHA.
 * When `locked_commit` is non-empty and no update was forced, the fetch is
 * skipped entirely so offline rebuilds stay quiet and fast.
 */
static int ensure_git_checkout(ForgeLogger *logger, const char *name, const char *url,
                               const char *ref, const char *locked_commit,
                               int force_update, const char *cache_dir,
                               char *resolved_sha, size_t resolved_sha_size,
                               char *error, size_t error_size)
{
    char capture[FORGE_PATH_MAX];
    char *clone_arguments[] = { "clone", "--quiet", (char *)url, (char *)cache_dir, NULL };
    char *fetch_arguments[] = { "fetch", "--quiet", "--tags", "origin", NULL };
    char *checkout_arguments[] = { "checkout", "--quiet", "--detach", NULL, NULL };
    char *revparse_arguments[] = { "rev-parse", "HEAD", NULL };
    char target[FORGE_DEPS_VALUE_MAX];
    int use_locked = locked_commit[0] != '\0' && !force_update;

    if (!directory_exists(cache_dir)) {
        deps_log(logger, "deps", "cloning %s (%s)", name, url);
        if (git_run(logger, NULL, clone_arguments, NULL, 0, error, error_size) != 0) {
            return -1;
        }
    } else if (!use_locked) {
        if (git_run(logger, cache_dir, fetch_arguments, NULL, 0, error, error_size) != 0) {
            return -1;
        }
    }
    if (use_locked) {
        (void)snprintf(target, sizeof(target), "%s", locked_commit);
    } else if (ref[0] != '\0') {
        (void)snprintf(target, sizeof(target), "%s", ref);
    } else {
        /* No ref pinned: track the remote's default branch tip. */
        (void)snprintf(target, sizeof(target), "FETCH_HEAD");
    }
    checkout_arguments[3] = target;
    if (git_run(logger, cache_dir, checkout_arguments, NULL, 0, error, error_size) != 0) {
        return -1;
    }
    if (snprintf(capture, sizeof(capture), "%s/revparse.tmp", cache_dir) < 0 ||
        strlen(cache_dir) + 14U >= sizeof(capture)) {
        forge_util_set_error(error, error_size, "dependency cache path is too long");
        return -1;
    }
    if (git_run(logger, cache_dir, revparse_arguments, capture, 1, error, error_size) != 0 ||
        read_first_line(capture, resolved_sha, resolved_sha_size) != 0) {
        forge_util_set_error(error, error_size,
                  "could not read the resolved commit for '%s'", name);
        (void)remove(capture);
        return -1;
    }
    (void)remove(capture);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lockfile                                                            */
/* ------------------------------------------------------------------ */

static ForgeLockEntry *lock_find(ForgeLockFile *lock, const char *name)
{
    size_t index;

    for (index = 0U; index < lock->count; ++index) {
        if (strcmp(lock->items[index].name, name) == 0) {
            return &lock->items[index];
        }
    }
    return NULL;
}

/*
 * Parses the generated Forge.lock subset:
 *   [dependencies]
 *   name = { commit = "...", url = "...", ref = "..." }
 */
static int load_lockfile(const char *path, ForgeLockFile *lock,
                         char *error, size_t error_size)
{
    FILE *file;
    char line[FORGE_LOCK_LINE_MAX];
    int in_section = 0;

    memset(lock, 0, sizeof(*lock));
    file = fopen(path, "r");
    if (file == NULL) {
        return 0; /* no lockfile yet is normal */
    }
    lock->exists = 1;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = forge_util_trim(line);
        char *equals;
        char *open;
        char *close;
        char *cursor;
        ForgeLockEntry *entry;

        if (*text == '\0' || *text == '#') {
            continue;
        }
        if (*text == '[') {
            in_section = strcmp(text, "[dependencies]") == 0;
            continue;
        }
        if (!in_section) {
            continue;
        }
        equals = strchr(text, '=');
        open = strchr(text, '{');
        close = strrchr(text, '}');
        if (equals == NULL || open == NULL || close == NULL || equals > open) {
            forge_util_set_error(error, error_size,
                      "%s: malformed lock entry '%s'", path, text);
            (void)fclose(file);
            return -1;
        }
        if (lock->count == FORGE_LOCK_MAX_ENTRIES) {
            forge_util_set_error(error, error_size, "%s: too many locked dependencies",
                      path);
            (void)fclose(file);
            return -1;
        }
        entry = &lock->items[lock->count++];
        *equals = '\0';
        text = forge_util_trim(text);
        if (strlen(text) >= sizeof(entry->name)) {
            forge_util_set_error(error, error_size, "%s: dependency name is too long", path);
            (void)fclose(file);
            return -1;
        }
        (void)snprintf(entry->name, sizeof(entry->name), "%s", text);
        cursor = open + 1;
        *close = '\0';
        while (*cursor != '\0') {
            char *key_start;
            char *key_end;
            char *quote;
            char *value;

            /* Skip separators and whitespace between entries. */
            while (*cursor == ',' || isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            if (*cursor == '\0') {
                break;
            }
            key_start = cursor;
            while (*cursor != '\0' && *cursor != '=') {
                ++cursor;
            }
            key_end = cursor;
            if (*cursor == '\0' ||
                memchr(key_start, '"', (size_t)(key_end - key_start)) != NULL) {
                break; /* tolerate trailing commas / whitespace */
            }
            ++cursor; /* skip '=' */
            quote = strchr(cursor, '"');
            if (quote == NULL) {
                break;
            }
            value = quote + 1;
            quote = strchr(value, '"');
            if (quote == NULL) {
                break;
            }
            *key_end = '\0';
            key_start = forge_util_trim(key_start);
            *quote = '\0';
            if (strcmp(key_start, "commit") == 0) {
                (void)snprintf(entry->commit, sizeof(entry->commit), "%s", value);
            } else if (strcmp(key_start, "url") == 0) {
                (void)snprintf(entry->url, sizeof(entry->url), "%s", value);
            } else if (strcmp(key_start, "ref") == 0) {
                (void)snprintf(entry->ref, sizeof(entry->ref), "%s", value);
            }
            cursor = quote + 1;
        }
    }
    (void)fclose(file);
    return 0;
}

static int save_lockfile(const char *path, const ForgeLockFile *lock,
                         char *error, size_t error_size)
{
    FILE *file = fopen(path, "w");
    size_t index;

    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not write '%s'", path);
        return -1;
    }
    (void)fputs("# Generated by forge. Do not edit.\n[dependencies]\n", file);
    for (index = 0U; index < lock->count; ++index) {
        const ForgeLockEntry *entry = &lock->items[index];

        if (fprintf(file, "%s = { commit = \"%s\", url = \"%s\", ref = \"%s\" }\n",
                    entry->name, entry->commit, entry->url, entry->ref) < 0) {
            forge_util_set_error(error, error_size, "could not write '%s'", path);
            (void)fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) {
        forge_util_set_error(error, error_size, "could not write '%s'", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Resolution                                                          */
/* ------------------------------------------------------------------ */

typedef struct ForgeResolveContext {
    const char *project_root;
    char lock_path[FORGE_PATH_MAX];
    ForgeLockFile lock;
    int lock_dirty;
    int force_update;
    ForgeDepGraph *graph;
    ForgeLogger *logger;
    char error[FORGE_COMMAND_MAX];
} ForgeResolveContext;

static ForgeDepNode *graph_find_node(ForgeDepGraph *graph, const char *name)
{
    size_t index;

    for (index = 0U; index < graph->count; ++index) {
        if (strcmp(graph->nodes[index].name, name) == 0) {
            return &graph->nodes[index];
        }
    }
    return NULL;
}

static int resolve_recursive(ForgeResolveContext *context, const char *consumer_root,
                             const ForgeManifest *manifest, int depth,
                             char names[][FORGE_DEPS_VALUE_MAX])
{
    size_t index;

    if (depth >= (int)FORGE_DEPS_MAX_DEPTH) {
        forge_util_set_error(context->error, sizeof(context->error),
                  "dependency graph is deeper than %d levels", FORGE_DEPS_MAX_DEPTH);
        return -1;
    }
    for (index = 0U; index < manifest->dependencies.count; ++index) {
        const ForgeDependency *dependency = &manifest->dependencies.items[index];
        char root[FORGE_PATH_MAX];
        ForgeDepNode *node;
        ForgeManifest *parsed = NULL;
        int is_native;
        size_t stack_index;

        /* Cycle check along the current path. */
        for (stack_index = 0U; (int)stack_index < depth; ++stack_index) {
            if (strcmp(names[stack_index], dependency->name) == 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency cycle detected at '%s'", dependency->name);
                return -1;
            }
        }

        node = graph_find_node(context->graph, dependency->name);
        if (node != NULL) {
            continue; /* already resolved (diamond); cycles caught above */
        }
        if (context->graph->count == FORGE_DEPS_MAX_NODES) {
            forge_util_set_error(context->error, sizeof(context->error),
                      "more than %u dependencies", FORGE_DEPS_MAX_NODES);
            return -1;
        }

        if (dependency->path[0] != '\0') {
            if (forge_paths_join(root, sizeof(root), consumer_root,
                                 dependency->path) != 0 ||
                !directory_exists(root)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "path dependency '%s' does not exist at '%s'",
                          dependency->name, root);
                return -1;
            }
            is_native = dir_has_file(root, "Forge.toml");
        } else {
            ForgeLockEntry *locked = lock_find(&context->lock, dependency->name);
            char cache_home[FORGE_PATH_MAX];
            char cache_hash[32];
            char cache_dir[FORGE_PATH_MAX];
            char resolved_sha[FORGE_DEPS_VALUE_MAX];

            if (git_available(context->error, sizeof(context->error)) != 0 ||
                forge_home_root(cache_home, sizeof(cache_home)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "cannot resolve '%s': no dependency cache available",
                          dependency->name);
                return -1;
            }
            url_cache_hash(dependency->git_url, cache_hash, sizeof(cache_hash));
            if (snprintf(cache_dir, sizeof(cache_dir), "%s/git/%s", cache_home,
                         cache_hash) < 0 ||
                strlen(cache_home) + 22U >= sizeof(cache_dir)) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency cache path is too long");
                return -1;
            }
            if (ensure_git_checkout(context->logger, dependency->name,
                                    dependency->git_url, dependency->ref,
                                    locked != NULL &&
                                    strcmp(locked->url, dependency->git_url) == 0 &&
                                    strcmp(locked->ref, dependency->ref) == 0 ?
                                        locked->commit : "",
                                    context->force_update, cache_dir,
                                    resolved_sha, sizeof(resolved_sha),
                                    context->error, sizeof(context->error)) != 0) {
                return -1;
            }
            deps_log(context->logger, "deps", "resolved %s @ %s",
                     dependency->name, resolved_sha);

            /* Record/update the pin. */
            if (locked == NULL) {
                if (context->lock.count == FORGE_LOCK_MAX_ENTRIES) {
                    forge_util_set_error(context->error, sizeof(context->error),
                              "more than %u locked dependencies", FORGE_LOCK_MAX_ENTRIES);
                    return -1;
                }
                locked = &context->lock.items[context->lock.count++];
                (void)snprintf(locked->name, sizeof(locked->name), "%s", dependency->name);
            }
            if (strcmp(locked->commit, resolved_sha) != 0 ||
                strcmp(locked->url, dependency->git_url) != 0 ||
                strcmp(locked->ref, dependency->ref) != 0) {
                (void)snprintf(locked->commit, sizeof(locked->commit), "%s", resolved_sha);
                (void)snprintf(locked->url, sizeof(locked->url), "%s", dependency->git_url);
                (void)snprintf(locked->ref, sizeof(locked->ref), "%s", dependency->ref);
                context->lock_dirty = 1;
            }
            (void)snprintf(root, sizeof(root), "%s", cache_dir);
            is_native = dir_has_file(root, "Forge.toml");
        }

        /* Parse native manifests and recurse before inserting this node so
         * dependencies always precede their consumers in build order. */
        if (is_native) {
            char manifest_path[FORGE_PATH_MAX];

            parsed = calloc(1U, sizeof(*parsed));
            if (parsed == NULL) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "out of memory while resolving dependencies");
                return -1;
            }
            if (forge_paths_join(manifest_path, sizeof(manifest_path), root,
                                 "Forge.toml") != 0 ||
                forge_manifest_load(manifest_path, parsed, context->error,
                                    sizeof(context->error)) != 0) {
                forge_util_set_error(context->error, sizeof(context->error),
                          "dependency '%s': %s", dependency->name, context->error);
                free(parsed);
                return -1;
            }
            names[depth][0] = '\0';
            (void)snprintf(names[depth], FORGE_DEPS_VALUE_MAX, "%s", dependency->name);
            if (resolve_recursive(context, root, parsed, depth + 1, names) != 0) {
                free(parsed);
                return -1;
            }
        }

        node = &context->graph->nodes[context->graph->count++];
        (void)snprintf(node->name, sizeof(node->name), "%s", dependency->name);
        (void)snprintf(node->root, sizeof(node->root), "%s", root);
        node->manifest = parsed;
        node->is_native = is_native;
        node->link_artifact[0] = '\0';
        deps_log(context->logger, "deps", "using %s (%s)", dependency->name, root);
    }
    return 0;
}

int forge_deps_resolve(const char *project_root, const ForgeManifest *manifest,
                       int force_update, ForgeDepGraph *graph, ForgeLogger *logger,
                       char *error, size_t error_size)
{
    ForgeResolveContext context;
    char names[FORGE_DEPS_MAX_DEPTH][FORGE_MANIFEST_VALUE_MAX];
    int status;

    if (project_root == NULL || manifest == NULL || graph == NULL) {
        forge_util_set_error(error, error_size, "project root, manifest, and graph are required");
        return -1;
    }
    memset(graph, 0, sizeof(*graph));
    if (manifest->dependencies.count == 0U) {
        return 0;
    }
    memset(&context, 0, sizeof(context));
    context.project_root = project_root;
    context.force_update = force_update;
    context.graph = graph;
    context.logger = logger;
    if (forge_paths_join(context.lock_path, sizeof(context.lock_path), project_root,
                         "Forge.lock") != 0) {
        forge_util_set_error(error, error_size, "lockfile path is too long");
        return -1;
    }
    if (load_lockfile(context.lock_path, &context.lock, error, error_size) != 0) {
        return -1;
    }
    status = resolve_recursive(&context, project_root, manifest, 0, names);
    if (status == 0 && (context.lock_dirty || !context.lock.exists)) {
        if (save_lockfile(context.lock_path, &context.lock, error, error_size) != 0) {
            status = -1;
        }
    }
    if (status != 0) {
        (void)snprintf(error, error_size, "%s", context.error);
    }
    return status;
}

void forge_deps_free_graph(ForgeDepGraph *graph)
{
    size_t index;

    if (graph == NULL) {
        return;
    }
    for (index = 0U; index < graph->count; ++index) {
        free(graph->nodes[index].manifest);
        graph->nodes[index].manifest = NULL;
    }
}

int forge_deps_include_dirs(const ForgeDepGraph *graph, ForgeStringList *output,
                            char *error, size_t error_size)
{
    size_t index;

    for (index = 0U; index < graph->count; ++index) {
        const ForgeDepNode *node = &graph->nodes[index];
        char candidate[FORGE_PATH_MAX];

        if (output->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "too many include directories");
            return -1;
        }
        if (forge_paths_join(candidate, sizeof(candidate), node->root, "include") != 0) {
            forge_util_set_error(error, error_size, "include path is too long");
            return -1;
        }
        {
            const char *chosen = directory_exists(candidate) ? candidate : node->root;

            if (strlen(chosen) >= sizeof(output->items[output->count])) {
                forge_util_set_error(error, error_size,
                          "include path '%s' is too long", chosen);
                return -1;
            }
            memcpy(output->items[output->count], chosen, strlen(chosen) + 1U);
        }
        ++output->count;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Foreign builds                                                      */
/* ------------------------------------------------------------------ */

static int has_static_lib_suffix(const char *name)
{
    return forge_util_has_suffix(name, ".a") || forge_util_has_suffix(name, ".lib");
}

/*
 * Depth-first search of `dir` for the first static library. Returns 1 when
 * one was copied into `found`, 0 when none was found, -1 on a hard error.
 * Generated/build directories that never hold the final artifact are pruned
 * to keep the scan bounded by `budget` entries.
 */
static int scan_dir_for_static_lib(const char *dir, char *found, size_t found_size,
                                   unsigned *budget)
{
#if FORGE_PLATFORM_WINDOWS
    WIN32_FIND_DATAA entry;
    HANDLE handle;
    char pattern[FORGE_PATH_MAX];
#else
    DIR *stream;
    struct dirent *item;
#endif

    if (*budget == 0U) {
        return 0;
    }
    --*budget;
#if FORGE_PLATFORM_WINDOWS
    if (forge_paths_join(pattern, sizeof(pattern), dir, "*") != 0) {
        return -1;
    }
    handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        char child[FORGE_PATH_MAX];
        int status;

        if (strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), dir, entry.cFileName) != 0) {
            (void)FindClose(handle);
            return -1;
        }
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            if (strcmp(entry.cFileName, "CMakeFiles") == 0 ||
                strcmp(entry.cFileName, ".git") == 0 ||
                strcmp(entry.cFileName, "target") == 0) {
                continue;
            }
            status = scan_dir_for_static_lib(child, found, found_size, budget);
            if (status != 0) {
                (void)FindClose(handle);
                return status;
            }
        } else if (has_static_lib_suffix(entry.cFileName)) {
            if (snprintf(found, found_size, "%s", child) < 0 ||
                strlen(child) >= found_size) {
                (void)FindClose(handle);
                return -1;
            }
            (void)FindClose(handle);
            return 1;
        }
    } while (FindNextFileA(handle, &entry) != 0);
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        (void)FindClose(handle);
        return -1;
    }
    (void)FindClose(handle);
    return 0;
#else
    stream = opendir(dir);
    if (stream == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((item = readdir(stream)) != NULL) {
        char child[FORGE_PATH_MAX];
        struct stat details;
        int status;

        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (forge_paths_join(child, sizeof(child), dir, item->d_name) != 0) {
            (void)closedir(stream);
            return -1;
        }
        if (stat(child, &details) != 0) {
            continue;
        }
        if (S_ISDIR(details.st_mode)) {
            if (strcmp(item->d_name, "CMakeFiles") == 0 ||
                strcmp(item->d_name, ".git") == 0 ||
                strcmp(item->d_name, "target") == 0) {
                continue;
            }
            status = scan_dir_for_static_lib(child, found, found_size, budget);
            if (status != 0) {
                (void)closedir(stream);
                return status;
            }
        } else if (has_static_lib_suffix(item->d_name)) {
            if (snprintf(found, found_size, "%s", child) < 0 ||
                strlen(child) >= found_size) {
                (void)closedir(stream);
                return -1;
            }
            (void)closedir(stream);
            return 1;
        }
    }
    (void)closedir(stream);
    return 0;
#endif
}

static int find_static_artifact(const char *root, char *artifact, size_t artifact_size,
                                char *error, size_t error_size)
{
    static const char *const search_dirs[] = { "build", "lib", "." };
    size_t index;

    for (index = 0U; index < 3U; ++index) {
        char candidate[FORGE_PATH_MAX];
        unsigned budget = FORGE_SCAN_ENTRY_LIMIT;
        int status;

        if (forge_paths_join(candidate, sizeof(candidate), root,
                             search_dirs[index]) != 0) {
            continue;
        }
        status = scan_dir_for_static_lib(candidate, artifact, artifact_size, &budget);
        if (status < 0) {
            forge_util_set_error(error, error_size,
                      "could not search '%s' for a built library", candidate);
            return -1;
        }
        if (status > 0) {
            return 0;
        }
    }
    return 1; /* nothing found */
}

int forge_deps_build_foreign(const ForgeDepNode *node, const ForgeCompiler *compiler,
                             int release, int max_jobs, ForgeLogger *logger,
                             char *artifact, size_t artifact_size,
                             char *error, size_t error_size)
{
    const char *build_type = release ? "Release" : "Debug";
    const char *capture_path = logger != NULL ? logger->path : NULL;
    char jobs[16];
    int exit_code = 0;
    int status;

    (void)snprintf(jobs, sizeof(jobs), "%d",
                   max_jobs > 0 ? max_jobs : forge_thread_processor_count());
    deps_log(logger, "deps", "building foreign dependency '%s' in %s",
             node->name, node->root);
    if (dir_has_file(node->root, "CMakeLists.txt")) {
        ForgeArgv argv = {0};

        if (!forge_util_program_available("cmake")) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' uses CMake but cmake was not found on PATH",
                      node->name);
            return -1;
        }
        if (forge_argv_append(&argv, "cmake") != 0 ||
            forge_argv_append(&argv, "-S") != 0 ||
            forge_argv_append(&argv, node->root) != 0 ||
            forge_argv_append(&argv, "-B") != 0 ||
            forge_argv_appendf(&argv, "%s/build", node->root) != 0 ||
            forge_argv_appendf(&argv, "-DCMAKE_BUILD_TYPE=%s", build_type) != 0 ||
            run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "cmake configure failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
        if (forge_argv_append(&argv, "cmake") != 0 ||
            forge_argv_append(&argv, "--build") != 0 ||
            forge_argv_appendf(&argv, "%s/build", node->root) != 0 ||
            forge_argv_append(&argv, "--config") != 0 ||
            forge_argv_append(&argv, build_type) != 0 ||
            forge_argv_append(&argv, "--parallel") != 0 ||
            forge_argv_append(&argv, jobs) != 0 ||
            run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "cmake build failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
    } else if (dir_has_file(node->root, "Makefile") ||
               dir_has_file(node->root, "makefile") ||
               dir_has_file(node->root, "GNUmakefile")) {
        ForgeArgv argv = {0};

        if (!forge_util_program_available("make")) {
            forge_util_set_error(error, error_size,
                      "dependency '%s' uses Make but make was not found on PATH",
                      node->name);
            return -1;
        }
        if (forge_argv_append(&argv, "make") != 0 ||
            forge_argv_append(&argv, "-C") != 0 ||
            forge_argv_append(&argv, node->root) != 0 ||
            forge_argv_append(&argv, "-j") != 0 ||
            forge_argv_append(&argv, jobs) != 0) {
            forge_util_set_error(error, error_size,
                      "out of memory while building a make command");
            forge_argv_free(&argv);
            return -1;
        }
        if (compiler != NULL && compiler->kind != FORGE_COMPILER_MSVC &&
            forge_argv_appendf(&argv, "CC=%s", compiler->program) != 0) {
            forge_util_set_error(error, error_size,
                      "out of memory while building a make command");
            forge_argv_free(&argv);
            return -1;
        }
        if (run_tool(logger, "deps", &argv, capture_path, 1, &exit_code,
                     error, error_size) != 0 ||
            exit_code != 0) {
            forge_util_set_error(error, error_size,
                      "make failed for dependency '%s'", node->name);
            forge_argv_free(&argv);
            return -1;
        }
        forge_argv_free(&argv);
    } else {
        forge_util_set_error(error, error_size,
                  "dependency '%s' has no Forge.toml, CMakeLists.txt, or Makefile; "
                  "forge does not know how to build it", node->name);
        return -1;
    }

    status = find_static_artifact(node->root, artifact, artifact_size, error, error_size);
    if (status != 0) {
        if (status > 0) {
            forge_util_set_error(error, error_size,
                      "built '%s' but no static library (.a/.lib) was found",
                      node->name);
        }
        return -1;
    }
    deps_log(logger, "deps", "artifact: %s", artifact);
    return 0;
}
