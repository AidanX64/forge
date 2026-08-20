#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "forge/cli.h"
#include "forge/orchestrator.h"

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Forge — C/C++/assembly build orchestrator\n"
            "Usage:\n"
            "  forge build [--release] [--jobs N] [--manifest PATH]\n"
            "  forge run [--release] [--jobs N] [--manifest PATH]\n"
            "  forge debug [--release] [--jobs N] [--manifest PATH]\n"
            "  forge clean [--manifest PATH]\n");
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

int forge_cli_main(int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    const char *command;
    int release = 0;
    int max_jobs = 0;
    int debug;
    int clean;
    int index;

    if (argc == 1 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "build") != 0 && strcmp(argv[1], "run") != 0 &&
        strcmp(argv[1], "debug") != 0 && strcmp(argv[1], "clean") != 0) {
        fprintf(stderr, "forge: unsupported command '%s'\n", argv[1]);
        print_usage(stderr);
        return 1;
    }
    command = argv[1];
    debug = strcmp(command, "debug") == 0;
    clean = strcmp(command, "clean") == 0;
    for (index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--release") == 0) {
            release = 1;
        } else if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_path = argv[++index];
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
    if (clean) {
        return forge_orchestrate_clean(manifest_path);
    }
    if (strcmp(command, "build") == 0) {
        return forge_orchestrate_build(manifest_path, release, max_jobs);
    }
    return debug ? forge_orchestrate_debug(manifest_path, release, max_jobs) :
                   forge_orchestrate_run(manifest_path, release, max_jobs);
}
