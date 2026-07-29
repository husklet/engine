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
#include "lint.h"
#include "policy.h"

static void list_init(StringList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

static char *string_duplicate(const char *value) {
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, value, size);
    return copy;
}

static bool list_contains(const StringList *list, const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0) return true;
    }
    return false;
}

static void list_append(StringList *list, const char *value) {
    if (list_contains(list, value)) return;
    if (list->count + 1 > list->cap) {
        size_t grow = list->cap == 0 ? 64 : list->cap * 2;
        char **next = realloc(list->items, grow * sizeof(char *));
        if (!next) {
            fprintf(stdout, "error: out of memory while collecting paths\n");
            exit(1);
        }
        list->items = next;
        list->cap = grow;
    }
    list->items[list->count] = string_duplicate(value);
    if (list->items[list->count] == NULL) {
        fprintf(stdout, "error: out of memory while collecting paths\n");
        exit(1);
    }
    list->count++;
}

static void list_free(StringList *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

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
                list_append(files, child);
            }
            free(child);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    } else if (is_source_file(root)) {
        list_append(files, root);
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
                list_append(files, child);
            }
        }
        closedir(dir);
        return;
    }

    if (S_ISREG(st.st_mode) && is_source_file(root)) { list_append(files, root); }
}
#endif

static void print_usage(const char *prog) {
    fprintf(stdout, "usage: %s [options] [--source-dir <path>]... [--source-file <path>]...\n", prog);
    fprintf(stdout, "options:\n");
    fprintf(stdout, "  --source-dir PATH         add recursive source directory (default: src)\n");
    fprintf(stdout, "  --source-file PATH        add explicit source file\n");
    fprintf(stdout, "  --clang-tidy-source-file PATH analyze a compiled translation unit\n");
    fprintf(stdout, "  --include-dir PATH        add include directory for cppcheck\n");
    fprintf(stdout, "  --compile-commands-dir DIR directory containing compile_commands.json for clang-tidy\n");
    fprintf(stdout, "  --clang-format-bin PATH   clang-format path\n");
    fprintf(stdout, "  --clang-tidy-bin PATH     clang-tidy path\n");
    fprintf(stdout, "  --cppcheck-bin PATH       cppcheck path\n");
    fprintf(stdout,
            "  --clang-tidy-checks LIST  clang-tidy checks (default: bugprone-*,clang-analyzer-*,performance-*)\n");
    fprintf(stdout, "  --max-function-lines N    opt in to lexical function-length warnings\n");
    fprintf(stdout, "  --max-nesting N           opt in to lexical brace-depth warnings\n");
    fprintf(stdout, "  --max-line-length N       opt in to line-length warnings\n");
    fprintf(stdout, "  --strict                  fail on warnings as errors\n");
    fprintf(stdout, "  --skip-clang-format       disable clang-format stage\n");
    fprintf(stdout, "  --skip-clang-tidy         disable clang-tidy stage\n");
    fprintf(stdout, "  --skip-cppcheck           disable cppcheck stage\n");
    fprintf(stdout, "  --skip-custom             disable custom heuristics stage\n");
    fprintf(stdout, "  --allow-getenv-file PATH  allow direct environment access in this source file\n");
    fprintf(stdout, "  --allow-stdio-file PATH   temporarily allow direct console output in this file\n");
    fprintf(stdout, "  --allow-shell-file PATH   temporarily allow shell execution in this file\n");
    fprintf(stdout, "  --clang-format-check/--clang-format-no-check\n");
    fprintf(stdout, "  --clang-tidy-check/--clang-tidy-no-check\n");
    fprintf(stdout, "  --cppcheck-check/--cppcheck-no-check\n");
    fprintf(stdout, "  --help                    show this help\n");
}

static int parse_positive_int(const char *value) {
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < 0 || parsed > 1000000) {
        fprintf(stdout, "error: invalid integer `%s`\n", value);
        exit(1);
    }
    return (int)parsed;
}

int main(int argc, char **argv) {
    LintConfig cfg = {
        .max_function_lines = 0,
        .max_nesting_depth = 0,
        .max_line_length = 0,
        .run_clang_format = true,
        .run_clang_tidy = true,
        .run_cppcheck = true,
        .run_custom = true,
        .strict = false,
        .clang_tidy_checks = "clang-analyzer-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,"
                             "bugprone-assignment-in-if-condition,bugprone-branch-clone,bugprone-inc-dec-in-conditions,"
                             "bugprone-infinite-loop,bugprone-not-null-terminated-result,bugprone-posix-return,"
                             "bugprone-signal-handler,bugprone-sizeof-expression,bugprone-suspicious-memory-comparison,"
                             "bugprone-suspicious-memset-usage,bugprone-undefined-memory-manipulation",
    };

    list_init(&cfg.source_files);
    list_init(&cfg.source_dirs);
    list_init(&cfg.clang_tidy_files);
    list_init(&cfg.include_dirs);
    list_init(&cfg.allow_getenv_files);
    list_init(&cfg.allow_stdio_files);
    list_init(&cfg.allow_shell_files);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            list_free(&cfg.source_files);
            list_free(&cfg.source_dirs);
            list_free(&cfg.clang_tidy_files);
            list_free(&cfg.include_dirs);
            list_free(&cfg.allow_getenv_files);
            list_free(&cfg.allow_stdio_files);
            list_free(&cfg.allow_shell_files);
            return 0;
        } else if (strcmp(arg, "--source-dir") == 0 || strcmp(arg, "--src") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.source_dirs, argv[++i]);
        } else if (strcmp(arg, "--source-file") == 0 || strcmp(arg, "--file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.source_files, argv[++i]);
        } else if (strcmp(arg, "--clang-tidy-source-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.clang_tidy_files, argv[++i]);
        } else if (strcmp(arg, "--include-dir") == 0 || strcmp(arg, "-I") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.include_dirs, argv[++i]);
        } else if (strcmp(arg, "--compile-commands-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            cfg.compile_db_dir = argv[++i];
        } else if (strcmp(arg, "--clang-format-bin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            cfg.clang_format_bin = argv[++i];
        } else if (strcmp(arg, "--clang-tidy-bin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            cfg.clang_tidy_bin = argv[++i];
        } else if (strcmp(arg, "--cppcheck-bin") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            cfg.cppcheck_bin = argv[++i];
        } else if (strcmp(arg, "--clang-tidy-checks") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <checks>\n", arg);
                return 2;
            }
            cfg.clang_tidy_checks = argv[++i];
        } else if (strcmp(arg, "--max-function-lines") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <value>\n", arg);
                return 2;
            }
            cfg.max_function_lines = parse_positive_int(argv[++i]);
        } else if (strcmp(arg, "--max-nesting") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <value>\n", arg);
                return 2;
            }
            cfg.max_nesting_depth = parse_positive_int(argv[++i]);
        } else if (strcmp(arg, "--max-line-length") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <value>\n", arg);
                return 2;
            }
            cfg.max_line_length = parse_positive_int(argv[++i]);
        } else if (strcmp(arg, "--strict") == 0) {
            cfg.strict = true;
        } else if (strcmp(arg, "--skip-clang-format") == 0) {
            cfg.run_clang_format = false;
        } else if (strcmp(arg, "--skip-clang-tidy") == 0 || strcmp(arg, "--clang-tidy-no-check") == 0) {
            cfg.run_clang_tidy = false;
        } else if (strcmp(arg, "--skip-cppcheck") == 0 || strcmp(arg, "--cppcheck-no-check") == 0) {
            cfg.run_cppcheck = false;
        } else if (strcmp(arg, "--skip-custom") == 0) {
            cfg.run_custom = false;
        } else if (strcmp(arg, "--allow-getenv-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.allow_getenv_files, argv[++i]);
        } else if (strcmp(arg, "--allow-stdio-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.allow_stdio_files, argv[++i]);
        } else if (strcmp(arg, "--allow-shell-file") == 0) {
            if (i + 1 >= argc) {
                fprintf(stdout, "error: %s expects <path>\n", arg);
                return 2;
            }
            list_append(&cfg.allow_shell_files, argv[++i]);
        } else if (strcmp(arg, "--clang-format-check") == 0) {
            cfg.run_clang_format = true;
        } else if (strcmp(arg, "--clang-format-no-check") == 0) {
            cfg.run_clang_format = false;
        } else if (strcmp(arg, "--clang-tidy-check") == 0) {
            cfg.run_clang_tidy = true;
        } else if (strcmp(arg, "--clang-tidy-no-check") == 0) {
            cfg.run_clang_tidy = false;
        } else if (strcmp(arg, "--cppcheck-check") == 0) {
            cfg.run_cppcheck = true;
        } else if (strcmp(arg, "--cppcheck-no-check") == 0) {
            cfg.run_cppcheck = false;
        } else {
            fprintf(stdout, "error: unknown option `%s`\n", arg);
            print_usage(argv[0]);
            list_free(&cfg.source_files);
            list_free(&cfg.source_dirs);
            list_free(&cfg.clang_tidy_files);
            list_free(&cfg.include_dirs);
            list_free(&cfg.allow_getenv_files);
            list_free(&cfg.allow_stdio_files);
            list_free(&cfg.allow_shell_files);
            return 2;
        }
    }

    StringList all_files;
    list_init(&all_files);

    if (cfg.source_files.count == 0 && cfg.source_dirs.count == 0) {
        collect_recursive("src", &all_files);
    } else {
        for (size_t i = 0; i < cfg.source_files.count; i++) {
            list_append(&all_files, cfg.source_files.items[i]);
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
        list_append(&cfg.allow_getenv_files, "src/core/environment.c");
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

    list_free(&all_files);
    list_free(&cfg.source_files);
    list_free(&cfg.source_dirs);
    list_free(&cfg.clang_tidy_files);
    list_free(&cfg.include_dirs);
    list_free(&cfg.allow_getenv_files);
    list_free(&cfg.allow_stdio_files);
    list_free(&cfg.allow_shell_files);
    return rc;
}
