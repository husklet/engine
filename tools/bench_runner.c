/*
 * bench_runner.c -- fair, pluggable multi-environment runner for the combined
 * self-timing benchmark guest (tests/perf/combined_bench.c).
 *
 * The guest self-times every phase FROM INSIDE THE GUEST (cntvct on arm64,
 * clock_gettime(CLOCK_MONOTONIC) on amd64 -- one portable source) and prints
 *
 *     PHASE <name> us=<n> ok=<checksum>
 *
 * Because the timing is inside the guest, each environment's process / startup
 * / isolation cost is excluded automatically: the per-phase us are pure
 * execution and are directly comparable across environments.
 *
 * ENVIRONMENTS are pluggable "providers". Each is a struct with function
 * pointers for setup/run/teardown; adding an environment means adding one
 * struct to the PROVIDERS registry. Providers construct argv directly and the
 * runner executes it without a host shell.
 *
 *   native     direct run on this host (host arch only; orbstack = alias)
 *   qemu       qemu-<arch> <binary>                          (qemu-user)
 *   hl-engine  hl-engine-linux-<arch> <binary>              (single hl worker = DEFAULT)
 *   docker     docker run --platform linux/<arch> ... /b     (native-in-container)
 *
 * Per-env flags are read from the CLI ("based on env we adjust the flags"):
 *   --sock S     docker daemon socket        (docker: docker -H unix://S)
 *   --image I    container image             (docker)
 *   --engine E   hl engine binary            (hl-engine)
 *   --qemu-bin Q qemu-user binary            (qemu)
 *   --binary B   guest binary to run         (all)
 * The Manager can also point docker at a host/remote daemon with
 * DOCKER="mac docker" (env), e.g. OrbStack's host docker.
 *
 * Subcommands:
 *   bench-runner list
 *   bench-runner run --env <env> --arch <arm64|amd64> [--repeats N] [--out F.csv] [env-flags]
 *   bench-runner report F.csv [F.csv ...]
 *
 * Output is CSV: env,arch,phase,us,ok,us_min,us_max,repeats
 * `report` pivots one-or-more CSVs into a phase x (env,arch) table with
 * x-<baseline> ratios, and flags any checksum divergence. Checksums are compared
 * PER ARCH (every env of one arch runs the same guest binary, so they must
 * match); the tid-based syscall phase is skipped entirely, and arch-dependent
 * phases such as 'crypto' are not compared across arches.
 *
 * ============================================================================
 * EXAMPLE -- reproduce the ARM64 + AMD64 comparison tables (run from repo root)
 * ============================================================================
 * Each cell is one CSV; you generate one per (arch,env) by running the guest in
 * that env, then `report` merges them. On an OrbStack arm64 Linux machine:
 *
 *   # build the guests (both arches), engine (both arches), and this runner
 *   nix develop -c bash -c 'ninja -C <build-dir> perf/combined-bench-aarch64 \
 *       build/perf/combined-bench-x86_64 build/tools/bench-runner \
 *       build/linux-production/hl-engine-linux-aarch64 \
 *       build/linux-production/hl-engine-linux-x86_64'
 *   R=build/tools/bench-runner; mkdir -p results
 *   # arm64 cells (native is the true baseline here):
 *   $R run --env native    --arch arm64 --repeats 4 --out results/native-arm64.csv
 *   $R run --env qemu      --arch arm64 --repeats 4 --out results/qemu-arm64.csv
 *   $R run --env hl-engine --arch arm64 --repeats 4 --out results/hl-arm64.csv
 *   DOCKER='mac docker' $R run --env docker --arch arm64 --out results/docker-arm64.csv
 *   # amd64 cells (all emulated on an arm host -- no native amd cell):
 *   $R run --env qemu      --arch amd64 --repeats 4 --out results/qemu-amd64.csv
 *   $R run --env hl-engine --arch amd64 --repeats 4 --out results/hl-amd64.csv
 *   DOCKER='mac docker' $R run --env docker --arch amd64 --out results/docker-amd64.csv
 *   # native-mac cell (the 2nd native-arm point): build+run on the mac host --
 *   # NOTE Apple clang != gcc, so treat its absolute us as compiler-comparable
 *   # only, not a drop-in for the gcc-built linux cells.
 *   # the two tables, each column as a multiple of hl-engine for its arch:
 *   $R report --baseline hl-engine results/<csv-files>
 *   # (or --baseline native to read everything relative to native arm.)
 *
 * On an actual amd64 host, run the same with --arch amd64 and --env native to
 * fill the real native-amd64 baseline. The `bench` target runs the locally-reachable
 * cells for you.
 *
 * ============================================================================
 * HOW TO READ THE TABLES
 * ============================================================================
 * Rows = benchmark phase; columns = env/arch. Each phase self-times INSIDE the
 * guest, so startup/process/isolation cost is excluded -- the us are pure
 * execution and comparable across envs. With --baseline hl-engine every cell is
 * a MULTIPLE of the hl-engine cell OF THE SAME ARCH:
 *     ratio > 1  -> that env is SLOWER than hl on that phase
 *     ratio < 1  -> that env is FASTER than hl on that phase   (hl overhead)
 *     ratio = 1.00 -> the hl-engine reference column itself
 *
 * ARM64 is the trustworthy comparison: `native` is a REAL native run, so
 * native<1 marks hl's genuine execution overhead (the attack targets), and
 * docker/arm64 (native-in-a-linux-container) should sit ~=native as a sanity
 * check. hl at ~1.0 vs native means "runs at native speed"; hl>1 vs native
 * (e.g. syscall/signal/file) means hl is FASTER than native (in-engine
 * syscall shortcuts). qemu columns show how far hl beats qemu-user.
 *
 * AMD64 on an arm host is an EMULATOR SHOOTOUT, not a native comparison: there
 * is NO native cell (needs real amd hardware). hl-engine and qemu both do
 * x86->arm translation; docker/amd64 is Apple's Rosetta 2 (Docker Desktop on
 * Apple Silicon), a heavily silicon-assisted x86 translator -- so Rosetta
 * beating hl there is expected and is NOT a native number. Read amd64 only as
 * "hl vs qemu vs Rosetta at x86 translation".
 *
 * `results/` is generated output and is gitignored; only this tool + the guest
 * source are tracked. Re-run the EXAMPLE any time to regenerate both tables.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "config.h"
#include "process.h"

#define MAX_PHASES 32
#define MAX_REPEATS 128
#define MAX_COLS 32
#define LINE 4096
#define MAX_PROCESS_ARGS 32
#define DEFAULT_PATH_SIZE (LINE + 128)

/* ------------------------------------------------------------------ options */
typedef struct {
    const char *arch;     /* public: arm64 | amd64 */
    const char *gnu;      /* aarch64 | x86_64 */
    const char *docker;   /* arm64 | amd64 */
    const char *binary;   /* guest binary */
    const char *engine;   /* hl engine binary */
    const char *qemu_bin; /* qemu-user binary */
    const char *sock;     /* docker socket */
    const char *image;    /* docker image */
    char root[LINE];      /* repo root */
} ctx_t;

typedef struct {
    char *argv[MAX_PROCESS_ARGS];
    size_t argc;
    char **owned_prefix;
    const char *stdin_path;
    char endpoint[LINE];
    char platform[64];
} invocation_t;

static int invocation_append(invocation_t *invocation, const char *argument) {
    if (invocation->argc + 1 >= MAX_PROCESS_ARGS) return -1;
    invocation->argv[invocation->argc++] = (char *)argument;
    invocation->argv[invocation->argc] = NULL;
    return 0;
}

static int invocation_prefix(invocation_t *invocation, const char *prefix) {
    size_t count = 0;
    if (hl_process_split_prefix(prefix, &invocation->owned_prefix, &count) != 0) return -1;
    for (size_t index = 0; index < count; index++)
        if (invocation_append(invocation, invocation->owned_prefix[index]) != 0) return -1;
    return 0;
}

static void invocation_clear(invocation_t *invocation) {
    hl_process_free_argv(invocation->owned_prefix);
    memset(invocation, 0, sizeof(*invocation));
}

/* Repo root from /proc/self/exe: build/tools/bench-runner -> up 3. */
static void detect_root(char *out, size_t n) {
    char exe[LINE];
    ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) {
        snprintf(out, n, ".");
        return;
    }
    exe[k] = '\0';
    /* strip three path components: /bench-runner, /tools, /build */
    for (int i = 0; i < 3; ++i) {
        char *slash = strrchr(exe, '/');
        if (!slash) break;
        *slash = '\0';
    }
    snprintf(out, n, "%s", exe[0] ? exe : ".");
}

static const char *arch_to_gnu(const char *a) {
    if (!strcmp(a, "arm64")) return "aarch64";
    if (!strcmp(a, "amd64")) return "x86_64";
    return NULL;
}

static const char *arch_to_docker(const char *a) {
    return a;
}

static const char *host_arch(void) {
    struct utsname u;
    if (uname(&u) != 0) return "unknown";
    if (!strcmp(u.machine, "aarch64") || !strcmp(u.machine, "arm64")) return "arm64";
    if (!strcmp(u.machine, "x86_64") || !strcmp(u.machine, "amd64")) return "amd64";
    return "unknown";
}

static int is_execu(const char *p) {
    return p && access(p, X_OK) == 0;
}

/* Fill defaults for binary/engine from root+arch when not overridden. */
static void ctx_defaults(ctx_t *c) {
    static char bpath[DEFAULT_PATH_SIZE], epath[DEFAULT_PATH_SIZE];
    if (!c->binary) {
        snprintf(bpath, sizeof(bpath), "%s/build/perf/combined-bench-%s", c->root, c->gnu);
        c->binary = bpath;
    }
    if (!c->engine) {
        snprintf(epath, sizeof(epath), "%s/build/linux-production/hl-engine-linux-%s", c->root, c->gnu);
        c->engine = epath;
    }
}

/* Environment override for the docker command (default "docker"). */
static const char *docker_cmd(void) {
    return hl_tool_config_docker_command();
}

static const char *docker_image(const ctx_t *c) {
    if (c->image) return c->image;
    return hl_tool_config_docker_image();
}

/* ------------------------------------------------------------ provider model */
typedef struct provider {
    const char *name;
    /* reachable: 1 if this env can run ctx on this host; writes a reason. */
    int (*reachable)(const ctx_t *, char *reason, size_t n);
    int (*build)(const ctx_t *, invocation_t *);
    int (*setup)(const ctx_t *);    /* optional, may be NULL */
    int (*teardown)(const ctx_t *); /* optional, may be NULL */
} provider_t;

/* ---- native (direct run on this host) ----
 * Runs the guest straight on whatever machine you are on -- no container, no
 * qemu, no engine. Reachable only when the requested arch equals the host arch
 * (you cannot run amd64 natively on an arm64 host: use qemu/docker-emulation
 * for the cross-arch cell, or run --env native on an amd64 host). This is the
 * "native" baseline; on an OrbStack Linux machine it IS the OrbStack cell, so
 * "orbstack" is registered as an alias of this same provider. */
static int native_reach(const ctx_t *c, char *r, size_t n) {
    if (strcmp(c->arch, host_arch()) != 0) {
        snprintf(r, n,
                 "host arch is %s; native can only run the host arch -- run "
                 "--env native on a %s host, or use qemu/docker for the %s cell",
                 host_arch(), c->arch, c->arch);
        return 0;
    }
    if (!is_execu(c->binary)) {
        snprintf(r, n, "guest binary missing: %s", c->binary);
        return 0;
    }
    snprintf(r, n, "native run on this %s host", c->arch);
    return 1;
}

static int native_build(const ctx_t *c, invocation_t *invocation) {
    return invocation_append(invocation, c->binary);
}

/* ---- qemu-user ---- */
static const char *qemu_of(const ctx_t *c) {
    static char q[LINE];
    if (c->qemu_bin) return c->qemu_bin;
    snprintf(q, sizeof(q), "qemu-%s", c->gnu);
    return q;
}

static int which_ok(const char *bin) {
    if (!bin || !*bin) return 0;
    if (strchr(bin, '/')) return access(bin, X_OK) == 0;
    const char *path = hl_tool_config_path();
    while (*path) {
        const char *end = strchr(path, ':');
        size_t length = end ? (size_t)(end - path) : strlen(path);
        char candidate[LINE];
        int written = length == 0 ? snprintf(candidate, sizeof(candidate), "./%s", bin)
                                  : snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, path, bin);
        if (written > 0 && written < (int)sizeof(candidate) && access(candidate, X_OK) == 0) return 1;
        if (!end) break;
        path = end + 1;
    }
    return 0;
}

static int qemu_reach(const ctx_t *c, char *r, size_t n) {
    if (!is_execu(c->binary)) {
        snprintf(r, n, "guest binary missing: %s", c->binary);
        return 0;
    }
    if (!which_ok(qemu_of(c))) {
        snprintf(r, n, "%s not found on PATH", qemu_of(c));
        return 0;
    }
    snprintf(r, n, "%s available", qemu_of(c));
    return 1;
}

static int qemu_build(const ctx_t *c, invocation_t *invocation) {
    if (invocation_append(invocation, qemu_of(c)) != 0) return -1;
    return invocation_append(invocation, c->binary);
}

/* ---- hl-engine (single worker) ---- */
static int hl_reach(const ctx_t *c, char *r, size_t n) {
    if (!is_execu(c->binary)) {
        snprintf(r, n, "guest binary missing: %s", c->binary);
        return 0;
    }
    if (!is_execu(c->engine)) {
        snprintf(r, n, "engine missing: %s", c->engine);
        return 0;
    }
    snprintf(r, n, "engine %s", c->engine);
    return 1;
}

static int hl_build(const ctx_t *c, invocation_t *invocation) {
    if (invocation_append(invocation, c->engine) != 0) return -1;
    return invocation_append(invocation, c->binary);
}

/* ---- docker (native-in-container) ---- */
static int docker_prefix(const ctx_t *c, invocation_t *invocation) {
    if (invocation_prefix(invocation, docker_cmd()) != 0) return -1;
    if (!c->sock) return 0;
    int written = snprintf(invocation->endpoint, sizeof(invocation->endpoint), "unix://%s", c->sock);
    if (written < 0 || written >= (int)sizeof(invocation->endpoint)) return -1;
    if (invocation_append(invocation, "-H") != 0) return -1;
    return invocation_append(invocation, invocation->endpoint);
}

static int docker_reach(const ctx_t *c, char *r, size_t n) {
    if (!is_execu(c->binary)) {
        snprintf(r, n, "guest binary missing: %s", c->binary);
        return 0;
    }
    invocation_t invocation = {0};
    hl_process_result_t result;
    int run_error = docker_prefix(c, &invocation);
    if (run_error == 0) run_error = invocation_append(&invocation, "version");
    if (run_error == 0) run_error = hl_process_run(invocation.argv, NULL, 1, NULL, NULL, &result);
    int reachable = run_error == 0 && result.exec_errno == 0 && result.exited && result.exit_code == 0;
    invocation_clear(&invocation);
    if (!reachable) {
        snprintf(r, n, "docker not reachable via '%s'%s%s", docker_cmd(), c->sock ? " sock=" : "",
                 c->sock ? c->sock : "");
        return 0;
    }
    snprintf(r, n, "docker via '%s', image %s", docker_cmd(), docker_image(c));
    return 1;
}

static int docker_build(const ctx_t *c, invocation_t *invocation) {
    int written = snprintf(invocation->platform, sizeof(invocation->platform), "linux/%s", arch_to_docker(c->arch));
    if (written < 0 || written >= (int)sizeof(invocation->platform) || docker_prefix(c, invocation) != 0) return -1;
    const char *arguments[] = {
        "run",
        "--rm",
        "-i",
        "--platform",
        invocation->platform,
        docker_image(c),
        "sh",
        "-c",
        "cat >/b && chmod +x /b && /b",
    };
    for (size_t index = 0; index < sizeof(arguments) / sizeof(arguments[0]); index++)
        if (invocation_append(invocation, arguments[index]) != 0) return -1;
    invocation->stdin_path = c->binary;
    return 0;
}

static const provider_t PROVIDERS[] = {
    {"native", native_reach, native_build, NULL, NULL}, {"orbstack", native_reach, native_build, NULL, NULL},
    {"docker", docker_reach, docker_build, NULL, NULL}, {"qemu", qemu_reach, qemu_build, NULL, NULL},
    {"hl-engine", hl_reach, hl_build, NULL, NULL},
};
static const int NPROV = (int)(sizeof(PROVIDERS) / sizeof(PROVIDERS[0]));
static const char *ARCHES[] = {"arm64", "amd64"};

static const provider_t *find_provider(const char *name) {
    for (int i = 0; i < NPROV; ++i)
        if (!strcmp(PROVIDERS[i].name, name)) return &PROVIDERS[i];
    return NULL;
}

/* ---------------------------------------------------------------- run engine */
typedef struct {
    char name[64];
    uint64_t us[MAX_REPEATS];
    uint64_t ok;
    int count;
} phase_acc_t;

static uint64_t umedian(uint64_t *v, int n) {
    /* insertion sort (n small) */
    for (int i = 1; i < n; ++i) {
        uint64_t x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = x;
    }
    return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static uint64_t umin(const uint64_t *v, int n) {
    uint64_t m = v[0];
    for (int i = 1; i < n; ++i)
        if (v[i] < m) m = v[i];
    return m;
}

static uint64_t umax(const uint64_t *v, int n) {
    uint64_t m = v[0];
    for (int i = 1; i < n; ++i)
        if (v[i] > m) m = v[i];
    return m;
}

static int run_once(const invocation_t *invocation, phase_acc_t *acc, int *nph) {
    hl_process_result_t result;
    char *output = NULL;
    if (hl_process_run((char *const *)invocation->argv, invocation->stdin_path, 0, &output, NULL, &result) != 0)
        return -1;
    if (result.exec_errno != 0 || !result.exited || result.exit_code != 0) {
        free(output);
        return -2;
    }
    int seen = 0;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[64];
        unsigned long long us = 0, ok = 0;
        if (sscanf(line, "PHASE %63s us=%llu ok=%llu", name, &us, &ok) == 3) {
            /* find or create phase slot */
            int idx = -1;
            for (int i = 0; i < *nph; ++i)
                if (!strcmp(acc[i].name, name)) {
                    idx = i;
                    break;
                }
            if (idx < 0) {
                if (*nph >= MAX_PHASES) continue;
                idx = (*nph)++;
                snprintf(acc[idx].name, sizeof(acc[idx].name), "%s", name);
                acc[idx].count = 0;
                acc[idx].ok = ok;
            }
            if (acc[idx].count < MAX_REPEATS) acc[idx].us[acc[idx].count++] = us;
            seen++;
        }
    }
    free(output);
    return seen > 0 ? 0 : -3;
}

static void print_invocation(const invocation_t *invocation) {
    fprintf(stderr, "   argv:");
    for (size_t index = 0; index < invocation->argc; index++)
        fprintf(stderr, " [%s]", invocation->argv[index]);
    if (invocation->stdin_path) fprintf(stderr, " < [%s]", invocation->stdin_path);
    fputc('\n', stderr);
}

static int ensure_parent_dir(const char *path) {
    char tmp[LINE];
    size_t length = strlen(path);
    struct stat status;
    if (length >= sizeof(tmp)) return -1;
    memcpy(tmp, path, length + 1);
    char *slash = strrchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    for (char *cursor = tmp + (tmp[0] == '/');; ++cursor) {
        if (*cursor != '/' && *cursor != '\0') continue;
        char saved = *cursor;
        *cursor = '\0';
        if (tmp[0] != '\0' && mkdir(tmp, 0777) != 0 &&
            (errno != EEXIST || stat(tmp, &status) != 0 || !S_ISDIR(status.st_mode)))
            return -1;
        *cursor = saved;
        if (saved == '\0') break;
    }
    return 0;
}

static int cmd_run(int argc, char **argv) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    detect_root(c.root, sizeof(c.root));
    const char *env = NULL, *out = NULL;
    int repeats = 5;
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : NULL)
        if (!strcmp(a, "--env"))
            env = NEXT();
        else if (!strcmp(a, "--arch"))
            c.arch = NEXT();
        else if (!strcmp(a, "--repeats")) {
            const char *v = NEXT();
            if (v) repeats = atoi(v);
        } else if (!strcmp(a, "--out"))
            out = NEXT();
        else if (!strcmp(a, "--binary"))
            c.binary = NEXT();
        else if (!strcmp(a, "--engine"))
            c.engine = NEXT();
        else if (!strcmp(a, "--qemu-bin"))
            c.qemu_bin = NEXT();
        else if (!strcmp(a, "--sock"))
            c.sock = NEXT();
        else if (!strcmp(a, "--image"))
            c.image = NEXT();
        else {
            fprintf(stderr, "run: unknown flag '%s'\n", a);
            return 2;
        }
#undef NEXT
    }
    if (!env || !c.arch) {
        fprintf(stderr, "run: --env and --arch are required\n");
        return 2;
    }
    c.gnu = arch_to_gnu(c.arch);
    if (!c.gnu) {
        fprintf(stderr, "run: bad --arch '%s'\n", c.arch);
        return 2;
    }
    const provider_t *p = find_provider(env);
    if (!p) {
        fprintf(stderr, "run: unknown --env '%s'\n", env);
        return 2;
    }
    if (repeats < 1 || repeats > MAX_REPEATS) repeats = 5;
    ctx_defaults(&c);

    char reason[LINE];
    if (!p->reachable(&c, reason, sizeof(reason))) {
        fprintf(stderr, "error: env=%s arch=%s not reachable: %s\n", env, c.arch, reason);
        return 1;
    }
    fprintf(stderr, "== run env=%s arch=%s repeats=%d ==\n   %s\n", env, c.arch, repeats, reason);
    if (p->setup && p->setup(&c) != 0) {
        fprintf(stderr, "error: setup failed\n");
        return 1;
    }
    invocation_t invocation = {0};
    if (p->build(&c, &invocation) != 0) {
        invocation_clear(&invocation);
        fprintf(stderr, "error: too many or invalid process arguments\n");
        return 1;
    }
    print_invocation(&invocation);

    phase_acc_t acc[MAX_PHASES];
    int nph = 0;
    for (int r = 0; r < repeats; ++r) {
        int rc = run_once(&invocation, acc, &nph);
        if (rc != 0) {
            fprintf(stderr, "error: repeat %d failed (rc=%d)\n", r, rc);
            if (p->teardown) p->teardown(&c);
            invocation_clear(&invocation);
            return 1;
        }
    }
    invocation_clear(&invocation);
    if (p->teardown) p->teardown(&c);
    if (nph == 0) {
        fprintf(stderr, "error: no PHASE output\n");
        return 1;
    }

    /* emit CSV */
    FILE *o = stdout;
    if (out) {
        if (ensure_parent_dir(out) != 0) {
            fprintf(stderr, "cannot create output directory for %s: %s\n", out, strerror(errno));
            return 1;
        }
        o = fopen(out, "w");
        if (!o) {
            perror("fopen");
            return 1;
        }
    }
    fprintf(o, "env,arch,phase,us,ok,us_min,us_max,repeats\n");
    for (int i = 0; i < nph; ++i) {
        uint64_t md = umedian(acc[i].us, acc[i].count);
        fprintf(o, "%s,%s,%s,%llu,%llu,%llu,%llu,%d\n", env, c.arch, acc[i].name, (unsigned long long)md,
                (unsigned long long)acc[i].ok, (unsigned long long)umin(acc[i].us, acc[i].count),
                (unsigned long long)umax(acc[i].us, acc[i].count), acc[i].count);
    }
    if (out) {
        fclose(o);
        fprintf(stderr, "wrote %s\n", out);
    }
    return 0;
}

/* ---------------------------------------------------------------- list */
static int cmd_list(void) {
    ctx_t c;
    memset(&c, 0, sizeof(c));
    detect_root(c.root, sizeof(c.root));
    struct utsname u;
    uname(&u);
    printf("host: %s %s (arch=%s)  root=%s\n", u.sysname, u.machine, host_arch(), c.root);
    printf("%-10s %-6s %-10s %s\n", "env", "arch", "reachable", "detail");
    printf("--------------------------------------------------------------------\n");
    for (int i = 0; i < NPROV; ++i) {
        for (size_t a = 0; a < sizeof(ARCHES) / sizeof(ARCHES[0]); ++a) {
            ctx_t cc = c;
            cc.arch = ARCHES[a];
            cc.gnu = arch_to_gnu(cc.arch);
            ctx_defaults(&cc);
            char reason[LINE];
            int ok = PROVIDERS[i].reachable(&cc, reason, sizeof(reason));
            printf("%-10s %-6s %-10s %s\n", PROVIDERS[i].name, cc.arch, ok ? "yes" : "no", reason);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- report */
typedef struct {
    char env[32], arch[16];
    char phase[MAX_PHASES][64];
    uint64_t us[MAX_PHASES];
    uint64_t ok[MAX_PHASES];
    int nph;
} col_t;

static int read_csv(const char *path, col_t *col) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "report: cannot open %s\n", path);
        return -1;
    }
    char line[LINE];
    col->nph = 0;
    col->env[0] = col->arch[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "env,arch,phase", 14)) continue; /* header */
        char env[32], arch[16], ph[64];
        unsigned long long us, ok, mn, mx;
        int rep;
        if (sscanf(line, "%31[^,],%15[^,],%63[^,],%llu,%llu,%llu,%llu,%d", env, arch, ph, &us, &ok, &mn, &mx, &rep) >=
            5) {
            snprintf(col->env, sizeof(col->env), "%s", env);
            snprintf(col->arch, sizeof(col->arch), "%s", arch);
            if (col->nph < MAX_PHASES) {
                snprintf(col->phase[col->nph], 64, "%s", ph);
                col->us[col->nph] = us;
                col->ok[col->nph] = ok;
                col->nph++;
            }
        }
    }
    fclose(f);
    return col->nph > 0 ? 0 : -1;
}

static int col_us(const col_t *c, const char *ph, uint64_t *out) {
    for (int i = 0; i < c->nph; ++i)
        if (!strcmp(c->phase[i], ph)) {
            *out = c->us[i];
            return 1;
        }
    return 0;
}

static int col_ok(const col_t *c, const char *ph, uint64_t *out) {
    for (int i = 0; i < c->nph; ++i)
        if (!strcmp(c->phase[i], ph)) {
            *out = c->ok[i];
            return 1;
        }
    return 0;
}

static int cmd_report(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "report: need CSV file(s)\n");
        return 2;
    }
    /* --baseline <env>: which env's column is the 1.00x reference per arch
     * (default native). e.g. --baseline hl-engine shows every column as a
     * multiple of hl-engine, so you read "how does X compare to hl". */
    const char *baseline_env = "native";
    static col_t cols[MAX_COLS];
    int ncol = 0;
    for (int i = 0; i < argc && ncol < MAX_COLS; ++i) {
        if (!strcmp(argv[i], "--baseline") && i + 1 < argc) {
            baseline_env = argv[++i];
            continue;
        }
        if (read_csv(argv[i], &cols[ncol]) == 0) ncol++;
    }
    if (ncol == 0) {
        fprintf(stderr, "report: no valid CSV\n");
        return 1;
    }

    /* union of phase names, first-seen order */
    char order[MAX_PHASES][64];
    int norder = 0;
    for (int c = 0; c < ncol; ++c)
        for (int i = 0; i < cols[c].nph; ++i) {
            int found = 0;
            for (int j = 0; j < norder; ++j)
                if (!strcmp(order[j], cols[c].phase[i])) {
                    found = 1;
                    break;
                }
            if (!found && norder < MAX_PHASES) snprintf(order[norder++], 64, "%s", cols[c].phase[i]);
        }

    /* baseline column per arch: the --baseline env if present for that arch,
     * else native (or its orbstack alias), else the first column of that arch. */
    int base_of[MAX_COLS];
    for (int c = 0; c < ncol; ++c) {
        int b = -1, want = -1, nat = -1;
        for (int k = 0; k < ncol; ++k)
            if (!strcmp(cols[k].arch, cols[c].arch)) {
                if (b < 0) b = k;
                if (!strcmp(cols[k].env, baseline_env) && want < 0) want = k;
                if ((!strcmp(cols[k].env, "native") || !strcmp(cols[k].env, "orbstack")) && nat < 0) nat = k;
            }
        base_of[c] = want >= 0 ? want : (nat >= 0 ? nat : b);
    }

    /* header */
    printf("%-14s", "phase");
    for (int c = 0; c < ncol; ++c) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%.*s/%.*s", 31, cols[c].env, 15, cols[c].arch);
        printf(" %14s", lbl);
    }
    for (int c = 0; c < ncol; ++c) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "x:%.*s", 31, cols[c].env);
        printf(" %9s", lbl);
    }
    printf("\n");

    int diverged = 0;
    char divlist[LINE] = "";
    for (int r = 0; r < norder; ++r) {
        const char *ph = order[r];
        printf("%-14s", ph);
        for (int c = 0; c < ncol; ++c) {
            uint64_t v;
            if (col_us(&cols[c], ph, &v))
                printf(" %14llu", (unsigned long long)v);
            else
                printf(" %14s", "-");
        }
        for (int c = 0; c < ncol; ++c) {
            uint64_t v, bv;
            int b = base_of[c];
            if (b >= 0 && col_us(&cols[c], ph, &v) && col_us(&cols[b], ph, &bv) && bv > 0)
                printf(" %9.2f", (double)v / (double)bv);
            else
                printf(" %9s", "-");
        }
        printf("\n");
        /* Checksum divergence, compared PER ARCH (skip tid-based syscall).
         * Every env running the SAME arch runs the SAME guest binary, so their
         * ok= must match exactly -- that is the check that actually detects a
         * translation bug (e.g. hl-engine disagreeing with qemu on amd64).
         * Comparing ACROSS arches would false-positive on phases whose result
         * is legitimately arch-dependent: `crypto` is the live example, where
         * x86 AES-NI (aesenc) and ARM AESE apply the round transform in a
         * different order, so the two arches produce different (individually
         * correct) digests. */
        if (strcmp(ph, "syscall") != 0) {
            int bad = 0;
            for (int c = 0; c < ncol && !bad; ++c) {
                uint64_t o;
                if (!col_ok(&cols[c], ph, &o)) continue;
                for (int d = 0; d < c; ++d) {
                    uint64_t p;
                    if (strcmp(cols[d].arch, cols[c].arch) != 0) continue; /* different arch: not comparable */
                    if (col_ok(&cols[d], ph, &p) && p != o) {
                        bad = 1;
                        break;
                    }
                }
            }
            if (bad) {
                diverged = 1;
                strncat(divlist, " ", sizeof(divlist) - strlen(divlist) - 1);
                strncat(divlist, ph, sizeof(divlist) - strlen(divlist) - 1);
            }
        }
    }
    printf("ratios are x-%s per arch (each column vs the %s cell of the same "
           "arch; missing -> first column).\n",
           baseline_env, baseline_env);
    printf("note: 'syscall' ok= is tid-dependent; divergence there is expected.\n");
    printf("note: checksums are compared per-arch; arch-dependent phases (e.g. "
           "'crypto') legitimately differ between arm64 and amd64.\n");
    if (diverged) {
        fprintf(stderr, "WARNING: checksum divergence in phases:%s\n", divlist);
        return 3;
    }
    printf("checksums consistent across envs of each arch (except tid-based "
           "syscall). OK.\n");
    return 0;
}

/* ---------------------------------------------------------------- main */
static int usage(void) {
    fprintf(stderr, "usage:\n"
                    "  bench-runner list\n"
                    "  bench-runner run --env <native|orbstack|docker|qemu|hl-engine> "
                    "--arch <arm64|amd64>\n"
                    "                   [--repeats N] [--out F.csv] [--binary B] "
                    "[--engine E]\n"
                    "                   [--qemu-bin Q] [--sock S] [--image I]\n"
                    "  bench-runner report F.csv [F.csv ...]\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "list")) return cmd_list();
    if (!strcmp(argv[1], "run")) return cmd_run(argc - 2, argv + 2);
    if (!strcmp(argv[1], "report")) return cmd_report(argc - 2, argv + 2);
    return usage();
}
