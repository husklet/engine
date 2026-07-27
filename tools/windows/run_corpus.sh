#!/usr/bin/env bash
# Drive the x86_64 guest corpus suite-by-suite, one log per suite.
#
# Why not `ctest -R production.full-x86_64`: ctest pins
# HL_MATRIX_TIMEOUT_SCALE=30 per test, so a hanging case costs 3600s. Measured
# on this host the whole 91-case libc suite runs in 3.0s (~33ms/case), so the
# UNSCALED 120s per-case budget is already ~2600x the slowest observed case;
# scale 1 turns a hang into a 2-minute cost instead of an hour. Override with
# HL_MATRIX_TIMEOUT_SCALE if a case is genuinely slow-but-correct.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-corpus}"
# Overridable so a second engine can be measured against the SAME staged corpus:
#   ENGINE=.../build-fix/... OUT=corpus-logs-fixed tools/windows/run_corpus.sh
ENGINE="${ENGINE:-$BUILD/windows-production/hl-engine-windows-x86_64.exe}"
RUNNER="$BUILD/tools/linux-matrix.exe"
OUT="${OUT:-$ROOT/corpus-logs}"
export HL_MATRIX_TIMEOUT_SCALE="${HL_MATRIX_TIMEOUT_SCALE:-1}"

mkdir -p "$OUT"

suites="
abi:tests/compat/abi
abi-corpus:tests/compat/abi/corpus
libc:tests/compat/libc
completeness:tests/compat/completeness
posix:tests/compat/posix
syscall:tests/compat/syscall
network:tests/compat/network
procfs:tests/compat/procfs
memory:tests/compat/memory
filesystem:tests/compat/filesystem
signals:tests/compat/signals
process:tests/compat/process
time:tests/compat/time
core/abi:tests/compat/core/abi
core/workload:tests/compat/core/workload
core/syscall:tests/compat/core/syscall
core/regress:tests/compat/core/regress
ipc:tests/compat/ipc
threads:tests/compat/threads
isolation:tests/compat/isolation
syscall_edges:tests/compat/syscall_edges
isa/x86_64:tests/compat/isa/x86_64
"

only="${1:-}"
for pair in $suites; do
    dir="${pair%%:*}"
    src="${pair#*:}"
    label="$(echo "$dir" | tr '/' '-')"
    [ -n "$only" ] && [ "$only" != "$label" ] && continue
    bin="$BUILD/compat/$dir/x86_64"
    case "$dir" in isa/x86_64) bin="$BUILD/compat/isa/x86_64" ;; esac
    [ -d "$bin" ] || { echo "SKIP $label (no $bin)"; continue; }
    start=$(date +%s)
    "$RUNNER" --suite "$ENGINE" "$bin" "$ROOT/$src" >"$OUT/$label.log" 2>&1
    rc=$?
    end=$(date +%s)
    summary=$(grep -h "active x86_64 cases passed" "$OUT/$label.log" | tail -1)
    echo "[$((end-start))s rc=$rc] $label :: ${summary:-NO SUMMARY}"
done
