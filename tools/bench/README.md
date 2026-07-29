# Combined benchmark

`tests/perf/combined_bench.c` times deterministic phases inside one guest
process, excluding engine startup. `tools/bench_runner.c` runs the same binary
through native, QEMU, hl-engine, or Docker and writes CSV results.

```sh
build/tools/bench-runner list
build/tools/bench-runner run --env hl-engine --arch arm64 --repeats 5 \
  --out build/bench/hl-engine-arm64.csv
build/tools/bench-runner report --baseline hl-engine build/bench/*.csv
```

The default target builds both guest architectures, runs reachable backends,
and prints the comparison:

```sh
ninja -C build bench
BENCH_ARCH=amd64 BENCH_REPEATS=9 ninja -C build bench
```

Each result row is `env,arch,phase,us,ok,us_min,us_max,repeats`. Checksums must
match within an architecture except for the thread-ID-based syscall phase.

Useful runner overrides are `--binary`, `--engine`, `--qemu-bin`, `--image`,
and `--sock`. `DOCKER` may name a remote Docker command:

```sh
DOCKER='mac docker' build/tools/bench-runner run \
  --env docker --arch amd64 --out build/bench/docker-amd64.csv
```

For engine-in-engine profiling, restrict the guest workload:

```sh
outer-engine inner-engine inner-engine perf/combined-bench-aarch64 \
  --divisor 100000 --phase syscall
```

Ratios compare each cell with the selected baseline of the same guest
architecture. Native ARM is the direct performance baseline. AMD64 Docker on
an ARM Mac measures Rosetta, so it is an emulator comparison rather than a
native AMD64 baseline.

Sub-native syscall, signal, or temporary-file timings reflect in-engine
shortcuts, not faster translated instructions. CPU and memory phases are the
meaningful translation-overhead measurements.
