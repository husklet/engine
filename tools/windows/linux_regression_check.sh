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
#   5. a real compat suite on both trees -- matrix-runner driving actual Linux
#      guests. Only 1-4 exercise the BUILD; nothing above this line runs the
#      runner whose process supervision the Windows work edits, so a change that
#      compiled everywhere and broke the Linux supervision path would pass 1-4
#      cleanly. Skipped, loudly, when no cross compiler is present.
#
# Run from a Linux host or WSL:
#     tools/windows/linux_regression_check.sh [baseline-ref]
#
# A pre-existing baseline failure is not a regression -- what matters is that the
# two columns match. The script prints both rather than a single verdict, so a
# baseline that is already red stays visible instead of being subtracted away.
#
# UNCOMMITTED WORK. Both columns come from `git clone`, so by construction this
# compares two COMMITTED trees and cannot see a dirty working tree. That is not
# an oversight -- a dirty tree in a repository several lanes share contains other
# people's half-finished edits, and attributing their red to your change is worse
# than not measuring. Set HL_PATCH=<file> to apply one patch to the head column
# instead of checking out HL_HEAD_REF: the two columns then differ by exactly
# that patch and nothing else, which is what a regression claim needs.
#     git diff <your files> > /tmp/mine.patch
#     HL_PATCH=/tmp/mine.patch tools/windows/linux_regression_check.sh
set -u

SRC="${HL_SRC:-$(cd "$(dirname "$0")/../.." && pwd)}"
WORK="${HL_WORK:-$HOME/hl-regression}"
BASE="${1:-6d8514c3}"          # default: the feat/windows-amd64 branch point
HEAD_REF="${HL_HEAD_REF:-feat/windows-amd64}"
PATCH="${HL_PATCH:-}"
# The compat suites in step 5. Small on purpose: isa-x86-64 is eight committed
# binaries and core-abi is ~60 quick guests, so the step costs under a minute
# while still driving matrix-runner end to end -- fork, process group, stall
# sampling, capture, golden compare.
COMPAT_SUITES="${HL_COMPAT_SUITES:-isa-x86-64 core-abi}"

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

# Put the work tree in the state one column names. Both columns are produced
# from this one checkout in sequence, so every step that needs sources must call
# it rather than assuming what the previous step left behind.
checkout_at () {
    local ref="$1" tag="$2"
    git checkout -q -f . 2>/dev/null
    if [ -n "$PATCH" ] && [ "$tag" = head ]; then
        # Head column = baseline + exactly this patch. Applied on top of BASE, not
        # HEAD_REF, so the diff between the two columns is the patch alone.
        git checkout -q -f "$BASE" || { echo "FATAL: checkout $BASE failed"; exit 1; }
        # exit, not return: a caller that only checks build_at's status prints an
        # EMPTY head column and then compares steps 2-5 against a build directory
        # that was never created -- which reads as "no differences" and is the
        # exact silent pass this script exists to prevent. The default BASE is the
        # branch point, and a patch cut against a later HEAD will not apply to it,
        # so this fires often and must fire hard.
        git apply "$PATCH" || {
            echo "FATAL: $PATCH does not apply to BASE=$BASE."
            echo "       A patch cut against current HEAD needs the commit it was cut from:"
            echo "           HL_PATCH=$PATCH $0 \$(git rev-parse --short HEAD)"
            exit 1
        }
    else
        git checkout -q -f "$ref" || { echo "FATAL: checkout $ref failed"; return 1; }
    fi
}

build_at () {
    local ref="$1" tag="$2"
    checkout_at "$ref" "$tag" || return 1
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

echo
echo "=== 5. compat suites (matrix-runner driving real guests) ==="
# The compat suites need the guest cross compilers. cmake/GuestFixtures.cmake
# reads exactly these variables; the Ubuntu defaults below are the same shape
# tools/windows/build_guest_fixtures.sh uses, and are only defaults -- an
# already-exported value (the nix devShell's) wins.
: "${AARCH64_LINUX_STATIC_CC:=aarch64-linux-gnu-gcc -L/usr/lib/aarch64-linux-gnu}"
: "${X86_64_LINUX_STATIC_CC:=gcc}"
: "${AARCH64_LINUX_CC:=aarch64-linux-gnu-gcc}"
: "${X86_64_LINUX_CC:=gcc}"
: "${AARCH64_DYNAMIC_LOADER:=/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1}"
: "${AARCH64_DYNAMIC_LIBC:=/usr/aarch64-linux-gnu/lib/libc.so.6}"
: "${X86_64_DYNAMIC_LOADER:=/lib64/ld-linux-x86-64.so.2}"
: "${X86_64_DYNAMIC_LIBC:=/lib/x86_64-linux-gnu/libc.so.6}"
export AARCH64_LINUX_STATIC_CC X86_64_LINUX_STATIC_CC AARCH64_LINUX_CC X86_64_LINUX_CC
export AARCH64_DYNAMIC_LOADER AARCH64_DYNAMIC_LIBC X86_64_DYNAMIC_LOADER X86_64_DYNAMIC_LIBC
if ! command -v "${AARCH64_LINUX_CC%% *}" >/dev/null 2>&1; then
    # Loud, not silent: a skipped step that prints nothing is indistinguishable
    # from a step that ran and found nothing wrong.
    echo "SKIPPED: no $AARCH64_LINUX_CC on PATH, so no guest fixtures can be built."
    echo "         Steps 1-4 above exercise the BUILD only; the Linux process"
    echo "         supervision in tools/matrix_runner.c is NOT covered by them."
else
    for t in baseline head; do
        if [ "$t" = baseline ]; then checkout_at "$BASE" baseline; else checkout_at "$HEAD_REF" head; fi || continue
        rm -rf "build-$t-guest"
        cmake -G Ninja -B "build-$t-guest" -DHL_BUILD_TESTS=ON -DHL_GUEST_TESTS=ON \
            > "cfg-$t-guest.log" 2>&1 || { echo "$t: configure with guest fixtures FAILED"; continue; }
        targets="compat-engines"
        for s in $COMPAT_SUITES; do targets="$targets compat-$s-fixtures"; done
        # --target, not bare words: without it cmake reads the first target as a
        # second positional argument, prints its usage and exits non-zero, so the
        # step reported "fixture/engine build FAILED" on BOTH columns and the
        # strongest check in this script had never actually run.
        # shellcheck disable=SC2086
        cmake --build "build-$t-guest" --target $targets > "build-$t-guest.log" 2>&1 ||
            { echo "$t: fixture/engine build FAILED"; continue; }
        for s in $COMPAT_SUITES; do
            printf '%-9s compat.%-12s %s\n' "$t:" "$s" \
                "$(ctest --test-dir "build-$t-guest" -R "^compat\.$s\$" --no-tests=error 2>&1 \
                   | grep -E 'tests passed' || echo 'NO RESULT')"
        done
    done
fi
