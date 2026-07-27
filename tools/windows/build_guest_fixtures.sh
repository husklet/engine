#!/usr/bin/env bash
# Cross-build the guest fixture corpus on a Linux userspace and export it for a
# host that has none -- in practice, a WSL2 Ubuntu serving the Windows checkout
# it is mounted on.
#
# WHY A SCRIPT AND NOT A CMAKE ARM: the corpus is ~3200 cross-compiled Linux
# programs, and the only trustworthy definition of what they are and how each is
# compiled is cmake/Phase3Compat.cmake plus cmake/Phase3Gates.cmake. Re-stating
# any of that here would be a second copy free to drift from the first, and the
# whole point of a fixture is that it was built the way the real build builds it.
# So this script does not compile anything itself: it configures THIS repo's own
# CMake on Linux with the cross compilers exported the way the nix devShell
# exports them, builds the two fixture targets, and copies out exactly the files
# the build says are fixtures (build/guest-fixtures.list, written by
# hl_guest_finalize).
#
# WHY IT STAGES A COPY: a WSL2 view of a Windows drive is DrvFs, whose per-file
# open cost is roughly an order of magnitude above ext4. Compiling 3200 binaries
# through it, each linking a static glibc, spends most of its wall clock in the
# filesystem. rsync-ing the tree onto the Linux filesystem first and copying only
# the results back turns that into two bulk transfers.
#
# WHAT CORRECT LOOKS LIKE (flake.nix, guestISAs / staticCCFor):
#   * one cross compiler per guest ISA, each with a STATIC glibc for that ISA;
#   * a static libsqlite3 for any ISA whose tests link -lsqlite3 -- testNeedsSqlite
#     is true for aarch64 (the dbserver/sqlite compat workloads and the
#     combined-bench sqlite phase) and false for x86_64;
#   * *_DYNAMIC_LOADER / *_DYNAMIC_LIBC for the handful of dynamically linked
#     rootfs-staging cases, or they bake host paths that exist nowhere.
# The Ubuntu packages below are the same shape from a different supplier. They
# are NOT the same glibc as CI's, so a corpus built here is a local development
# corpus: it proves the suites run, and it is not evidence about a golden that
# encodes a libc version.
#
# usage:
#   tools/windows/build_guest_fixtures.sh [--src DIR] [--out DIR] [--work DIR]
#                                         [--jobs N] [--reuse]
set -u

self=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$self/../.." && pwd)
out=""
work="${HL_FIXTURE_WORK:-$HOME/.cache/hl-guest-fixtures}"
jobs=$(nproc 2>/dev/null || echo 4)
reuse=0

while [ $# -gt 0 ]; do
	case $1 in
	--src) src=$2; shift 2 ;;
	--out) out=$2; shift 2 ;;
	--work) work=$2; shift 2 ;;
	--jobs) jobs=$2; shift 2 ;;
	--reuse) reuse=1; shift ;;      # keep the staged tree and build dir
	-h|--help) sed -n '2,37p' "$0"; exit 0 ;;
	*) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
	esac
done

src=$(CDPATH= cd -- "$src" && pwd) || exit 2
# Default matches cmake/GuestFixtures.cmake's HL_GUEST_PREBUILT_DIR default, so
# the common case needs no argument on either side.
[ -n "$out" ] || out=$src/build-guest-fixtures

die() { printf '\nbuild_guest_fixtures: %s\n' "$1" >&2; exit 1; }

# --- 1. toolchain ----------------------------------------------------------
# Overridable so this is not welded to Ubuntu; the defaults are what
# `apt-get install gcc-aarch64-linux-gnu libsqlite3-dev:arm64` provides.
A64_CC=${HL_A64_CC:-aarch64-linux-gnu-gcc}
X64_CC=${HL_X64_CC:-gcc}
# aarch64-linux-gnu-gcc searches /usr/aarch64-linux-gnu/lib, where the cross
# libc lives; the arm64 multiarch packages install into /usr/lib/aarch64-linux-gnu,
# where libsqlite3.a lives. Both are aarch64, and only the second needs saying.
A64_EXTRA_L=${HL_A64_EXTRA_L:-/usr/lib/aarch64-linux-gnu}

have_static() {   # <compiler> <library archive name>
	local p
	p=$("$1" -print-file-name="$2" 2>/dev/null) || return 1
	[ -f "$p" ]
}

missing=""
command -v "$A64_CC" >/dev/null 2>&1 || missing="$missing gcc-aarch64-linux-gnu"
command -v "$X64_CC" >/dev/null 2>&1 || missing="$missing gcc"
command -v cmake >/dev/null 2>&1 || missing="$missing cmake"
command -v ninja >/dev/null 2>&1 || missing="$missing ninja-build"
command -v rsync >/dev/null 2>&1 || missing="$missing rsync"
if [ -n "$missing" ]; then
	die "missing tools:$missing
    sudo apt-get install -y$missing"
fi
have_static "$X64_CC" libc.a ||
	die "no static glibc for the native x86_64 compiler (libc.a).
    sudo apt-get install -y libc6-dev"
have_static "$A64_CC" libc.a ||
	die "no static glibc for $A64_CC (libc.a).
    sudo apt-get install -y libc6-dev-arm64-cross"
# testNeedsSqlite is true for aarch64 only, so this one is required and the
# x86_64 equivalent is deliberately not looked for.
[ -f "$A64_EXTRA_L/libsqlite3.a" ] ||
	die "no static aarch64 libsqlite3 at $A64_EXTRA_L/libsqlite3.a -- the
    compat core dbserver/sqlite workloads and combined-bench link it. On Ubuntu:
    sudo dpkg --add-architecture arm64
    (add an arm64 ports.ubuntu.com apt source, restrict the amd64 ones)
    sudo apt-get install -y libsqlite3-dev:arm64"
[ -f /usr/include/sqlite3.h ] || die "no sqlite3.h: sudo apt-get install -y libsqlite3-dev"

# The dynamic-linkage cases stage a loader and libc into a guest rootfs. Wrong
# paths here do not fail the build, they produce guests that cannot start, so
# check rather than hope.
A64_LOADER=${HL_A64_LOADER:-/usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1}
A64_LIBC=${HL_A64_LIBC:-/usr/aarch64-linux-gnu/lib/libc.so.6}
X64_LOADER=${HL_X64_LOADER:-/lib64/ld-linux-x86-64.so.2}
X64_LIBC=${HL_X64_LIBC:-/lib/x86_64-linux-gnu/libc.so.6}
for f in "$A64_LOADER" "$A64_LIBC" "$X64_LOADER" "$X64_LIBC"; do
	[ -e "$f" ] || die "dynamic loader/libc not found: $f
    (libc6-arm64-cross supplies the aarch64 pair; override with HL_A64_LOADER etc.)"
done

a64_version=$("$A64_CC" --version | head -1)
x64_version=$("$X64_CC" --version | head -1)
printf 'toolchain aarch64: %s\n' "$a64_version"
printf 'toolchain x86_64:  %s\n' "$x64_version"

# --- 2. stage the tree onto a native filesystem ----------------------------
stage=$work/src
build=$work/build
if [ "$reuse" -eq 0 ]; then
	rm -rf "$build"
fi
mkdir -p "$stage" "$build" || die "cannot create $work"

printf 'staging %s -> %s\n' "$src" "$stage"
# --delete so a source file removed upstream is removed here too; a stale .c
# left in the staged tree would be swept up by hl_guest_suite()'s glob and
# produce a fixture this tree no longer declares.
# Every exclude is ANCHORED at the transfer root. An unanchored `target/` also
# matches src/core/target/, which holds the two per-ISA translator TUs the
# production engines are built from -- excluding it configures a tree whose
# dual_*_target objects have no sources.
rsync -a --delete \
	--exclude '/.git/' --exclude '/build/' --exclude '/build-*/' \
	--exclude '/pkgs/rust/target/' --exclude '/result' \
	"$src/" "$stage/" || die "rsync failed"

# --- 3. configure with the cross compilers, exactly as the devShell does ----
export AARCH64_LINUX_CC="$A64_CC"
export AARCH64_LINUX_STATIC_CC="$A64_CC -L$A64_EXTRA_L"
export X86_64_LINUX_CC="$X64_CC"
export X86_64_LINUX_STATIC_CC="$X64_CC"
export AARCH64_DYNAMIC_LOADER="$A64_LOADER"
export AARCH64_DYNAMIC_LIBC="$A64_LIBC"
export X86_64_DYNAMIC_LOADER="$X64_LOADER"
export X86_64_DYNAMIC_LIBC="$X64_LIBC"

printf 'configuring %s\n' "$build"
cmake -G Ninja -S "$stage" -B "$build" -DHL_BUILD_TESTS=ON -DHL_GUEST_TESTS=ON \
	>"$work/configure.log" 2>&1 || {
	tail -40 "$work/configure.log" >&2
	die "configure failed -- full log at $work/configure.log"
}

# --- 4. build the fixtures, and only the fixtures --------------------------
# guest-fixtures-gates is finalized after guest-fixtures and therefore covers
# the accumulated set (compat + e2e + perf + soak); both are named so a future
# split cannot quietly drop one. -k 0 keeps going after a failure: a corpus with
# a known, listed hole is a usable result, a build that stops at the first bad
# fixture is not.
printf 'building fixtures with %s jobs\n' "$jobs"
cmake --build "$build" --target guest-fixtures guest-fixtures-gates \
	-- -k 0 -j "$jobs" >"$work/build.log" 2>&1
build_status=$?

list=$build/guest-fixtures.list
[ -s "$list" ] || die "no $list -- the configure declared no fixtures"

# --- 5. export ------------------------------------------------------------
declared=0
built=0
failed_list=$work/failed.txt
ok_list=$work/built.txt
: >"$failed_list"
: >"$ok_list"

while IFS= read -r rel; do
	[ -n "$rel" ] || continue
	declared=$((declared + 1))
	if [ -f "$build/$rel" ]; then
		printf '%s\n' "$rel" >>"$ok_list"
		built=$((built + 1))
	else
		printf '%s\n' "$rel" >>"$failed_list"
	fi
done <"$list"

failed=$((declared - built))

# Emptied first, then one rsync rather than 3200 copies: the destination is
# usually a DrvFs mount, where per-file syscalls dominate. Emptying is what
# guarantees the corpus holds nothing but what this tree declares -- a fixture
# left behind from an older recipe is drift the source digest cannot see,
# because the digest describes the inputs, not the leftovers.
# No -p: DrvFs refuses utime on a file it has just created, and a guest
# fixture's mtime means nothing to the engine that loads it.
rm -rf "$out"
mkdir -p "$out" || die "cannot create $out"
rsync -rl --files-from="$ok_list" "$build/" "$out/" ||
	die "cannot export the corpus to $out"

# --- 6. manifest ----------------------------------------------------------
# The digest comes from the ORIGINAL tree, not the staged copy: it is the tree
# the consuming configure will hash, and computing it from the same helper the
# consumer uses is what makes the two comparable at all.
digest=$(cmake -DHL_SOURCE_DIR="$src" -P "$src/tools/guest_fixture_digest.cmake" 2>&1) ||
	die "cannot compute the source digest"

commit=$(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo unknown)
dirty=clean
git -C "$src" diff --quiet 2>/dev/null || dirty=dirty

{
	printf '# hl guest fixture corpus -- generated by tools/windows/build_guest_fixtures.sh\n'
	printf '# Do not edit: cmake/GuestFixtures.cmake reads source-digest to decide\n'
	printf '# whether this corpus still describes the tree being configured.\n'
	printf 'source-digest: %s\n' "$digest"
	printf 'fixture-count: %d\n' "$built"
	printf 'fixture-declared: %d\n' "$declared"
	printf 'fixture-failed: %d\n' "$failed"
	printf 'built-at: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'source-commit: %s (%s)\n' "$commit" "$dirty"
	printf 'toolchain-aarch64: %s\n' "$a64_version"
	printf 'toolchain-x86_64: %s\n' "$x64_version"
	printf 'toolchain-host: %s\n' "$(uname -sr)"
	printf 'toolchain-sqlite-aarch64: %s\n' "$A64_EXTRA_L/libsqlite3.a"
	if [ "$failed" -gt 0 ]; then
		printf '# fixtures declared but NOT built:\n'
		sed 's/^/# missing: /' "$failed_list"
	fi
} >"$out/MANIFEST.txt"

printf '\ncorpus: %s\n' "$out"
printf '  declared %d, built %d, failed %d\n' "$declared" "$built" "$failed"
printf '  source-digest %s\n' "$digest"
printf '  size %s\n' "$(du -sh "$out" | cut -f1)"
if [ "$failed" -gt 0 ]; then
	printf '\nFAILED fixtures (%d), first 40:\n' "$failed"
	head -40 "$failed_list"
	printf 'full list: %s\nbuild log: %s\n' "$failed_list" "$work/build.log"
fi
[ "$build_status" -eq 0 ] || printf '\nnote: the fixture build exited %d\n' "$build_status"
exit 0
