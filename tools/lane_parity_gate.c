#define _POSIX_C_SOURCE 200809L

#include "lane_parity_gate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} strings_t;

typedef struct {
    char *first;
    char *second;
} pair_t;

typedef struct {
    pair_t *items;
    size_t count;
    size_t capacity;
} pairs_t;

typedef struct {
    strings_t hosts;
    pairs_t reservations;
    pairs_t lanes;
    pairs_t tests;
} manifest_t;

static int valid_token(const char *text) {
    if (text == NULL || *text == '\0') return 0;
    for (; *text != '\0'; ++text)
        if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') || (*text >= '0' && *text <= '9') ||
              *text == '_' || *text == '-' || *text == '.'))
            return 0;
    return 1;
}

static int strings_contains(const strings_t *values, const char *value) {
    for (size_t index = 0; index < values->count; ++index)
        if (strcmp(values->items[index], value) == 0) return 1;
    return 0;
}

static char *copy_string(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

static int strings_add(strings_t *values, const char *value) {
    if (strings_contains(values, value)) return 1;
    if (values->count == values->capacity) {
        size_t capacity = values->capacity == 0 ? 8 : values->capacity * 2;
        char **items = realloc(values->items, capacity * sizeof(*items));
        if (items == NULL) return -1;
        values->items = items;
        values->capacity = capacity;
    }
    values->items[values->count] = copy_string(value);
    if (values->items[values->count] == NULL) return -1;
    ++values->count;
    return 0;
}

static int pairs_contains(const pairs_t *values, const char *first, const char *second) {
    for (size_t index = 0; index < values->count; ++index)
        if (strcmp(values->items[index].first, first) == 0 && strcmp(values->items[index].second, second) == 0)
            return 1;
    return 0;
}

static int pairs_add(pairs_t *values, const char *first, const char *second) {
    if (pairs_contains(values, first, second)) return 1;
    if (values->count == values->capacity) {
        size_t capacity = values->capacity == 0 ? 16 : values->capacity * 2;
        pair_t *items = realloc(values->items, capacity * sizeof(*items));
        if (items == NULL) return -1;
        values->items = items;
        values->capacity = capacity;
    }
    pair_t pair = {copy_string(first), copy_string(second)};
    if (pair.first == NULL || pair.second == NULL) {
        free(pair.first);
        free(pair.second);
        return -1;
    }
    values->items[values->count++] = pair;
    return 0;
}

static void strings_destroy(strings_t *values) {
    for (size_t index = 0; index < values->count; ++index)
        free(values->items[index]);
    free(values->items);
}

static void pairs_destroy(pairs_t *values) {
    for (size_t index = 0; index < values->count; ++index) {
        free(values->items[index].first);
        free(values->items[index].second);
    }
    free(values->items);
}

static void manifest_destroy(manifest_t *manifest) {
    strings_destroy(&manifest->hosts);
    pairs_destroy(&manifest->reservations);
    pairs_destroy(&manifest->lanes);
    pairs_destroy(&manifest->tests);
}

static int split_record(char *line, char **fields, size_t expected) {
    size_t count = 0;
    char *cursor = line;
    while (count < expected) {
        fields[count++] = cursor;
        char *tab = strchr(cursor, '\t');
        if (tab == NULL) break;
        *tab = '\0';
        cursor = tab + 1;
    }
    return count == expected && strchr(fields[expected - 1], '\t') == NULL;
}

static int load_manifest(const char *path, manifest_t *manifest, FILE *diagnostics) {
    FILE *input = fopen(path, "r");
    char line[16384];
    size_t line_number = 0;
    int result = 2;
    if (input == NULL) {
        fprintf(diagnostics, "lane-parity: cannot read %s: %s\n", path, strerror(errno));
        return 2;
    }
    while (fgets(line, sizeof line, input) != NULL) {
        ++line_number;
        size_t length = strlen(line);
        if (length == 0 || line[length - 1] != '\n') {
            fprintf(diagnostics, "lane-parity: malformed line %zu in %s\n", line_number, path);
            goto done;
        }
        line[--length] = '\0';
        if (length > 0 && line[length - 1] == '\r') line[--length] = '\0';
        char *fields[3] = {0};
        int added;
        if (strncmp(line, "host\t", 5) == 0 && split_record(line, fields, 2)) {
            added = valid_token(fields[1]) ? strings_add(&manifest->hosts, fields[1]) : 1;
        } else if (strncmp(line, "reserve\t", 8) == 0 && split_record(line, fields, 3)) {
            added = valid_token(fields[1]) && valid_token(fields[2])
                        ? pairs_add(&manifest->reservations, fields[1], fields[2])
                        : 1;
        } else if (strncmp(line, "lane\t", 5) == 0 && split_record(line, fields, 3)) {
            added = valid_token(fields[1]) && valid_token(fields[2]) ? pairs_add(&manifest->lanes, fields[1], fields[2])
                                                                     : 1;
        } else if (strncmp(line, "test\t", 5) == 0 && split_record(line, fields, 3)) {
            added = valid_token(fields[1]) && valid_token(fields[2]) ? pairs_add(&manifest->tests, fields[1], fields[2])
                                                                     : 1;
        } else {
            added = 1;
        }
        if (added != 0) {
            fprintf(diagnostics, "lane-parity: %s record at line %zu in %s\n",
                    added < 0 ? "cannot store" : "invalid or duplicate", line_number, path);
            goto done;
        }
    }
    if (ferror(input)) {
        fprintf(diagnostics, "lane-parity: cannot read %s: %s\n", path, strerror(errno));
        goto done;
    }
    result = 0;
done:
    fclose(input);
    return result;
}

static int supported_os(const char *host_os) {
    return strcmp(host_os, "Linux") == 0 || strcmp(host_os, "Darwin") == 0 || strcmp(host_os, "Windows") == 0;
}

int hl_lane_parity_check(const char *manifest_path, const char *host_os, const char *host_cpu, FILE *diagnostics) {
    manifest_t manifest = {0};
    char host[256];
    int result = 2;
    if (manifest_path == NULL || !valid_token(host_os) || !valid_token(host_cpu) || diagnostics == NULL) return 2;
    if (!supported_os(host_os)) {
        fprintf(diagnostics, "lane-parity: unsupported host OS %s\n", host_os);
        return 2;
    }
    int host_length = snprintf(host, sizeof host, "%s-%s", host_os, host_cpu);
    if (host_length < 0 || (size_t)host_length >= sizeof host) return 2;
    if (load_manifest(manifest_path, &manifest, diagnostics) != 0) goto done;
    if (!strings_contains(&manifest.hosts, host)) {
        fprintf(diagnostics, "lane-parity: %s is not declared in the manifest\n", host);
        goto done;
    }
    for (size_t index = 0; index < manifest.reservations.count; ++index) {
        if (!strings_contains(&manifest.hosts, manifest.reservations.items[index].first)) {
            fprintf(diagnostics, "lane-parity: %s reserves %s for an undeclared host\n",
                    manifest.reservations.items[index].first, manifest.reservations.items[index].second);
            goto done;
        }
    }

    size_t checked = 0;
    size_t missing = 0;
    for (size_t index = 0; index < manifest.lanes.count; ++index) {
        const pair_t *lane = &manifest.lanes.items[index];
        if (strcmp(lane->first, host_os) != 0) continue;
        int reserved = 0;
        int mine = 0;
        for (size_t reservation = 0; reservation < manifest.reservations.count; ++reservation) {
            const pair_t *entry = &manifest.reservations.items[reservation];
            if (strcmp(entry->second, lane->second) != 0) continue;
            reserved = 1;
            if (strcmp(entry->first, host) == 0) mine = 1;
        }
        if (reserved && !mine) continue;
        ++checked;
        int found = 0;
        for (size_t test = 0; test < manifest.tests.count; ++test)
            if (strcmp(manifest.tests.items[test].second, lane->second) == 0) {
                found = 1;
                break;
            }
        if (!found) {
            fprintf(diagnostics, "lane-parity: label %s selects zero tests on %s\n", lane->second, host);
            ++missing;
        }
    }
    if (checked == 0) {
        fprintf(diagnostics, "lane-parity: manifest has no applicable lanes for %s\n", host);
        result = 1;
    } else if (missing != 0) {
        result = 1;
    } else {
        fprintf(diagnostics, "lane-parity: %zu declared lanes are non-empty on %s\n", checked, host);
        result = 0;
    }
done:
    manifest_destroy(&manifest);
    return result;
}

#ifndef HL_LANE_PARITY_LIBRARY
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <manifest> <os> <cpu>\n", argv[0]);
        return 2;
    }
    return hl_lane_parity_check(argv[1], argv[2], argv[3], stdout);
}
#endif
