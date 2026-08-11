#include <stdio.h>
#include <string.h>

#include "forge/cli.h"
#include "forge/orchestrator.h"

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Forge — C/C++/assembly build orchestrator\n"
            "Usage:\n"
            "  forge run [--release] [--manifest PATH]\n"
            "  forge debug [--release] [--manifest PATH]\n"
            "  forge clean [--manifest PATH]\n");
}

int forge_cli_main(int argc, char **argv)
{
    const char *manifest_path = "Forge.toml";
    int release = 0;
    int debug;
    int clean;
    int index;

    if (argc == 1 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    debug = strcmp(argv[1], "debug") == 0;
    clean = strcmp(argv[1], "clean") == 0;
    if (strcmp(argv[1], "run") != 0 && !debug && !clean) {
        fprintf(stderr, "forge: unsupported command '%s'\n", argv[1]);
        print_usage(stderr);
        return 1;
    }
    for (index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--release") == 0) {
            release = 1;
        } else if (strcmp(argv[index], "--manifest") == 0 && index + 1 < argc) {
            manifest_path = argv[++index];
        } else {
            fprintf(stderr, "forge: unsupported %s option '%s'\n",
                    clean ? "clean" : debug ? "debug" : "run", argv[index]);
            return 1;
        }
    }
    if (clean) {
        return forge_orchestrate_clean(manifest_path);
    }
    return debug ? forge_orchestrate_debug(manifest_path, release) :
                   forge_orchestrate_run(manifest_path, release);
}
