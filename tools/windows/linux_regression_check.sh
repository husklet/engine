#!/usr/bin/env bash
# Prove the Windows port has not regressed the Linux host.
#
# The Windows work edits shared build files (CMakeLists.txt, eight cmake/*.cmake)
# and one shared header (src/host/native_context.h). All of it is supposed to be
# inert on Linux. "Supposed to be" is not evidence, and none of it can be checked
# from a Windows box, so this script builds the branch POINT and the current HEAD
# side by side under an identical configure and compares them.
#
# It checks four things, in increasing order of strength:
#   1. configure + build exit status and warning counts
#   2. the flag SET on a representative TU, from the generated ninja file
#   3. the flag ORDER -- CMakeLists.txt moved -fvisibility=hidden out of the
#      inline hl_engine_cflags list into a trailing guarded
#      target_compile_options and claimed order was preserved byte-for-byte.
#      Only a diff of the generated command line can settle that.
#   4. ctest -L unit on both trees
#
# Run from a Linux host or WSL:
#     tools/windows/linux_regression_check.sh [baseline-ref]
#
# A pre-existing baseline failure is not a regression -- what matters is that the
# two columns match. The script prints both rather than a single verdict, so a
# baseline that is already red stays visible instead of being subtracted away.
set -u

SRC="${HL_SRC:-$(cd "$(dirname "$0")/../.." && pwd)}"
WORK="${HL_WORK:-$HOME/hl-regression}"
BASE="${1:-6d8514c3}"          # default: the feat/windows-amd64 branch point
HEAD_REF="${HL_HEAD_REF:-feat/windows-amd64}"

# A DrvFs-mounted checkout never matches the WSL uid, so git's dubious-ownership
# guard fires -- on the .git path as well as the work tree.
git config --global --add safe.directory "$SRC" 2>/dev/null
git config --global --add safe.directory "$SRC/.git" 2>/dev/null

echo "source:   $SRC"
echo "baseline: $BASE"
echo "head:     $HEAD_REF"

rm -rf "$WORK"
git clone -q "$SRC" "$WORK" || { echo "FATAL: clone failed"; exit 1; }
cd "$WORK" || exit 1

build_at () {
    local ref="$1" tag="$2"
    git checkout -q "$ref" || { echo "FATAL: checkout $ref failed"; return 1; }
    rm -rf "build-$tag"
    cmake -G Ninja -B "build-$tag" -DHL_BUILD_TESTS=ON > "cfg-$tag.log" 2>&1
    local cfg=$?
    cmake --build "build-$tag" > "build-$tag.log" 2>&1
    local b=$?
    printf '%-9s configure=%d build=%d warnings=%s\n' \
        "$tag:" "$cfg" "$b" "$(grep -c 'warning:' "build-$tag.log")"
    return $(( cfg || b ))
}

echo
echo "=== 1. configure, build, warnings ==="
build_at "$BASE" baseline
build_at "$HEAD_REF" head

echo
echo "=== 2/3. compiler command line for src/core/engine.c ==="
for t in baseline head; do
    grep -A4 "build CMakeFiles/hl-engine.dir/src/core/engine.c.o" "build-$t/build.ninja" \
        | grep -o 'FLAGS = .*' > "/tmp/hl-ord-$t.txt"
    tr ' ' '\n' < "/tmp/hl-ord-$t.txt" | grep -E '^-' | sort > "/tmp/hl-set-$t.txt"
done
if diff -q /tmp/hl-set-baseline.txt /tmp/hl-set-head.txt >/dev/null; then
    echo "flag SET:   identical ($(wc -l < /tmp/hl-set-head.txt) flags)"
else
    echo "flag SET:   DIFFERS"; diff /tmp/hl-set-baseline.txt /tmp/hl-set-head.txt
fi
if diff -q /tmp/hl-ord-baseline.txt /tmp/hl-ord-head.txt >/dev/null; then
    echo "flag ORDER: identical"
else
    echo "flag ORDER: DIFFERS"; diff /tmp/hl-ord-baseline.txt /tmp/hl-ord-head.txt
fi
printf 'fvisibility=hidden present: baseline=%s head=%s\n' \
    "$(grep -c 'fvisibility=hidden' /tmp/hl-set-baseline.txt)" \
    "$(grep -c 'fvisibility=hidden' /tmp/hl-set-head.txt)"

echo
echo "=== 4. ctest -L unit ==="
for t in baseline head; do
    printf '%-9s %s\n' "$t:" \
        "$(ctest --test-dir "build-$t" -L unit --no-tests=error 2>&1 | grep -E 'tests passed' || echo 'NO RESULT')"
done
