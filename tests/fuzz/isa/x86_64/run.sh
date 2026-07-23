#!/usr/bin/env bash
# Differential x86-64 ISA fuzz driver.
#
# For every seed it generates a self-contained guest program (see isafuzz_gen.c), cross-compiles
# it, runs the SAME binary under qemu-x86_64 (reference) and under the engine (under test), and
# compares the canonical state dumps. Any difference is a translator divergence.
#
#   run.sh [options]
#     --seeds N          number of seeds to run (default 100)
#     --start N          first seed (default 1)
#     --list "a b c"     run exactly these seeds
#     --steps N          instructions per program (default 200)
#     --jobs N           parallel workers (default: nproc)
#     --engine PATH      engine binary (default build/linux-production/hl-engine-linux-x86_64)
#     --qemu PATH        reference (default qemu-x86_64)
#     --out DIR          work/repro directory (default build/isafuzz)
#     --ignore-mxcsr     drop the MXCSR line from the comparison
#     --ignore-flags     drop the FLG line from the comparison (useful when minimizing: the
#                        undefined-flag mask is a constant baked from the FULL sequence)
#     --gen-args "..."   extra generator flags, e.g. "+x87"
#     --minimize SEED    shrink one failing seed to a minimal diverging sequence
#
# Exit status is 0 only when every seed matched.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"

seeds=100
start=1
list=""
steps=200
jobs="$(nproc 2>/dev/null || echo 4)"
engine="$root/build/linux-production/hl-engine-linux-x86_64"
qemu="qemu-x86_64"
outdir="$root/build/isafuzz"
ignore_mxcsr=0
ignore_flags=0
gen_args=""
minimize=""

while [ $# -gt 0 ]; do
  case "$1" in
    --seeds) seeds="$2"; shift 2;;
    --start) start="$2"; shift 2;;
    --list) list="$2"; shift 2;;
    --steps) steps="$2"; shift 2;;
    --jobs) jobs="$2"; shift 2;;
    --engine) engine="$2"; shift 2;;
    --qemu) qemu="$2"; shift 2;;
    --out) outdir="$2"; shift 2;;
    --ignore-mxcsr) ignore_mxcsr=1; shift;;
    --ignore-flags) ignore_flags=1; shift;;
    --gen-args) gen_args="$2"; shift 2;;
    --minimize) minimize="$2"; shift 2;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

CROSS_CC="${CROSS_CC:-x86_64-linux-gnu-gcc}"
GUEST_CFLAGS="-O2 -static -no-pie -fno-pie -w"

mkdir -p "$outdir/work" "$outdir/repro"
gen="$outdir/isafuzz_gen"

if [ ! -x "$gen" ] || [ "$here/isafuzz_gen.c" -nt "$gen" ]; then
  ${HOST_CC:-cc} -O2 -std=gnu11 -o "$gen" "$here/isafuzz_gen.c" || exit 2
fi

command -v "$qemu" >/dev/null || { echo "missing reference oracle: $qemu" >&2; exit 2; }
command -v "$CROSS_CC" >/dev/null || { echo "missing cross compiler: $CROSS_CC" >&2; exit 2; }
[ -x "$engine" ] || { echo "missing engine: $engine" >&2; exit 2; }

filter() {
  local pat='^$'
  [ "$ignore_mxcsr" = 1 ] && pat='^MXC '
  [ "$ignore_flags" = 1 ] && pat="$pat|^FLG "
  grep -Ev "$pat"
}

# --------------------------------------------------------------------- one seed
# echoes "<verdict> <seed>"; verdicts: ok | diverge | engine-error | build-error
run_seed() {
  local seed="$1" w
  w="$outdir/work/s$seed"
  mkdir -p "$w"
  "$gen" "$seed" "$w/prog.c" "$steps" $gen_args || { echo "build-error $seed"; return 1; }
  $CROSS_CC $GUEST_CFLAGS "$w/prog.c" -o "$w/prog" 2>"$w/cc.log" || { echo "build-error $seed"; return 1; }
  "$qemu" "$w/prog" >"$w/ref.out" 2>"$w/ref.err"; local qrc=$?
  "$engine" "$w/prog" >"$w/hl.out" 2>"$w/hl.err"; local hrc=$?
  if [ "$qrc" != 0 ]; then echo "ref-error $seed"; return 1; fi
  if [ "$hrc" != 0 ] || [ ! -s "$w/hl.out" ]; then
    cp -r "$w" "$outdir/repro/s$seed" 2>/dev/null
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
minimize_seed() {
  local seed="$1"
  local w="$outdir/min/s$seed"
  rm -rf "$w"; mkdir -p "$w"
  "$gen" "$seed" "$w/full.c" "$steps" $gen_args || exit 2

  awk '/BODY-BEGIN/{print NR; exit}' "$w/full.c" >"$w/b"
  local b e total
  b=$(awk '/BODY-BEGIN/{print NR; exit}' "$w/full.c")
  e=$(awk '/BODY-END/{print NR; exit}' "$w/full.c")
  head -n "$b" "$w/full.c" >"$w/head.c"
  tail -n +"$e" "$w/full.c" >"$w/tail.c"
  sed -n "$((b + 1)),$((e - 1))p" "$w/full.c" >"$w/body.txt"
  total=$(wc -l <"$w/body.txt")
  echo "minimizing seed $seed: $total body lines"

  # keep[i]=1 means line i is retained
  local -a keep
  local i
  for ((i = 1; i <= total; i++)); do keep[i]=1; done

  # Minimization is TARGETED: the reduced program must reproduce the SAME failure mode as the
  # full-size one -- an engine failure stays an engine failure, and a dump divergence must still
  # differ on one of the state keys (R11 / FLG / XMM4 / MEM ...) that differed originally. Without
  # this the delta debugger happily drifts onto an unrelated shrunken-program artifact (notably the
  # FLG line, whose architecturally-undefined-bit mask is a constant baked from the FULL sequence
  # and is no longer the right mask once instructions are deleted).
  mode=""
  keys=""

  probe() { # runs $w/cand -> sets probe_mode / probe_keys
    probe_mode=""; probe_keys=""
    "$qemu" "$w/cand" >"$w/r.out" 2>/dev/null || { probe_mode="ref-error"; return; }
    if ! "$engine" "$w/cand" >"$w/h.out" 2>/dev/null || [ ! -s "$w/h.out" ]; then
      probe_mode="engine-error"; return
    fi
    if filter <"$w/r.out" | diff -q - <(filter <"$w/h.out") >/dev/null; then probe_mode="ok"; return; fi
    probe_mode="diverge"
    probe_keys=$(filter <"$w/r.out" | diff - <(filter <"$w/h.out") |
                 grep -E '^[<>]' | awk '{print $2}' | sort -u | tr '\n' ' ')
  }

  try() { # $1 = file of body lines -> 0 if the ORIGINAL failure still reproduces
    cat "$w/head.c" "$1" "$w/tail.c" >"$w/cand.c"
    $CROSS_CC $GUEST_CFLAGS "$w/cand.c" -o "$w/cand" 2>/dev/null || return 1
    probe
    [ "$probe_mode" = "$mode" ] || return 1
    [ "$mode" = "engine-error" ] && return 0
    local k
    for k in $keys; do
      case " $probe_keys " in *" $k "*) return 0;; esac
    done
    return 1
  }

  emit() { # write current keep-set to $1
    : >"$1"
    local n=0
    while IFS= read -r ln; do
      n=$((n + 1))
      [ "${keep[n]}" = 1 ] && printf '%s\n' "$ln" >>"$1"
    done <"$w/body.txt"
  }

  emit "$w/cur.txt"
  cat "$w/head.c" "$w/cur.txt" "$w/tail.c" >"$w/cand.c"
  $CROSS_CC $GUEST_CFLAGS "$w/cand.c" -o "$w/cand" 2>/dev/null || { echo "seed $seed does not build"; return 1; }
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
  $CROSS_CC $GUEST_CFLAGS "$w/min.c" -o "$w/min"
  "$qemu" "$w/min" >"$w/min.ref.out"
  "$engine" "$w/min" >"$w/min.hl.out"
  echo "--- minimal body ($(wc -l <"$w/min.txt") lines) -> $w/min.txt"
  cat "$w/min.txt"
  echo "--- diff (qemu vs engine)"
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
export outdir gen steps gen_args CROSS_CC GUEST_CFLAGS qemu engine ignore_mxcsr ignore_flags

results="$outdir/results.txt"
: >"$results"
printf '%s\n' $seed_list | xargs -P "$jobs" -I{} bash -c 'run_seed "$@"' _ {} >>"$results" 2>/dev/null

ok=$(grep -c '^ok ' "$results" || true)
div=$(grep -c '^diverge ' "$results" || true)
eerr=$(grep -c '^engine-error ' "$results" || true)
berr=$(grep -c '^build-error ' "$results" || true)
rerr=$(grep -c '^ref-error ' "$results" || true)
echo "seeds=$(printf '%s\n' $seed_list | wc -l) ok=$ok diverge=$div engine-error=$eerr build-error=$berr ref-error=$rerr"
if [ "$div" != 0 ] || [ "$eerr" != 0 ]; then
  echo "failing seeds (repros under $outdir/repro):"
  grep -E '^(diverge|engine-error) ' "$results" | sort -k2 -n | sed 's/^/  /'
  exit 1
fi
exit 0
