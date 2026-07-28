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

#include "process.h"

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StringList;

typedef struct {
    StringList source_files;
    StringList source_dirs;
    StringList include_dirs;
    const char *clang_format_bin;
    const char *clang_tidy_bin;
    const char *cppcheck_bin;
    const char *compile_db_dir;
    const char *clang_tidy_checks;
    StringList allow_getenv_files;
    StringList allow_stdio_files;
    StringList allow_shell_files;
    int max_function_lines;
    int max_nesting_depth;
    int max_line_length;
    bool run_clang_format;
    bool run_clang_tidy;
    bool run_cppcheck;
    bool run_custom;
    bool strict;
} LintConfig;

typedef struct {
    long warnings;
    long errors;
} LintStats;

typedef struct {
    bool in_block_comment;
    bool in_string;
    bool in_char;
} ScanState;

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

static const char *skip_space(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool has_ext(const char *path, const char *ext) {
    size_t lp = strlen(path), le = strlen(ext);
    return lp > le && strcmp(path + lp - le, ext) == 0;
}

static bool path_matches(const char *path, const char *needle) {
    if (!path || !needle) return false;
    if (strcmp(path, needle) == 0) return true;
    size_t lp = strlen(path);
    size_t ln = strlen(needle);
    if (lp <= ln) return false;
    return (path[lp - ln - 1] == '/' && strcmp(path + lp - ln, needle) == 0);
}

static bool is_getenv_allowed_in_file(const LintConfig *cfg, const char *path) {
    if (cfg->allow_getenv_files.count == 0) return false;
    for (size_t i = 0; i < cfg->allow_getenv_files.count; i++) {
        if (path_matches(path, cfg->allow_getenv_files.items[i])) return true;
    }
    return false;
}

static bool is_stdio_allowed_in_file(const LintConfig *cfg, const char *path) {
    for (size_t i = 0; i < cfg->allow_stdio_files.count; i++) {
        if (path_matches(path, cfg->allow_stdio_files.items[i])) return true;
    }
    return false;
}

static bool is_shell_allowed_in_file(const LintConfig *cfg, const char *path) {
    for (size_t i = 0; i < cfg->allow_shell_files.count; i++) {
        if (path_matches(path, cfg->allow_shell_files.items[i])) return true;
    }
    return false;
}

static bool is_source_file(const char *path) {
    return has_ext(path, ".c") || has_ext(path, ".h");
}

static bool dir_should_skip(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || name[0] == '.'
           || strcmp(name, "build") == 0 || strncmp(name, "build-", 6) == 0
           || strcmp(name, "result") == 0 || strncmp(name, "result-", 7) == 0
           || strcmp(name, "hl_errmat_") == 0;
}

#ifdef _WIN32
static wchar_t *path_to_wide(const char *path) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                                    NULL, 0);
    wchar_t *wide;
    if (count == 0) return NULL;
    wide = malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            count) == 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *path_from_wide(const wchar_t *path) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                   NULL, 0, NULL, NULL);
    char *utf8;
    if (count == 0) return NULL;
    utf8 = malloc((size_t)count);
    if (utf8 == NULL) return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8,
                            count, NULL, NULL) == 0) {
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
        fprintf(stdout, "warn: cannot inspect path `%s` (Windows error %lu)\n",
                root, (unsigned long)GetLastError());
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
            fprintf(stdout, "warn: cannot open directory `%s` (Windows error %lu)\n",
                    root, (unsigned long)GetLastError());
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

    if (S_ISREG(st.st_mode) && is_source_file(root)) {
        list_append(files, root);
    }
}
#endif

static void emit_diag(const char *severity, const char *path, int line, int col, const char *rule, const char *message) {
    if (line > 0) {
        fprintf(stdout, "%s:%d:%d: [%s] %s: %s\n", path, line, col, severity, rule, message);
    } else {
        fprintf(stdout, "%s: [%s] %s: %s\n", path, severity, rule, message);
    }
}

static int run_command_argv(const char *label, const char *const argv[],
                            LintStats *stats) {
    HlLintProcessResult result;
    if (hl_lint_process_run(argv, 0, &result) != 0) {
#ifdef _WIN32
        fprintf(stdout, "error: %s failed to execute (Windows error %d)\n",
                label, result.platform_error);
#else
        fprintf(stdout, "error: %s failed to execute: %s\n", label,
                strerror(result.platform_error));
#endif
        stats->errors++;
        return 1;
    }
    if (result.output_size > 0) {
        fwrite(result.output, 1, result.output_size, stdout);
    }
    if (result.output_truncated) {
        fprintf(stdout, "warn: %s output truncated\n", label);
    }
    int rc = result.exit_code;
    if (result.term_signal != 0) {
        fprintf(stdout, "error: %s terminated by signal %d\n", label,
                result.term_signal);
        stats->errors++;
        rc = 1;
    }
    hl_lint_process_result_destroy(&result);
    return rc;
}

static int run_clang_format(const LintConfig *cfg, const StringList *files, LintStats *stats) {
    int rc = 0;
    if (!cfg->run_clang_format) return 0;
    if (!cfg->clang_format_bin) {
        if (cfg->strict) {
            fprintf(stdout, "error: clang-format not configured\n");
            stats->errors++;
            return 1;
        }
        fprintf(stdout, "warn: skipping clang-format (binary not configured)\n");
        return 0;
    }

    for (size_t i = 0; i < files->count; i++) {
        const char *file = files->items[i];
        const char *const argv[] = {
            cfg->clang_format_bin,
            "--dry-run",
            "--Werror",
            "--style=file",
            "--ferror-limit=1",
            file,
            NULL
        };
        int c = run_command_argv("clang-format", argv, stats);
        if (c != 0) {
            if (cfg->strict) {
                emit_diag("error", file, 0, 0, "clang-format", "formatting violation");
                stats->errors++;
                rc = 1;
            } else {
                emit_diag("warn", file, 0, 0, "clang-format", "formatting violation");
                stats->warnings++;
            }
        }
        if (cfg->strict && rc != 0) return 1;
        if (cfg->strict) rc = (rc != 0) ? rc : c;
        else rc = 0;
    }
    return rc;
}

static int run_clang_tidy(const LintConfig *cfg, const StringList *files, LintStats *stats) {
    int rc = 0;
    if (!cfg->run_clang_tidy) return 0;
    if (!cfg->clang_tidy_bin) {
        if (cfg->strict) {
            fprintf(stdout, "error: clang-tidy not configured\n");
            stats->errors++;
            return 1;
        }
        fprintf(stdout, "warn: skipping clang-tidy (binary not configured)\n");
        return 0;
    }
    char *compile_db = cfg->compile_db_dir
        ? xdup_format("%s/compile_commands.json", cfg->compile_db_dir)
        : NULL;
    if (!compile_db ||
#ifdef _WIN32
        _access(compile_db, 0) != 0) {
#else
        access(compile_db, F_OK) != 0) {
#endif
        if (cfg->strict) {
            fprintf(stdout, "error: compile database missing for clang-tidy: %s\n",
                    compile_db ? compile_db : "<unset>");
            stats->errors++;
            free(compile_db);
            return 1;
        }
        fprintf(stdout, "warn: skipping clang-tidy (missing compile db)\n");
        free(compile_db);
        return 0;
    }
    free(compile_db);

    for (size_t i = 0; i < files->count; i++) {
        const char *file = files->items[i];
        if (!has_ext(file, ".c")) continue;
        char *checks = xdup_format(
            "--checks=%s",
            cfg->clang_tidy_checks
                ? cfg->clang_tidy_checks
                : "bugprone-*,clang-analyzer-*,performance-*");
        if (!checks) {
            fprintf(stdout, "error: out of memory building clang-tidy command\n");
            return 1;
        }
        const char *const argv[] = {
            cfg->clang_tidy_bin,
            "--quiet",
            "-p",
            cfg->compile_db_dir,
            checks,
            "--extra-arg=-std=c11",
            "--warnings-as-errors=*",
            file,
            NULL
        };
        int c = run_command_argv("clang-tidy", argv, stats);
        free(checks);
        if (c != 0) {
            emit_diag("warn", file, 0, 0, "clang-tidy", "diagnostic(s) reported");
            if (cfg->strict) {
                stats->errors++;
                return 1;
            }
            stats->warnings++;
            c = 0;
        }
        rc = (rc != 0) ? rc : c;
    }
    return rc;
}

static int run_cppcheck(const LintConfig *cfg, const StringList *files, LintStats *stats) {
    int rc = 0;
    if (!cfg->run_cppcheck) return 0;
    if (!cfg->cppcheck_bin) {
        if (cfg->strict) {
            fprintf(stdout, "error: cppcheck not configured\n");
            stats->errors++;
            return 1;
        }
        fprintf(stdout, "warn: skipping cppcheck (binary not configured)\n");
        return 0;
    }

    for (size_t i = 0; i < files->count; i++) {
        const char *file = files->items[i];
        if (!has_ext(file, ".c") && !has_ext(file, ".h")) continue;

        size_t argument_count = 9 + (cfg->include_dirs.count * 2);
        const char **argv = calloc(argument_count, sizeof *argv);
        if (!argv) {
            fprintf(stdout, "error: out of memory building cppcheck command\n");
            return 1;
        }
        size_t a = 0;
        argv[a++] = cfg->cppcheck_bin;
        argv[a++] = "--quiet";
        argv[a++] = "--std=c11";
        argv[a++] = "--enable=warning,performance,style,portability,information";
        argv[a++] = "--inconclusive";
        argv[a++] = "--suppress=missingIncludeSystem";
        argv[a++] = "--error-exitcode=1";
        for (size_t d = 0; d < cfg->include_dirs.count; d++) {
            argv[a++] = "-I";
            argv[a++] = cfg->include_dirs.items[d];
        }
        argv[a++] = file;
        argv[a] = NULL;

        int c = run_command_argv("cppcheck", argv, stats);
        free(argv);
        if (c != 0) {
            emit_diag("warn", file, 0, 0, "cppcheck", "diagnostic(s) reported");
            if (cfg->strict) {
                stats->errors++;
                return 1;
            }
            stats->warnings++;
            c = 0;
        }
        rc = (rc != 0) ? rc : c;
    }
    return rc;
}

static bool word_starts_token(const char *line, const char *found, size_t len) {
    size_t offset = (size_t)(found - line);
    bool left_ok = offset == 0 || !(isalnum((unsigned char)line[offset - 1]) || line[offset - 1] == '_');
    bool right_ok = !isalnum((unsigned char)found[len]) && found[len] != '_';
    return left_ok && right_ok;
}

static bool line_has_word(const char *line, const char *word) {
    size_t wl = strlen(word);
    const char *p = line;
    while (true) {
        const char *m = strstr(p, word);
        if (!m) return false;
        if (word_starts_token(p, m, wl)) return true;
        p = m + wl;
    }
}

static bool line_has_direct_console_output(const char *line) {
    if (line_has_word(line, "printf")
        || line_has_word(line, "vprintf")
        || line_has_word(line, "puts")
        || line_has_word(line, "perror")) {
        return true;
    }
    if ((line_has_word(line, "fprintf") || line_has_word(line, "vfprintf")
         || line_has_word(line, "fputs"))
        && (line_has_word(line, "stderr") || line_has_word(line, "stdout"))) {
        return true;
    }
    return false;
}

static bool line_has_control_prefix(const char *line) {
    const char *s = skip_space(line);
    static const char *const k_controls[] = {
        "if", "else", "for", "while", "switch", "case", "default", "do", "goto",
        "sizeof", "struct", "union", "enum", "typedef", "return", "asm", "asm volatile",
        NULL};
    for (size_t i = 0; k_controls[i]; i++) {
        const char *kw = k_controls[i];
        size_t klen = strlen(kw);
        if (strncmp(s, kw, klen) == 0 && (isspace((unsigned char)s[klen]) || s[klen] == '(' || s[klen] == '\0')) {
            return true;
        }
    }
    return false;
}

static bool looks_like_function_signature(const char *sig) {
    if (!sig) return false;
    const char *s = skip_space(sig);
    if (!*s || *s == '#') return false;
    if (!strchr(s, '(') || !strchr(s, ')')) return false;
    if (strchr(s, ';')) return false;
    if (line_has_control_prefix(s)) return false;
    if (strstr(s, "static_assert") || strstr(s, "typedef")) return false;
    if (strstr(s, "sizeof(")) return false;
    if (strstr(s, "alignas(")) return false;
    return true;
}

static void strip_trailing_newline(char *line) {
    size_t len = strlen(line);
    if (len == 0) return;
    if (line[len - 1] == '\n') line[len - 1] = '\0';
}

static void sanitize_for_parse(const char *src, char *dst, size_t dst_len, ScanState *state) {
    size_t j = 0;
    bool in_line_comment = false;
    for (size_t i = 0; i < strlen(src) && j + 1 < dst_len; i++) {
        char c = src[i];
        char cnext = src[i + 1];
        if (in_line_comment) break;

        if (state->in_block_comment) {
            if (c == '*' && cnext == '/') {
                state->in_block_comment = false;
                i++;
            }
            continue;
        }
        if (state->in_string) {
            if (c == '\\' && cnext != '\0') {
                i++;
                continue;
            }
            if (c == '\"') state->in_string = false;
            continue;
        }
        if (state->in_char) {
            if (c == '\\' && cnext != '\0') {
                i++;
                continue;
            }
            if (c == '\'') state->in_char = false;
            continue;
        }

        if (c == '/' && cnext == '/') {
            in_line_comment = true;
            continue;
        }
        if (c == '/' && cnext == '*') {
            state->in_block_comment = true;
            i++;
            continue;
        }
        if (c == '\"') {
            state->in_string = true;
            continue;
        }
        if (c == '\'') {
            state->in_char = true;
            continue;
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static void check_file_custom(const LintConfig *cfg, const char *path, LintStats *stats) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        emit_diag("error", path, 0, 0, "fs", "failed to open file for custom checks");
        stats->errors++;
        return;
    }

    char raw[8192];
    char clean[8192];
    ScanState state = {false, false, false};
    bool in_function = false;
    int brace_depth = 0;
    int func_base_depth = 0;
    int func_start_line = 0;
    int func_lines = 0;
    int func_max_nesting = 0;
    bool sig_collecting = false;
    int sig_start_line = 0;
    char signature[8192] = {0};

    int lineno = 0;
    while (fgets(raw, sizeof(raw), fp)) {
        lineno++;
        strip_trailing_newline(raw);
        sanitize_for_parse(raw, clean, sizeof(clean), &state);

        size_t line_len = strlen(raw);
        if (cfg->max_line_length > 0
            && line_len > (size_t)cfg->max_line_length) {
            emit_diag("warn", path, lineno, 1, "style", "long line");
            stats->warnings++;
        }

        if (line_has_word(clean, "getenv")) {
            if (!is_getenv_allowed_in_file(cfg, path)) {
                emit_diag("error", path, lineno, 1, "api",
                          "getenv usage is only allowed in explicitly whitelisted files");
                stats->errors++;
            }
        }
        if (line_has_direct_console_output(clean)
            && !is_stdio_allowed_in_file(cfg, path)) {
            emit_diag("error", path, lineno, 1, "logging",
                      "direct console output is forbidden; use tagged logging");
            stats->errors++;
        }
        if ((line_has_word(clean, "system") || line_has_word(clean, "popen"))
            && !is_shell_allowed_in_file(cfg, path)) {
            emit_diag("error", path, lineno, 1, "process",
                      "shell execution is forbidden; launch an argv vector directly");
            stats->errors++;
        }

        if (!in_function && brace_depth == 0) {
            if (sig_collecting) {
                if (sig_start_line == 0) sig_start_line = lineno;
                if (signature[0] == '\0') {
                    snprintf(signature, sizeof(signature), "%s", clean);
                } else {
                    size_t used = strnlen(signature, sizeof(signature));
                    if (used + 1 < sizeof(signature)) {
                        size_t room = sizeof(signature) - used - 1;
                        size_t need = strlen(clean);
                        if (need + 1 > room) need = room - 1;
                        signature[used] = ' ';
                        signature[used + 1] = '\0';
                        strncat(signature, clean, need);
                    }
                }
                if (strchr(clean, ';')) {
                    signature[0] = '\0';
                    sig_collecting = false;
                }
            } else if (strchr(clean, '(') && !line_has_control_prefix(clean) && strncmp(clean, "#", 1) != 0) {
                signature[0] = '\0';
                snprintf(signature, sizeof(signature), "%s", clean);
                sig_start_line = lineno;
                sig_collecting = true;
            }

            char *brace = strchr(clean, '{');
            if (sig_collecting && brace) {
                *brace = '\0';
                if (looks_like_function_signature(signature)) {
                    in_function = true;
                    func_base_depth = brace_depth;
                    func_start_line = sig_start_line;
                    func_lines = 1;
                    func_max_nesting = 1;
                } else {
                    sig_collecting = false;
                    signature[0] = '\0';
                }
            }
        }

        if (in_function && lineno != func_start_line) func_lines++;

        for (size_t i = 0; i < strlen(clean); i++) {
            char c = clean[i];
            if (c == '{') {
                brace_depth++;
                if (in_function) {
                    int nesting = brace_depth - func_base_depth;
                    if (nesting > func_max_nesting) func_max_nesting = nesting;
                }
                continue;
            }
            if (c == '}') {
                if (brace_depth > 0) brace_depth--;
                if (in_function && brace_depth < func_base_depth + 1) {
                    if (cfg->max_function_lines > 0
                        && func_lines > cfg->max_function_lines) {
                        emit_diag("warn", path, func_start_line, 1, "complexity", "function exceeds max lines");
                        stats->warnings++;
                    }
                    if (cfg->max_nesting_depth > 0
                        && func_max_nesting > cfg->max_nesting_depth) {
                        emit_diag("warn", path, func_start_line, 1, "complexity", "function exceeds max nesting depth");
                        stats->warnings++;
                    }
                    in_function = false;
                    break;
                }
            }
        }
    }

    fclose(fp);
}

static void run_custom_checks(const LintConfig *cfg, const StringList *files, LintStats *stats) {
    if (!cfg->run_custom) return;
    for (size_t i = 0; i < files->count; i++) {
        check_file_custom(cfg, files->items[i], stats);
    }
}

static void print_usage(const char *prog) {
    fprintf(stdout, "usage: %s [options] [--source-dir <path>]... [--source-file <path>]...\n", prog);
    fprintf(stdout, "options:\n");
    fprintf(stdout, "  --source-dir PATH         add recursive source directory (default: src)\n");
    fprintf(stdout, "  --source-file PATH        add explicit source file\n");
    fprintf(stdout, "  --include-dir PATH        add include directory for cppcheck\n");
    fprintf(stdout, "  --compile-commands-dir DIR directory containing compile_commands.json for clang-tidy\n");
    fprintf(stdout, "  --clang-format-bin PATH   clang-format path\n");
    fprintf(stdout, "  --clang-tidy-bin PATH     clang-tidy path\n");
    fprintf(stdout, "  --cppcheck-bin PATH       cppcheck path\n");
    fprintf(stdout, "  --clang-tidy-checks LIST  clang-tidy checks (default: bugprone-*,clang-analyzer-*,performance-*)\n");
    fprintf(stdout, "  --max-function-lines N    opt in to lexical function-length warnings\n");
    fprintf(stdout, "  --max-nesting N           opt in to lexical brace-depth warnings\n");
    fprintf(stdout, "  --max-line-length N       opt in to line-length warnings\n");
    fprintf(stdout, "  --strict                  fail on warnings as errors\n");
    fprintf(stdout, "  --skip-clang-format       disable clang-format stage\n");
    fprintf(stdout, "  --skip-clang-tidy         disable clang-tidy stage\n");
    fprintf(stdout, "  --skip-cppcheck           disable cppcheck stage\n");
    fprintf(stdout, "  --skip-custom             disable custom heuristics stage\n");
    fprintf(stdout, "  --allow-getenv-file PATH  allow getenv() usage in this source file\n");
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
        .clang_tidy_checks = "bugprone-*,clang-analyzer-*,performance-*",
    };

    list_init(&cfg.source_files);
    list_init(&cfg.source_dirs);
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

    if (all_files.count == 0) {
        fprintf(stdout, "warn: no source files matched\n");
    }

    if (cfg.allow_getenv_files.count == 0) {
        // Engine currently centralizes env-var reads in environment.c.
        list_append(&cfg.allow_getenv_files, "src/core/environment.c");
    }

    LintStats stats = {0, 0};
    int rc = 0;
    if (cfg.run_clang_format) rc = run_clang_format(&cfg, &all_files, &stats);
    if (rc == 0 && cfg.run_clang_tidy) rc = run_clang_tidy(&cfg, &all_files, &stats);
    if (rc == 0 && cfg.run_cppcheck) rc = run_cppcheck(&cfg, &all_files, &stats);
    if (cfg.run_custom) run_custom_checks(&cfg, &all_files, &stats);

    if (stats.errors == 0 && !cfg.strict && stats.warnings > 0) {
        fprintf(stdout, "hl-lint: warnings=%ld (non-fatal)\n", stats.warnings);
        rc = 0;
    } else {
        fprintf(stdout, "hl-lint: warnings=%ld errors=%ld\n", stats.warnings, stats.errors);
        if (stats.errors > 0) rc = 1;
        else if (cfg.strict && stats.warnings > 0) rc = 1;
    }

    if (cfg.strict) {
        fprintf(stdout, "hl-lint: strict mode enabled\n");
    }

    list_free(&all_files);
    list_free(&cfg.source_files);
    list_free(&cfg.source_dirs);
    list_free(&cfg.include_dirs);
    list_free(&cfg.allow_getenv_files);
    list_free(&cfg.allow_stdio_files);
    list_free(&cfg.allow_shell_files);
    return rc;
}
