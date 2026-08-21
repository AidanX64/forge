#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/cli.h"
#include "forge/orchestrator.h"
#include "forge/paths.h"
#include "forge/version.h"

#define FORGE_MAX_PROGRAM_ARGUMENTS 256U

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Forge — C/C++/assembly build orchestrator (v" FORGE_VERSION ")\n"
            "Usage:\n"
            "  forge build [--release] [--jobs N] [--manifest PATH]\n"
            "  forge check [--release] [--jobs N] [--manifest PATH]\n"
            "  forge run [--release] [--jobs N] [--manifest PATH] [-- ARGS...]\n"
            "  forge test [--release] [--jobs N] [--manifest PATH]\n"
            "  forge debug [--release] [--jobs N] [--manifest PATH]\n"
            "  forge clean [--manifest PATH]\n"
            "  forge update [--manifest PATH]   re-resolve dependencies\n"
            "  forge new <NAME>      scaffold a new project directory\n"
            "  forge init            scaffold into the current directory\n");
}

/* Parses a positive integer flag value; returns -1 on a bad value and leaves
 * `output` untouched when N is "auto" or the flag is absent. */
static int parse_jobs(const char *value, int *output)
{
    char *end = NULL;
    long parsed;
    int number;

    if (value == NULL || strcmp(value, "auto") == 0) {
        return 0;
    }
    parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 1 || parsed > 1024) {
        return -1;
    }
    number = (int)parsed;
    if (number != parsed) {
        return -1;
    }
    *output = number;
    return 0;
}

/* When the caller did not pass --manifest explicitly, search upward from the
 * working directory like Cargo does. Falls back to the plain default name so
 * the normal "could not open manifest" error still guides the user. */
static void discover_manifest(const char **manifest_path, int explicit_manifest,
                              char *storage, size_t storage_size)
{
    if (explicit_manifest) {
        return;
    }
    if (forge_paths_find_manifest("Forge.toml", storage, storage_size) == 1) {
        *manifest_path = storage;
    }
}

static int command_new(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "forge: 'new' requires a project name\n");
        print_usage(stderr);
        return 1;
    }
    if (argc > 3) {
        fprintf(stderr, "forge: unsupported option '%s'\n", argv[3]);
        return 1;
    }
    return forge_orchestrate_new(argv[2]);
}

static int command_init(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "forge: unsupported option '%s'\n", argv[2]);
        return 1;
    }
    return forge_orchestrate_init();
}

/* Shared parser for the manifest-only commands (clean/update). */
static int command_manifest_only(const char *command, int argc, char **argv,
                                 int (*dispatch)(const char *))
{
    const char *manifest_path = "Forge.toml";
    char discovered[FORGE_PATH_MAX];
    int explicit_manifest = 0;
    int index;

    for (index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else {
            fprintf(stderr, "forge: unsupported %s option '%s'\n", command, argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));
    return dispatch(manifest_path);
}

/* Shared parser for build/check/run/test/debug: profile and job flags plus
 * --manifest; run additionally accepts "-- ARGS..." for the program itself. */
static int command_build_like(const char *command, int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    const char *program_arguments[FORGE_MAX_PROGRAM_ARGUMENTS];
    char discovered[FORGE_PATH_MAX];
    size_t program_argument_count = 0U;
    int accepts_program_arguments = strcmp(command, "run") == 0;
    int explicit_manifest = 0;
    int release = 0;
    int max_jobs = 0;
    int index;

    for (index = 2; index < argc; ++index) {
        if (accepts_program_arguments && strcmp(argv[index], "--") == 0) {
            while (++index < argc) {
                if (program_argument_count == FORGE_MAX_PROGRAM_ARGUMENTS) {
                    fprintf(stderr, "forge: too many program arguments (max %u)\n",
                            (unsigned int)FORGE_MAX_PROGRAM_ARGUMENTS);
                    return 1;
                }
                program_arguments[program_argument_count++] = argv[index];
            }
            break;
        }
        if (strcmp(argv[index], "--release") == 0) {
            release = 1;
        } else if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_path = argv[++index];
            explicit_manifest = 1;
        } else if ((strcmp(argv[index], "--jobs") == 0 ||
                    strcmp(argv[index], "-j") == 0) && index + 1 < argc) {
            if (parse_jobs(argv[++index], &max_jobs) != 0) {
                fprintf(stderr, "forge: '%s' expects a positive integer or 'auto'\n",
                        argv[index - 1]);
                return 1;
            }
        } else {
            fprintf(stderr, "forge: unsupported %s option '%s'\n",
                    command, argv[index]);
            return 1;
        }
    }
    discover_manifest(&manifest_path, explicit_manifest, discovered, sizeof(discovered));

    if (strcmp(command, "build") == 0) {
        return forge_orchestrate_build(manifest_path, release, max_jobs);
    }
    if (strcmp(command, "check") == 0) {
        return forge_orchestrate_check(manifest_path, release, max_jobs);
    }
    if (strcmp(command, "test") == 0) {
        return forge_orchestrate_test(manifest_path, release, max_jobs);
    }
    if (strcmp(command, "debug") == 0) {
        return forge_orchestrate_debug(manifest_path, release, max_jobs);
    }
    return forge_orchestrate_run(manifest_path, release, max_jobs,
                                 program_arguments, program_argument_count);
}

int forge_cli_main(int argc, char **argv)
{
    const char *command;

    if (argc == 1 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("forge %s\n", FORGE_VERSION);
        return 0;
    }
    command = argv[1];
    if (strcmp(command, "build") != 0 && strcmp(command, "check") != 0 &&
        strcmp(command, "run") != 0 && strcmp(command, "test") != 0 &&
        strcmp(command, "debug") != 0 && strcmp(command, "clean") != 0 &&
        strcmp(command, "update") != 0 &&
        strcmp(command, "new") != 0 && strcmp(command, "init") != 0) {
        fprintf(stderr, "forge: unsupported command '%s'\n", argv[1]);
        print_usage(stderr);
        return 1;
    }
    if (strcmp(command, "new") == 0) {
        return command_new(argc, argv);
    }
    if (strcmp(command, "init") == 0) {
        return command_init(argc, argv);
    }
    if (strcmp(command, "clean") == 0) {
        return command_manifest_only(command, argc, argv, forge_orchestrate_clean);
    }
    if (strcmp(command, "update") == 0) {
        return command_manifest_only(command, argc, argv, forge_orchestrate_update);
    }
    return command_build_like(command, argc, argv);
}
