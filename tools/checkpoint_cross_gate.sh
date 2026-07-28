#!/usr/bin/env bash
# Restore a checkpoint image with a DIFFERENT host backend than the one that wrote it.
#
# struct cpu is the checkpoint format -- sizeof(struct cpu) is written into the image and validated on
# restore -- and the stated reason is that the interpreter backend and the JIT backend must be able to read
# each other's guest state. Nothing tested that. This gate does: the image is captured by one engine and
# restored by the other, in both directions, for both guest ISAs.
#
# On an x86_64 host the two backends are the native INTERPRETER and the cross-built AArch64-host JIT run
# under qemu-aarch64, so this shares the emulated lane's boundary: read docs/amd64-host.md before
# trusting or distrusting a result. What it does vouch for is the part that has nothing to do with emulation
# -- that the image one backend writes is the image the other reads.
#
#   checkpoint_cross_gate.sh <cross-tree> <shim-dir> <runner> <fixture-dir> <isa> <direction> <fixture> [scenario]
#
# direction: interp-to-jit (capture native, restore cross) | jit-to-interp (capture cross, restore native)
#
# Exit 77 (SKIP_RETURN_CODE) when qemu-aarch64 or the cross tree is absent -- both are external to a default
# build. Anything else missing is a hard failure.
set -uo pipefail

if [ "$#" -lt 7 ] || [ "$#" -gt 8 ]; then
	printf 'usage: %s <cross-tree> <shim-dir> <runner> <fixture-dir> <isa> <direction> <fixture> [scenario]\n' "$0" >&2
	exit 2
fi
cross=$1
shim=$2
runner=$3
fixtures=$4
isa=$5
direction=$6
fixture=$7
scenario=${8:-}

qemu=${HL_QEMU_AARCH64:-$(command -v qemu-aarch64 2>/dev/null)}
if [ -z "$qemu" ] || [ ! -x "$qemu" ]; then
	cat >&2 <<-EOF
		checkpoint-cross: SKIPPED, this gate did NOT run and is NOT green.
		checkpoint-cross:   missing: qemu-aarch64 (not on PATH, and HL_QEMU_AARCH64 unset)
		checkpoint-cross: It is not in the flake devShell. Supply it with either:
		checkpoint-cross:   nix shell nixpkgs#qemu --command ctest ... -L checkpoint-cross
		checkpoint-cross:   HL_QEMU_AARCH64=/path/to/qemu-aarch64 ctest ... -L checkpoint-cross
	EOF
	exit 77
fi
if [ ! -x "$cross/hl-engine-linux-$isa" ]; then
	cat >&2 <<-EOF
		checkpoint-cross: SKIPPED, this gate did NOT run and is NOT green.
		checkpoint-cross:   missing: $cross/hl-engine-linux-$isa
		checkpoint-cross: The aarch64-host engines come from a second toolchain and a second configure that
		checkpoint-cross: no default build of this tree produces. Build them with:
		checkpoint-cross:   cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \\
		checkpoint-cross:         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake
		checkpoint-cross:   ninja -C build-arm-check
	EOF
	exit 77
fi

native=$(dirname -- "$runner")/../linux-production/hl-engine-linux-$isa
guest=$fixtures/checkpoint-$fixture-$isa
for path in "$runner" "$native" "$guest"; do
	if [ ! -e "$path" ]; then
		printf 'checkpoint-cross: %s is missing; build this tree first\n' "$path" >&2
		exit 1
	fi
done

# One shim per (isa, pid) so parallel cells cannot race on the same file.
mkdir -p "$shim" || exit 1
wrapper=$shim/hl-engine-linux-$isa.$$
printf '#!/bin/sh\nexec %s %s/hl-engine-linux-%s "$@"\n' "$qemu" "$cross" "$isa" >"$wrapper" || exit 1
chmod +x "$wrapper" || exit 1
trap 'rm -f "$wrapper"' EXIT

case $direction in
interp-to-jit)
	capture=$native
	export HL_CHECKPOINT_RESTORE_ENGINE=$wrapper
	;;
jit-to-interp)
	capture=$wrapper
	export HL_CHECKPOINT_RESTORE_ENGINE=$native
	;;
*)
	printf 'checkpoint-cross: unknown direction %s\n' "$direction" >&2
	exit 2
	;;
esac

# Emulation is roughly an order of magnitude slower, so the runner's per-wait budget has to grow with it.
# The runner prints the scale it used, and says in the same line that a scaled green is not a speed claim.
export HL_MATRIX_TIMEOUT_SCALE=${HL_MATRIX_TIMEOUT_SCALE:-30}

printf 'checkpoint-cross: %s %s %s under %s\n' "$isa" "$direction" "${scenario:-$fixture}" "$qemu"
if [ -n "$scenario" ]; then
	exec "$runner" "$capture" "$guest" "$scenario"
fi
exec "$runner" "$capture" "$guest"
