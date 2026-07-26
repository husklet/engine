#!/usr/bin/env bash
# Validate one of the exact static archives shipped by the Rust crate.
set -euo pipefail

if [ "$#" -ne 2 ]; then
	printf 'usage: %s <aarch64-unknown-linux-gnu|aarch64-apple-darwin> <archive>\n' "$0" >&2
	exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

target=$1
archive=$2

# Darwin archive inspection and linking must happen on Darwin. In particular,
# do not make Linux read an archive while libtool is still publishing it
# through the shared mount.
if [ "$target" = aarch64-apple-darwin ] && [ "$(uname -s)" != Darwin ]; then
	if command -v mac >/dev/null 2>&1; then
		exec mac "$root/tools/validate_crate_archive.sh" "$target" "$archive"
	fi
	# No Darwin host reachable: hosted Linux runners are in this position, and
	# demanding the link test there made check-crate-archives unconditionally
	# red no matter how fresh the archive was. Do the host-independent part --
	# ar container, member list, required symbols -- and leave the Mach-O link
	# test to the macOS workflow. The freshness digest and the recorded SHA-256
	# in tools/check_crate_archives.sh are unaffected by this.
	[ -s "$archive" ] || {
		printf 'validate-crate-archive: missing or empty archive: %s\n' "$archive" >&2
		exit 1
	}
	[ "$(head -c 8 "$archive")" = '!<arch>' ] || {
		printf 'validate-crate-archive: %s is not an ar archive\n' "$archive" >&2
		exit 1
	}
	members=$(ar -t "$archive")
	[ -n "$members" ] || {
		printf 'validate-crate-archive: %s has no archive members\n' "$archive" >&2
		exit 1
	}
	for symbol in hl_engine_create hl_host_process_open hl_production_clock_gettime; do
		grep -aq "_$symbol" "$archive" || {
			printf 'validate-crate-archive: %s is missing required symbol %s\n' \
				"$archive" "$symbol" >&2
			exit 1
		}
	done
	printf 'validated crate archive (no Darwin host; link test deferred to macOS CI): %s\n' \
		"$archive"
	exit 0
fi

[ -s "$archive" ] || {
	printf 'validate-crate-archive: missing or empty archive: %s\n' "$archive" >&2
	exit 1
}

scratch=${TMPDIR:-/tmp}/hl-crate-archive-validate.$$
mkdir -p "$scratch"
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

members=$scratch/members
symbols=$scratch/symbols
if [ "$target" = aarch64-apple-darwin ]; then
	/usr/bin/ar -t "$archive" >"$members"
	# Apple libtool has no read-only TOC command. Repacking the input into
	# scratch forces it to consume every member and produce a fresh TOC.
	/usr/bin/libtool -static -o "$scratch/repacked.a" "$archive"
	/usr/bin/ar -t "$scratch/repacked.a" >/dev/null
	/usr/bin/nm -gU "$archive" >"$symbols"
	linker=(clang -Iinclude -o "$scratch/link-test" tools/dual_backend_e2e_runner.c
		-Wl,-force_load,"$archive")
else
	ar -t "$archive" >"$members"
	nm --print-armap "$archive" >"$scratch/armap"
	grep -q '^Archive index:' "$scratch/armap"
	nm -g --defined-only "$archive" >"$symbols"
	: "${AARCH64_LINUX_CC:=aarch64-linux-gnu-gcc}"
	linker=("$AARCH64_LINUX_CC" -D_GNU_SOURCE -Iinclude -o "$scratch/link-test"
		tools/dual_backend_e2e_runner.c -Wl,--whole-archive "$archive"
		-Wl,--no-whole-archive -pthread -ldl -lm -latomic)
fi

[ -s "$members" ] || {
	printf 'validate-crate-archive: %s has no archive members\n' "$archive" >&2
	exit 1
}

for symbol in hl_engine_create hl_host_process_open hl_production_clock_gettime; do
	if ! grep -Eq "[[:space:]]_?${symbol}$" "$symbols"; then
		printf 'validate-crate-archive: %s is missing required symbol %s\n' "$archive" "$symbol" >&2
		exit 1
	fi
done

"${linker[@]}"
printf 'validated crate archive: %s\n' "$archive"
