#!/usr/bin/env bash
# Differential AArch64 ISA fuzz driver.
#
# For every seed it generates a self-contained guest program (see isafuzz_gen.c), compiles it
# NATIVELY, and runs the SAME binary twice: once directly on this ARM64 Linux host (the reference)
# and once under build/linux-production/hl-engine-linux-aarch64 (the engine under test). Guest ISA
# == host ISA == same silicon, so any difference in the canonical state dump is unambiguously an
# engine bug -- there is no second emulator to disagree with.
#
#   run.sh [options]
#     --seeds N          number of seeds to run (default 100)
#     --start N          first seed (default 1)
#     --list "a b c"     run exactly these seeds
#     --steps N          instructions per program (default 300)
#     --jobs N           parallel workers (default: nproc)
#     --engine PATH      engine binary (default build/linux-production/hl-engine-linux-aarch64)
#     --out DIR          work/repro directory (default build/isafuzz-arm)
#     --ignore-fpsr      drop the FPSR line from the comparison
#     --ignore-mem       drop the MEM line from the comparison
#     --gen-args "..."   extra generator flags: +i8mm +bf16 +dczva +fpcr +hot
#     --pie              build the guest static-PIE instead of non-PIE ET_EXEC. The two link models
#                        take DIFFERENT translator paths: a non-PIE image maps high and every low
#                        absolute access is bias-folded (emit_fold_mem), a PIE one is copied straight.
#     --minimize SEED    shrink one failing seed to a minimal diverging sequence
#
# Exit status is 0 only when every seed matched.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"

seeds=100
start=1
list=""
steps=300
jobs="$(nproc 2>/dev/null || echo 4)"
engine="$root/build/linux-production/hl-engine-linux-aarch64"
outdir="$root/build/isafuzz-arm"
ignore_fpsr=0
ignore_mem=0
gen_args=""
minimize=""
pie=0

while [ $# -gt 0 ]; do
  case "$1" in
    --seeds) seeds="$2"; shift 2;;
    --start) start="$2"; shift 2;;
    --list) list="$2"; shift 2;;
    --steps) steps="$2"; shift 2;;
    --jobs) jobs="$2"; shift 2;;
    --engine) engine="$2"; shift 2;;
    --out) outdir="$2"; shift 2;;
    --ignore-fpsr) ignore_fpsr=1; shift;;
    --ignore-mem) ignore_mem=1; shift;;
    --gen-args) gen_args="$2"; shift 2;;
    --pie) pie=1; shift;;
    --minimize) minimize="$2"; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

case "$(uname -m)" in
  aarch64|arm64) ;;
  *) echo "the aarch64 ISA fuzzer needs an ARM64 host (the oracle IS the host CPU)" >&2; exit 2;;
esac

GUEST_CC="${GUEST_CC:-cc}"
GUEST_CFLAGS="-O2 -static -w"
[ "$pie" = 1 ] && GUEST_CFLAGS="-O2 -static-pie -w"

mkdir -p "$outdir/work" "$outdir/repro"
gen="$outdir/isafuzz_gen"

if [ ! -x "$gen" ] || [ "$here/isafuzz_gen.c" -nt "$gen" ]; then
  ${HOST_CC:-cc} -O2 -std=gnu11 -o "$gen" "$here/isafuzz_gen.c" || exit 2
fi

[ -x "$engine" ] || { echo "missing engine: $engine" >&2; exit 2; }

filter() {
  local pat='^$'
  [ "$ignore_fpsr" = 1 ] && pat='^FPSR '
  [ "$ignore_mem" = 1 ] && pat="$pat|^MEM "
  grep -Ev "$pat"
}

# --------------------------------------------------------------------- one seed
# echoes "<verdict> <seed>"; verdicts: ok | diverge | engine-error | ref-error | build-error
run_seed() {
  local seed="$1" w
  w="$outdir/work/s$seed"
  mkdir -p "$w"
  "$gen" "$seed" "$w/prog.c" "$steps" $gen_args || { echo "build-error $seed"; return 1; }
  $GUEST_CC $GUEST_CFLAGS "$w/prog.c" -o "$w/prog" 2>"$w/cc.log" || { echo "build-error $seed"; return 1; }
  "$w/prog" >"$w/ref.out" 2>"$w/ref.err"; local nrc=$?
  "$engine" "$w/prog" >"$w/hl.out" 2>"$w/hl.err"; local hrc=$?
  if [ "$nrc" != 0 ] || [ ! -s "$w/ref.out" ]; then echo "ref-error $seed"; return 1; fi
  if [ "$hrc" != 0 ] || [ ! -s "$w/hl.out" ]; then
    rm -rf "$outdir/repro/s$seed"; cp -r "$w" "$outdir/repro/s$seed" 2>/dev/null
    echo "engine-error $seed"; return 1
  fi
  if filter <"$w/ref.out" | diff -q - <(filter <"$w/hl.out") >/dev/null; then
    echo "ok $seed"; return 0
  fi
  rm -rf "$outdir/repro/s$seed"; cp -r "$w" "$outdir/repro/s$seed"
  filter <"$w/ref.out" | diff - <(filter <"$w/hl.out") >"$outdir/repro/s$seed/diff.txt"
  echo "diverge $seed"; return 1
}

# --------------------------------------------------------------------- minimizer
# Delta-debugs the BODY region of a failing program down to a minimal still-diverging sequence.
# Two properties make it sound: each generated STEP is one source line, so a guard (the index
# clamp before a scaled memory operand, the `add x24,x25,#off` that establishes a writeback base,
# a conditional branch and its target) can never be separated from the instruction it protects;
# and the reduction is TARGETED -- the shrunken program must reproduce the same failure MODE and
# still differ on one of the same state keys (X07 / NZCV / V13 / MEM ...) as the full-size one.
minimize_seed() {
  local seed="$1"
  local w="$outdir/min/s$seed"
  rm -rf "$w"; mkdir -p "$w"
  "$gen" "$seed" "$w/full.c" "$steps" $gen_args || exit 2

  local b e total
  b=$(awk '/BODY-BEGIN/{print NR; exit}' "$w/full.c")
  e=$(awk '/BODY-END/{print NR; exit}' "$w/full.c")
  head -n "$b" "$w/full.c" >"$w/head.c"
  tail -n +"$e" "$w/full.c" >"$w/tail.c"
  sed -n "$((b + 1)),$((e - 1))p" "$w/full.c" >"$w/body.txt"
  total=$(wc -l <"$w/body.txt")
  echo "minimizing seed $seed: $total body lines"

  local -a keep
  local i
  for ((i = 1; i <= total; i++)); do keep[i]=1; done

  mode=""
  keys=""

  probe() {
    probe_mode=""; probe_keys=""
    "$w/cand" >"$w/r.out" 2>/dev/null || { probe_mode="ref-error"; return; }
    [ -s "$w/r.out" ] || { probe_mode="ref-error"; return; }
    if ! "$engine" "$w/cand" >"$w/h.out" 2>/dev/null || [ ! -s "$w/h.out" ]; then
      probe_mode="engine-error"; return
    fi
    if filter <"$w/r.out" | diff -q - <(filter <"$w/h.out") >/dev/null; then probe_mode="ok"; return; fi
    probe_mode="diverge"
    probe_keys=$(filter <"$w/r.out" | diff - <(filter <"$w/h.out") |
                 grep -E '^[<>]' | awk '{print $2}' | sort -u | tr '\n' ' ')
  }

  try() {
    cat "$w/head.c" "$1" "$w/tail.c" >"$w/cand.c"
    $GUEST_CC $GUEST_CFLAGS "$w/cand.c" -o "$w/cand" 2>/dev/null || return 1
    probe
    [ "$probe_mode" = "$mode" ] || return 1
    [ "$mode" = "engine-error" ] && return 0
    local k
    for k in $keys; do
      case " $probe_keys " in *" $k "*) return 0;; esac
    done
    return 1
  }

  emit() {
    : >"$1"
    local n=0
    while IFS= read -r ln; do
      n=$((n + 1))
      [ "${keep[n]}" = 1 ] && printf '%s\n' "$ln" >>"$1"
    done <"$w/body.txt"
  }

  emit "$w/cur.txt"
  cat "$w/head.c" "$w/cur.txt" "$w/tail.c" >"$w/cand.c"
  $GUEST_CC $GUEST_CFLAGS "$w/cand.c" -o "$w/cand" 2>/dev/null || { echo "seed $seed does not build"; return 1; }
  probe
  mode="$probe_mode"; keys="$probe_keys"
  case "$mode" in
    ok|ref-error) echo "seed $seed does not fail at full size; nothing to minimize"; return 1;;
  esac
  echo "target: mode=$mode keys=${keys:-<none>}"

  local gran=$(( (total + 1) / 2 ))
  while [ "$gran" -ge 1 ]; do
    local progressed=1
    while [ "$progressed" = 1 ]; do
      progressed=0
      local s
      for ((s = 1; s <= total; s += gran)); do
        local any=0 j
        for ((j = s; j < s + gran && j <= total; j++)); do
          [ "${keep[j]}" = 1 ] && any=1
        done
        [ "$any" = 0 ] && continue
        local -a saved=()
        for ((j = s; j < s + gran && j <= total; j++)); do saved[j]=${keep[j]}; keep[j]=0; done
        emit "$w/cand.txt"
        if try "$w/cand.txt"; then
          progressed=1
        else
          for ((j = s; j < s + gran && j <= total; j++)); do keep[j]=${saved[j]}; done
        fi
      done
    done
    [ "$gran" = 1 ] && break
    gran=$(( gran / 2 ))
  done

  emit "$w/min.txt"
  cat "$w/head.c" "$w/min.txt" "$w/tail.c" >"$w/min.c"
  $GUEST_CC $GUEST_CFLAGS "$w/min.c" -o "$w/min"
  "$w/min" >"$w/min.ref.out"
  "$engine" "$w/min" >"$w/min.hl.out"
  echo "--- minimal body ($(wc -l <"$w/min.txt") lines) -> $w/min.txt"
  cat "$w/min.txt"
  echo "--- diff (native vs engine)"
  filter <"$w/min.ref.out" | diff - <(filter <"$w/min.hl.out")
  echo "--- artifacts in $w"
}

if [ -n "$minimize" ]; then
  minimize_seed "$minimize"
  exit 0
fi

# --------------------------------------------------------------------- campaign
if [ -n "$list" ]; then
  seed_list="$list"
else
  seed_list="$(seq "$start" $((start + seeds - 1)))"
fi

export -f run_seed filter
export outdir gen steps gen_args GUEST_CC GUEST_CFLAGS engine ignore_fpsr ignore_mem

results="$outdir/results.txt"
: >"$results"
printf '%s\n' $seed_list | xargs -P "$jobs" -I{} bash -c 'run_seed "$@"' _ {} >>"$results" 2>/dev/null

ok=$(grep -c '^ok ' "$results" || true)
div=$(grep -c '^diverge ' "$results" || true)
eerr=$(grep -c '^engine-error ' "$results" || true)
berr=$(grep -c '^build-error ' "$results" || true)
rerr=$(grep -c '^ref-error ' "$results" || true)
echo "seeds=$(printf '%s\n' $seed_list | wc -l) ok=$ok diverge=$div engine-error=$eerr build-error=$berr ref-error=$rerr"
if [ "$div" != 0 ] || [ "$eerr" != 0 ] || [ "$berr" != 0 ] || [ "$rerr" != 0 ]; then
  echo "failing seeds (repros under $outdir/repro):"
  grep -Ev '^ok ' "$results" | sort -k2 -n | sed 's/^/  /'
  exit 1
fi
exit 0
