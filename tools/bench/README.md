# Fair combined benchmark

A single self-timing guest, driven identically across backends, for an
apples-to-apples per-phase execution comparison.

## Why it is fair

`tests/perf/combined_bench.c` runs many workload phases back-to-back in **one
process**. Each phase is timed **from inside the guest** (the ARM generic timer
`cntvct_el0` on arm64, `clock_gettime(CLOCK_MONOTONIC)` on amd64 — one portable
source builds for both). Engine startup and worker fork are paid **once**,
before any phase runs, so they are excluded from every phase measurement.
Because the clock is inside the guest, each backend's process / startup /
isolation cost is excluded automatically — the reported per-phase microseconds
are **pure execution** and directly comparable across backends.

## Phases

CPU/ALU: `compute_cold`, `compute` (int+float busyloop, cold vs warm),
`intdiv` (64-bit div/mul), `float_simd` (auto-vectorized NEON/SSE), `atomics`
(CAS + fetch-add), `branch` (unpredictable data-dependent branches), `calls`
(deep recursion + rotating indirect calls — stresses IBTC / stolen-x30).
Memory/allocator: `malloc` (malloc/free churn), `string` (strlen/strcmp/
memmove), `memory` (memcpy/memcmp bandwidth), `tlb` (large-stride page walk).
OS/kernel: `syscall` (gettid loop), `signal` (SIGALRM handler delivery), `mmap`,
`file` (pwrite/pread), `pipe` (round-trip). Optional: `sqlite` (in-memory
insert/select; compiled only when `HL_BENCH_SQLITE` is set — needs a static
libsqlite3 for the target arch).

Every phase prints `PHASE <name> us=<n> ok=<checksum>` and is deterministic:
the `ok` checksum must be identical across backends of the same arch. The one
exception is `syscall`, whose checksum is `SYS_gettid`-based and so legitimately
differs by thread id.

## Backends (pluggable providers)

`tools/bench_runner.c` is a pure-C tool. Each backend is a `provider_t` struct
with `reachable` / `build_cmd` (+ optional `setup`/`teardown`) function
pointers; adding a backend = add one struct to `PROVIDERS[]`.

| env         | what it measures            | command shape                                   |
|-------------|-----------------------------|-------------------------------------------------|
| `native`  | native (host arch only)     | `<binary>`                                       |
| `qemu`      | qemu-user                   | `qemu-<arch> <binary>`                           |
| `hl-engine` | single hl worker (DEFAULT)  | `hl-engine-linux-<arch> <binary>`               |
| `docker`    | native-in-container         | `docker run --platform linux/<arch> -v …:/b …`  |

## Usage

```sh
# what can run here?
build/tools/bench-runner list

# run one env x arch, N repeats, write CSV (median us per phase)
build/tools/bench-runner run --env hl-engine --arch arm64 --repeats 5 \
    --out build/bench/hl-engine-arm64.csv

# pivot one-or-more CSVs into a phase x (env,arch) table with x-native ratios
build/tools/bench-runner report build/bench/*.csv
```

CSV columns: `env,arch,phase,us,ok,us_min,us_max,repeats`.

### The default way: `make bench`

Builds both-arch guests + the production engine + the runner, runs the local
backends, and prints the table:

```sh
make bench                                   # native + qemu + hl-engine (local arch)
make bench BENCH_ARCH=amd64                  # x86_64 guests
make bench BENCH_REPEATS=9
```

### Per-env flags

`--binary B` (guest), `--engine E` (hl-engine), `--qemu-bin Q` (qemu),
`--sock S` + `--image I` (docker). The `DOCKER` env var overrides the docker
command itself (multi-word ok), and `DOCKER_IMAGE` the default image.

### Docker / remote daemon (Manager)

Docker is left to the Manager because it needs a reachable daemon. On host (native)
the host docker is reached with `DOCKER="mac docker"`:

```sh
make bench BENCH_ENVS='native qemu hl-engine docker' DOCKER='mac docker'
# or directly:
DOCKER='mac docker' build/tools/bench-runner run --env docker --arch arm64 \
    --image debian:stable-slim --out build/bench/docker-arm64.csv
```

To run a specific arch container use `--arch amd64` (sets `--platform
linux/amd64`); a `--sock /path/docker.sock` routes to a specific daemon socket.

## What is parameterized for the remaining cells

- **docker** cell: `DOCKER="mac docker"` (host daemon) + `--arch {arm64,amd64}`
  + `--image`. Runs the SAME static-PIE guest binary inside the container.
- **mac / host** commands: prefix with `mac` (host (native)) as needed.
- **x86_64 build**: `make build/perf/combined-bench-x86_64` (sqlite off by
  default on the aarch64 cross host; a native amd64 cell can enable it with
  `COMBINED_BENCH_SQLITE_x86_64=1`). Engine:
  `build/linux-production/hl-engine-linux-x86_64`.
- Merge results from every cell with
  `build/tools/bench-runner report build/bench/*.csv`.

## Reproduce the two comparison tables (example)

Each cell is one CSV (rows = benchmark phase, columns = result); you generate
one per (arch, env), then `report` merges them into the phase × (env, arch)
table. From the repo root on an OrbStack arm64 Linux machine:

```sh
nix develop -c bash -c 'CC=cc make build/perf/combined-bench-aarch64 \
    build/perf/combined-bench-x86_64 build/tools/bench-runner \
    build/linux-production/hl-engine-linux-aarch64 \
    build/linux-production/hl-engine-linux-x86_64'
R=build/tools/bench-runner; mkdir -p results
# arm64 (native = the real baseline)
$R run --env native    --arch arm64 --repeats 4 --out results/native-arm64.csv
$R run --env qemu      --arch arm64 --repeats 4 --out results/qemu-arm64.csv
$R run --env hl-engine --arch arm64 --repeats 4 --out results/hl-arm64.csv
DOCKER='mac docker' $R run --env docker --arch arm64 --out results/docker-arm64.csv
# amd64 (all emulated on an arm host — no native amd cell)
$R run --env qemu      --arch amd64 --repeats 4 --out results/qemu-amd64.csv
$R run --env hl-engine --arch amd64 --repeats 4 --out results/hl-amd64.csv
DOCKER='mac docker' $R run --env docker --arch amd64 --out results/docker-amd64.csv
# both tables, every column as a multiple of hl-engine for its arch:
$R report --baseline hl-engine results/*.csv
```

`make bench` runs the locally-reachable cells automatically. On a real amd64
host add `--env native --arch amd64` for the true native-amd baseline.

## How to read

Rows = phase, columns = env/arch. Timing is **inside the guest**, so startup /
process / isolation cost is excluded — the µs are pure execution. With
`--baseline hl-engine` each cell is a multiple of the hl-engine cell **of the
same arch**:

- `> 1` — that env is **slower** than hl on that phase
- `< 1` — that env is **faster** than hl on that phase (i.e. hl's overhead)
- `1.00` — the hl-engine reference column

**ARM64 is the trustworthy comparison** — `native` is a real native run, so
`native < 1` marks hl's genuine execution overhead (the attack targets), and
`docker/arm64` (native-in-container) should sit ≈ native as a sanity check.
`hl ≈ native` = runs at native speed; `hl > native` on syscall/signal/file =
hl is *faster* than native (in-engine syscall shortcuts). `qemu` shows how far
hl beats qemu-user.

**AMD64 on an arm host is an emulator shootout, not native** — there is no
native cell (needs real amd hardware). hl-engine and qemu both do x86→arm
translation; `docker/amd64` is Apple's **Rosetta 2**, a silicon-assisted x86
translator, so Rosetta beating hl there is expected. Read amd64 only as
"hl vs qemu vs Rosetta at x86 translation".

`results/` is generated output and is gitignored.

## Caveat: "beats native" is a kernel-shortcut artifact, not faster code

Some phases show hl **faster than native** (ratio < 1 vs native). This is NOT
"translated code runs faster than native code" — it is hl doing *less work* by
servicing the operation in userspace and avoiding the kernel round-trip native
pays. These are **not representative** of real workloads:

- **syscall** (gettid loop): hl answers from cached thread identity in-engine —
  no kernel trap. Only trivially-cacheable syscalls (gettid/getpid) benefit.
- **file** (pwrite/pread on an unlinked temp): the unlink-while-open makes it an
  anonymous RAM-backed memf, so read/write become memcpy — the real VFS/
  page-cache/kernel path is skipped. A real on-disk file would not beat native.
- **signal** (self-raised SIGALRM): delivered via hl's in-engine machinery,
  lighter than the full kernel signal path.

Real work — an actual disk write, real cross-process IPC, a syscall hl must
forward to the host kernel — cannot be short-circuited and will NOT beat native.

**The honest "runs at native speed" result is the CPU-bound rows**
(compute/intdiv/atomics/memory/tlb ≈ 1.0x native): once translated, the emitted
code executes at native speed. Read the syscall/signal/file sub-1.0 numbers as
"in-process emulation shortcut," not as a translation-speed win.
