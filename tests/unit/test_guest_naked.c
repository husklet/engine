/*
 * GCC has no AArch64 naked-function implementation: the attribute is accepted
 * but ignored, so fixed-layout test code must be emitted as an assembly symbol.
 * Keep the compat corpus from silently reintroducing a patchable C function.
 */
#include "test.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

static int findings;

#define SCANNER_SOURCE "tests/unit/test_guest_naked.c"

static int comment_line(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    return line[0] == '*' || (line[0] == '/' && (line[1] == '/' || line[1] == '*'));
}

static void scan_stream(FILE *file, const char *path) {
    char line[4096];
    unsigned number = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        char *attribute = strstr(line, "__attribute__");
        number++;
        if (attribute == NULL || strstr(attribute, "naked") == NULL) continue;
        if (comment_line(line)) continue;
        fprintf(stderr,
                "%s:%u: __attribute__((naked)) is ignored by GCC on AArch64; "
                "use a top-level __asm__ symbol instead\n",
                path, number);
        findings++;
    }
}

static int scan_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "test_guest_naked: cannot open %s\n", path);
        return -1;
    }
    scan_stream(file, path);
    fclose(file);
    return 0;
}

static int scan_tree(const char *root) {
    DIR *directory = opendir(root);
    struct dirent *entry;
    if (directory == NULL) {
        fprintf(stderr, "test_guest_naked: cannot open directory %s\n", root);
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char path[4096];
        struct stat info;
        size_t length;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", root, entry->d_name) >= sizeof path) continue;
        if (stat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            if (scan_tree(path) != 0) {
                closedir(directory);
                return -1;
            }
            continue;
        }
        if (!S_ISREG(info.st_mode)) continue;
        length = strlen(entry->d_name);
        if (length < 3) continue;
        if (strcmp(entry->d_name + length - 2, ".c") != 0 &&
            strcmp(entry->d_name + length - 2, ".h") != 0)
            continue;
        if (strcmp(path, SCANNER_SOURCE) == 0) continue;
        if (scan_file(path) != 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    return 0;
}

int main(void) {
    FILE *sample = tmpfile();
    HL_CHECK(sample != NULL);
    fputs("static void ok(void) {}\n"
          "__attribute__((naked, noinline)) static unsigned patchable(void) {\n"
          "    __asm__ volatile(\"mov w0,#11\\n\\tret\");\n"
          "}\n",
          sample);
    rewind(sample);
    scan_stream(sample, "<self-check>");
    fclose(sample);
    HL_CHECK(findings == 1);

    findings = 0;
    HL_CHECK(scan_tree("tests") == 0);
    HL_CHECK(findings == 0);
    return EXIT_SUCCESS;
}
