#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <string.h>

#include "forge/manifest.h"
#include "forge_util.h"

#define FORGE_MANIFEST_LINE_MAX 4096U

typedef enum ForgeManifestSection {
    FORGE_SECTION_NONE,
    FORGE_SECTION_PROJECT,
    FORGE_SECTION_SOURCES,
    FORGE_SECTION_TARGETS,
    FORGE_SECTION_BUILD,
    FORGE_SECTION_PROFILE_DEBUG,
    FORGE_SECTION_PROFILE_RELEASE
} ForgeManifestSection;

typedef struct ForgeManifestSeen {
    int project_name;
    int source_c;
    int source_cpp;
    int source_asm;
    int target_os;
    int target_arch;
    int compiler;
    int debug_cflags;
    int release_cflags;
} ForgeManifestSeen;

static void remove_comment(char *text)
{
    int quoted = 0;
    int escaped = 0;
    char *cursor;

    for (cursor = text; *cursor != '\0'; ++cursor) {
        if (escaped) {
            escaped = 0;
        } else if (*cursor == '\\' && quoted) {
            escaped = 1;
        } else if (*cursor == '"') {
            quoted = !quoted;
        } else if (*cursor == '#' && !quoted) {
            *cursor = '\0';
            return;
        }
    }
}

static int parse_string(char **cursor, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *source = *cursor;
    size_t length = 0U;

    if (*source != '"') {
        forge_util_set_error(error, error_size, "expected a quoted string");
        return -1;
    }

    ++source;
    while (*source != '\0' && *source != '"') {
        char character = *source++;

        if (character == '\\') {
            if (*source != '"' && *source != '\\') {
                forge_util_set_error(error, error_size,
                          "only \\\" and \\\\ escapes are supported");
                return -1;
            }
            character = *source++;
        }

        if (length + 1U >= destination_size) {
            forge_util_set_error(error, error_size, "string exceeds %u characters",
                      (unsigned int)(destination_size - 1U));
            return -1;
        }
        destination[length++] = character;
    }

    if (*source != '"') {
        forge_util_set_error(error, error_size, "unterminated quoted string");
        return -1;
    }

    destination[length] = '\0';
    *cursor = source + 1;
    return 0;
}

static int parse_string_list(char *value, ForgeStringList *list,
                             char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (*cursor != '[') {
        forge_util_set_error(error, error_size, "expected a string array starting with '['");
        return -1;
    }

    ++cursor;
    cursor = forge_util_trim(cursor);
    list->count = 0U;
    if (*cursor == ']') {
        cursor = forge_util_trim(cursor + 1);
        if (*cursor != '\0') {
            forge_util_set_error(error, error_size, "unexpected text after string array");
            return -1;
        }
        return 0;
    }

    for (;;) {
        if (list->count == FORGE_MANIFEST_MAX_ITEMS) {
            forge_util_set_error(error, error_size, "array has more than %u entries",
                      FORGE_MANIFEST_MAX_ITEMS);
            return -1;
        }
        if (parse_string(&cursor, list->items[list->count],
                         sizeof(list->items[list->count]), error, error_size) != 0) {
            return -1;
        }
        if (list->items[list->count][0] == '\0') {
            forge_util_set_error(error, error_size, "empty strings are not allowed in arrays");
            return -1;
        }
        ++list->count;

        cursor = forge_util_trim(cursor);
        if (*cursor == ']') {
            cursor = forge_util_trim(cursor + 1);
            if (*cursor != '\0') {
                forge_util_set_error(error, error_size, "unexpected text after string array");
                return -1;
            }
            return 0;
        }
        if (*cursor != ',') {
            forge_util_set_error(error, error_size, "expected ',' or ']' in string array");
            return -1;
        }
        cursor = forge_util_trim(cursor + 1);
        if (*cursor == ']') {
            forge_util_set_error(error, error_size, "trailing commas are not allowed in arrays");
            return -1;
        }
    }
}

static ForgeManifestSection parse_section(const char *text)
{
    if (strcmp(text, "[project]") == 0) {
        return FORGE_SECTION_PROJECT;
    }
    if (strcmp(text, "[sources]") == 0) {
        return FORGE_SECTION_SOURCES;
    }
    if (strcmp(text, "[targets]") == 0) {
        return FORGE_SECTION_TARGETS;
    }
    if (strcmp(text, "[build]") == 0) {
        return FORGE_SECTION_BUILD;
    }
    if (strcmp(text, "[profile.debug]") == 0) {
        return FORGE_SECTION_PROFILE_DEBUG;
    }
    if (strcmp(text, "[profile.release]") == 0) {
        return FORGE_SECTION_PROFILE_RELEASE;
    }
    return FORGE_SECTION_NONE;
}

static int parse_scalar(char *value, char *destination, size_t destination_size,
                        char *error, size_t error_size)
{
    char *cursor = forge_util_trim(value);

    if (parse_string(&cursor, destination, destination_size, error, error_size) != 0) {
        return -1;
    }
    cursor = forge_util_trim(cursor);
    if (*cursor != '\0') {
        forge_util_set_error(error, error_size, "unexpected text after quoted string");
        return -1;
    }
    if (destination[0] == '\0') {
        forge_util_set_error(error, error_size, "empty strings are not allowed");
        return -1;
    }
    return 0;
}

static int reject_duplicate(int *seen, const char *key, char *error, size_t error_size)
{
    if (*seen) {
        forge_util_set_error(error, error_size, "duplicate field '%s'", key);
        return -1;
    }
    *seen = 1;
    return 0;
}

static int parse_assignment(ForgeManifestSection section, char *line,
                            ForgeManifest *manifest, ForgeManifestSeen *seen,
                            char *error, size_t error_size)
{
    char *equals = strchr(line, '=');
    char *key;
    char *value;

    if (equals == NULL) {
        forge_util_set_error(error, error_size, "expected 'key = value'");
        return -1;
    }
    *equals = '\0';
    key = forge_util_trim(line);
    value = forge_util_trim(equals + 1);
    if (*key == '\0' || *value == '\0') {
        forge_util_set_error(error, error_size, "expected a non-empty key and value");
        return -1;
    }

    if (section == FORGE_SECTION_PROJECT && strcmp(key, "name") == 0) {
        return reject_duplicate(&seen->project_name, key, error, error_size) ||
               parse_scalar(value, manifest->project_name, sizeof(manifest->project_name),
                            error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "c") == 0) {
        return reject_duplicate(&seen->source_c, key, error, error_size) ||
               parse_string_list(value, &manifest->c_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "cpp") == 0) {
        return reject_duplicate(&seen->source_cpp, key, error, error_size) ||
               parse_string_list(value, &manifest->cpp_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_SOURCES && strcmp(key, "asm") == 0) {
        return reject_duplicate(&seen->source_asm, key, error, error_size) ||
               parse_string_list(value, &manifest->asm_source_dirs, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "os") == 0) {
        return reject_duplicate(&seen->target_os, key, error, error_size) ||
               parse_string_list(value, &manifest->target_os, error, error_size);
    }
    if (section == FORGE_SECTION_TARGETS && strcmp(key, "arch") == 0) {
        return reject_duplicate(&seen->target_arch, key, error, error_size) ||
               parse_string_list(value, &manifest->target_arch, error, error_size);
    }
    if (section == FORGE_SECTION_BUILD && strcmp(key, "compiler") == 0) {
        return reject_duplicate(&seen->compiler, key, error, error_size) ||
               parse_scalar(value, manifest->compiler_override,
                            sizeof(manifest->compiler_override), error, error_size);
    }
    if (section == FORGE_SECTION_PROFILE_DEBUG && strcmp(key, "cflags") == 0) {
        return reject_duplicate(&seen->debug_cflags, key, error, error_size) ||
               parse_string_list(value, &manifest->debug_profile.cflags, error, error_size);
    }
    if (section == FORGE_SECTION_PROFILE_RELEASE && strcmp(key, "cflags") == 0) {
        return reject_duplicate(&seen->release_cflags, key, error, error_size) ||
               parse_string_list(value, &manifest->release_profile.cflags,
                                 error, error_size);
    }

    forge_util_set_error(error, error_size, "field '%s' is not allowed in this section", key);
    return -1;
}

int forge_manifest_load(const char *path, ForgeManifest *manifest,
                        char *error, size_t error_size)
{
    FILE *file;
    char line[FORGE_MANIFEST_LINE_MAX];
    unsigned long line_number = 0UL;
    ForgeManifestSection section = FORGE_SECTION_NONE;
    ForgeManifestSeen seen = {0};

    if (path == NULL || manifest == NULL) {
        forge_util_set_error(error, error_size, "manifest path and output are required");
        return -1;
    }
    *manifest = (ForgeManifest){0};
    file = fopen(path, "r");
    if (file == NULL) {
        forge_util_set_error(error, error_size, "could not open manifest '%s'", path);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text;
        ++line_number;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            forge_util_set_error(error, error_size, "%s:%lu: line is too long", path, line_number);
            (void)fclose(file);
            return -1;
        }
        remove_comment(line);
        text = forge_util_trim(line);
        if (*text == '\0') {
            continue;
        }
        if (*text == '[') {
            section = parse_section(text);
            if (section == FORGE_SECTION_NONE) {
                forge_util_set_error(error, error_size, "%s:%lu: unknown section '%s'", path,
                          line_number, text);
                (void)fclose(file);
                return -1;
            }
            continue;
        }
        if (section == FORGE_SECTION_NONE) {
            forge_util_set_error(error, error_size, "%s:%lu: field without a section header; "
                                  "expected a [section] line first", path, line_number);
            (void)fclose(file);
            return -1;
        }
        if (parse_assignment(section, text, manifest, &seen, error, error_size) != 0) {
            char detail[FORGE_MANIFEST_LINE_MAX];
            if (error == NULL || error[0] == '\0') {
                (void)snprintf(detail, sizeof(detail), "invalid field");
            } else {
                (void)snprintf(detail, sizeof(detail), "%s", error);
            }
            forge_util_set_error(error, error_size, "%s:%lu: %s", path, line_number, detail);
            (void)fclose(file);
            return -1;
        }
    }
    (void)fclose(file);

    if (!seen.project_name || !seen.target_os || !seen.target_arch ||
        !seen.debug_cflags || !seen.release_cflags) {
        forge_util_set_error(error, error_size,
                  "%s: manifest requires project.name, targets.os/arch, "
                  "and profile.debug/release cflags", path);
        return -1;
    }
    if (manifest->c_source_dirs.count == 0U && manifest->cpp_source_dirs.count == 0U &&
        manifest->asm_source_dirs.count == 0U) {
        forge_util_set_error(error, error_size, "%s: at least one source directory is required", path);
        return -1;
    }
    if (manifest->target_os.count == 0U || manifest->target_arch.count == 0U) {
        forge_util_set_error(error, error_size, "%s: targets.os and targets.arch cannot be empty", path);
        return -1;
    }
    return 0;
}
