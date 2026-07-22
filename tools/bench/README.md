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
| `orbstack`  | native (host arch only)     | `<binary>`                                       |
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
make bench                                   # orbstack + qemu + hl-engine (local arch)
make bench BENCH_ARCH=amd64                  # x86_64 guests
make bench BENCH_REPEATS=9
```

### Per-env flags

`--binary B` (guest), `--engine E` (hl-engine), `--qemu-bin Q` (qemu),
`--sock S` + `--image I` (docker). The `DOCKER` env var overrides the docker
command itself (multi-word ok), and `DOCKER_IMAGE` the default image.

### Docker / remote daemon (Manager)

Docker is left to the Manager because it needs a reachable daemon. On OrbStack
the host docker is reached with `DOCKER="mac docker"`:

```sh
make bench BENCH_ENVS='orbstack qemu hl-engine docker' DOCKER='mac docker'
# or directly:
DOCKER='mac docker' build/tools/bench-runner run --env docker --arch arm64 \
    --image debian:stable-slim --out build/bench/docker-arm64.csv
```

To run a specific arch container use `--arch amd64` (sets `--platform
linux/amd64`); a `--sock /path/docker.sock` routes to a specific daemon socket.

## What is parameterized for the remaining cells

- **docker** cell: `DOCKER="mac docker"` (host daemon) + `--arch {arm64,amd64}`
  + `--image`. Runs the SAME static-PIE guest binary inside the container.
- **mac / host** commands: prefix with `mac` (OrbStack) as needed.
- **x86_64 build**: `make build/perf/combined-bench-x86_64` (sqlite off by
  default on the aarch64 cross host; a native amd64 cell can enable it with
  `COMBINED_BENCH_SQLITE_x86_64=1`). Engine:
  `build/linux-production/hl-engine-linux-x86_64`.
- Merge results from every cell with
  `build/tools/bench-runner report build/bench/*.csv`.
