// hl-lint: compact C linter orchestrator for the engine source tree.
//
// The implementation avoids external dependencies and reports findings to stdout.
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <sys/stat.h>

#include "analyzers.h"
#include "cli.h"
#include "lint.h"
#include "policy.h"

#ifdef _WIN32
static char *xdup_format(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}
#endif

static bool has_ext(const char *path, const char *ext) {
    size_t path_length = strlen(path);
    size_t extension_length = strlen(ext);
    return path_length > extension_length && strcmp(path + path_length - extension_length, ext) == 0;
}

static bool is_source_file(const char *path) {
    return has_ext(path, ".c") || has_ext(path, ".h") || has_ext(path, ".m") || has_ext(path, ".mm");
}

static bool dir_should_skip(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || name[0] == '.' || strcmp(name, "build") == 0 ||
           strncmp(name, "build-", 6) == 0 || strcmp(name, "result") == 0 || strncmp(name, "result-", 7) == 0 ||
           strcmp(name, "hl_errmat_") == 0;
}

#ifdef _WIN32
static wchar_t *path_to_wide(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;
    if (count == 0) return NULL;
    wide = malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) == 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *path_from_wide(const wchar_t *path) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
    char *utf8;
    if (count == 0) return NULL;
    utf8 = malloc((size_t)count);
    if (utf8 == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, count, NULL, NULL) == 0) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

static void collect_recursive(const char *root, StringList *files) {
    wchar_t *wide = path_to_wide(root);
    DWORD attributes;
    if (wide == NULL) {
        fprintf(stdout, "warn: invalid UTF-8 path `%s`\n", root);
        return;
    }
    attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fprintf(stdout, "warn: cannot inspect path `%s` (Windows error %lu)\n", root, (unsigned long)GetLastError());
        free(wide);
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        size_t length = wcslen(wide);
        wchar_t *pattern = malloc((length + 3) * sizeof(*pattern));
        WIN32_FIND_DATAW entry;
        HANDLE search;
        if (pattern == NULL) {
            free(wide);
            return;
        }
        memcpy(pattern, wide, length * sizeof(*pattern));
        pattern[length++] = L'\\';
        pattern[length++] = L'*';
        pattern[length] = L'\0';
        search = FindFirstFileW(pattern, &entry);
        free(pattern);
        if (search == INVALID_HANDLE_VALUE) {
            fprintf(stdout, "warn: cannot open directory `%s` (Windows error %lu)\n", root,
                    (unsigned long)GetLastError());
            free(wide);
            return;
        }
        do {
            char *name = path_from_wide(entry.cFileName);
            char *child;
            if (name == NULL || dir_should_skip(name)) {
                free(name);
                continue;
            }
            child = xdup_format("%s/%s", root, name);
            free(name);
            if (child == NULL) continue;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                collect_recursive(child, files);
            } else if (is_source_file(child)) {
                hl_lint_list_append(files, child);
            }
            free(child);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    } else if (is_source_file(root)) {
        hl_lint_list_append(files, root);
    }
    free(wide);
}
#else
static void collect_recursive(const char *root, StringList *files) {
    struct stat st;
    if (stat(root, &st) != 0) {
        fprintf(stdout, "warn: cannot stat path `%s`: %s\n", root, strerror(errno));
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(root);
        if (!dir) {
            fprintf(stdout, "warn: cannot open directory `%s`: %s\n", root, strerror(errno));
            return;
        }

        while (true) {
            struct dirent *ent = readdir(dir);
            if (!ent) break;
            const char *name = ent->d_name;
            if (dir_should_skip(name)) continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", root, name);
            if (stat(child, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                collect_recursive(child, files);
            } else if (S_ISREG(st.st_mode) && is_source_file(child)) {
                hl_lint_list_append(files, child);
            }
        }
        closedir(dir);
        return;
    }

    if (S_ISREG(st.st_mode) && is_source_file(root)) { hl_lint_list_append(files, root); }
}
#endif

int main(int argc, char **argv) {
    LintConfig cfg;
    hl_lint_config_init(&cfg);
    HlLintCliResult cli_result = hl_lint_cli_parse(&cfg, argc, argv);
    if (cli_result != HL_LINT_CLI_RUN) {
        hl_lint_config_destroy(&cfg);
        return cli_result == HL_LINT_CLI_EXIT_SUCCESS ? 0 : 2;
    }

    StringList all_files;
    hl_lint_list_init(&all_files);

    if (cfg.source_files.count == 0 && cfg.source_dirs.count == 0) {
        collect_recursive("src", &all_files);
    } else {
        for (size_t i = 0; i < cfg.source_files.count; i++) {
            hl_lint_list_append(&all_files, cfg.source_files.items[i]);
        }
        for (size_t i = 0; i < cfg.source_dirs.count; i++) {
            collect_recursive(cfg.source_dirs.items[i], &all_files);
        }
    }

    LintStats stats = {0, 0};
    if (all_files.count == 0) {
        fprintf(stdout, "%s: no source files matched\n", cfg.strict ? "error" : "warn");
        if (cfg.strict)
            stats.errors++;
        else
            stats.warnings++;
    }

    if (cfg.allow_getenv_files.count == 0) {
        // Engine currently centralizes env-var reads in environment.c.
        hl_lint_list_append(&cfg.allow_getenv_files, "src/core/environment.c");
    }

    const StringList *clang_tidy_files = cfg.clang_tidy_files.count > 0 ? &cfg.clang_tidy_files : &all_files;
    int rc = hl_lint_analyzers_run(&cfg, &all_files, clang_tidy_files, &stats);
    if (cfg.run_custom) hl_lint_policy_run(&cfg, &all_files, &stats);

    if (stats.errors == 0 && !cfg.strict && stats.warnings > 0) {
        fprintf(stdout, "hl-lint: warnings=%ld (non-fatal)\n", stats.warnings);
        rc = 0;
    } else {
        fprintf(stdout, "hl-lint: warnings=%ld errors=%ld\n", stats.warnings, stats.errors);
        if (stats.errors > 0)
            rc = 1;
        else if (cfg.strict && stats.warnings > 0)
            rc = 1;
    }

    if (cfg.strict) { fprintf(stdout, "hl-lint: strict mode enabled\n"); }

    hl_lint_list_destroy(&all_files);
    hl_lint_config_destroy(&cfg);
    return rc;
}
