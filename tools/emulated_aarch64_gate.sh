#!/usr/bin/env bash
# Run one compat suite against the AArch64-HOST engines under qemu-user.
#
# EMULATION, NOT HARDWARE. This lane exists because the aarch64 host arm is
# built on an x86_64 CI host but never executed there, so JIT changes ship
# unrun. qemu-aarch64 executes them. What that is and is not worth is measured
# in docs/emulated-aarch64.md -- read it before trusting or distrusting a
# result. In one line: it vouches for instruction semantics, the W^X arena and
# signal handling; it does NOT vouch for weak memory ordering or timing.
#
#   emulated_aarch64_gate.sh <cross-tree> <shim-dir> <matrix-runner> \
#                            <bindir> <suitedir>
#
# Exit 77 (SKIP_RETURN_CODE) when qemu-aarch64 or the cross tree is absent --
# both are external to a default build. Anything else missing is a hard failure.
set -uo pipefail

if [ "$#" -ne 5 ]; then
	printf 'usage: %s <cross-tree> <shim-dir> <matrix-runner> <bindir> <suitedir>\n' "$0" >&2
	exit 2
fi
cross=$1
shim=$2
runner=$3
bindir=$4
suitedir=$5

qemu=${HL_QEMU_AARCH64:-$(command -v qemu-aarch64 2>/dev/null)}
if [ -z "$qemu" ] || [ ! -x "$qemu" ]; then
	cat >&2 <<EOF
emulated-aarch64: SKIPPED, this gate did NOT run and is NOT green.
emulated-aarch64:   missing: qemu-aarch64 (not on PATH, and HL_QEMU_AARCH64 unset)
emulated-aarch64: It is not in the flake devShell. Supply it with either:
emulated-aarch64:   nix shell nixpkgs#qemu --command ctest ... -L emulated-aarch64
emulated-aarch64:   HL_QEMU_AARCH64=/path/to/qemu-aarch64 ctest ... -L emulated-aarch64
EOF
	exit 77
fi

for isa in aarch64 x86_64; do
	if [ ! -x "$cross/hl-engine-linux-$isa" ]; then
		cat >&2 <<EOF
emulated-aarch64: SKIPPED, this gate did NOT run and is NOT green.
emulated-aarch64:   missing: $cross/hl-engine-linux-$isa
emulated-aarch64: The aarch64-host engines come from a second toolchain and a second
emulated-aarch64: configure that no default build of this tree produces. Build them with:
emulated-aarch64:   cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \\
emulated-aarch64:         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
emulated-aarch64:   ninja -C build-arm-check
EOF
		exit 77
	fi
done

for path in "$runner" "$bindir" "$suitedir"; do
	if [ ! -e "$path" ]; then
		printf 'emulated-aarch64: %s is missing; build this tree first\n' "$path" >&2
		exit 1
	fi
done

# matrix-runner resolves the remote supervisor NEXT TO the engine it is given, so
# the shims cannot simply live beside the cross tree's engines: that directory
# has no supervisor and is a build output. Build a directory of our own holding
# both shims plus this tree's NATIVE supervisor -- the supervisor is the harness
# side of the wire and is never the thing under test.
supervisor=$(dirname -- "$runner")/../linux-production/hl-remote-supervisor
if [ ! -x "$supervisor" ]; then
	printf 'emulated-aarch64: native supervisor %s is missing\n' "$supervisor" >&2
	exit 1
fi
mkdir -p "$shim" || exit 1
cp -f "$supervisor" "$shim/hl-remote-supervisor" || exit 1
for isa in aarch64 x86_64; do
	printf '#!/bin/sh\nexec %s %s/hl-engine-linux-%s "$@"\n' \
		"$qemu" "$cross" "$isa" >"$shim/hl-engine-linux-$isa" || exit 1
	chmod +x "$shim/hl-engine-linux-$isa" || exit 1
done

printf 'emulated-aarch64: %s under %s\n' "$(basename -- "$suitedir")" "$qemu"
exec "$runner" env \
	"$shim/hl-engine-linux-aarch64" "$bindir/aarch64" \
	"$shim/hl-engine-linux-x86_64" "$bindir/x86_64" \
	"$suitedir"
