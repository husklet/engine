#!/usr/bin/env bash
# Validate one of the exact static archives shipped by the Rust crate.
set -euo pipefail

if [ "$#" -ne 2 ]; then
	printf 'usage: %s <host-triple> <archive>\n' "$0" >&2
	printf '  host-triple: aarch64-apple-darwin | {aarch64,x86_64}-unknown-linux-gnu\n' >&2
	printf '               | x86_64-pc-windows-msvc\n' >&2
	exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

target=$1
archive=$2

# The triple's two halves drive every decision below -- which host can inspect the
# archive (ELF vs Mach-O) and which compiler links it -- so split them once rather
# than matching whole triples.
target_arch=${target%%-*}
case "$target" in
aarch64-apple-darwin) target_os=darwin ;;
aarch64-unknown-linux-gnu | x86_64-unknown-linux-gnu) target_os=linux ;;
x86_64-pc-windows-msvc) target_os=windows ;;
*)
	printf 'validate-crate-archive: unknown host triple %s\n' "$target" >&2
	exit 2
	;;
esac

# The Windows archive is checked STRUCTURALLY only, on any host, and never
# link-tested. Two reasons, and the second is the one that matters:
#
#   * the tools differ. It is an MSVC-format COFF archive, so `nm` and `ar` are
#     llvm-nm and llvm-ar, and the linker is link.exe -- none of which the Linux
#     or Darwin lanes have, and the deferral machinery below is built around the
#     ELF/Mach-O pair.
#   * there is nothing to link it INTO yet. The link test the other two hosts run
#     compiles tools/dual_backend_e2e_runner.c against the archive, and that
#     runner needs the activation entry points. src/core/activation.c has no
#     Windows arm, so this archive does not contain them and the test would fail
#     for a reason that has nothing to do with the archive's integrity.
#
# So: format, members, and the three required symbols. Restore the link test in
# the same shape as the others once the archive carries an activation layer.
if [ "$target_os" = windows ]; then
	[ -s "$archive" ] || {
		printf 'validate-crate-archive: missing or empty archive: %s\n' "$archive" >&2
		exit 1
	}
	[ "$(head -c 8 "$archive")" = '!<arch>' ] || {
		printf 'validate-crate-archive: %s is not an ar archive\n' "$archive" >&2
		exit 1
	}
	nm_tool=${HL_LLVM_NM:-llvm-nm}
	if command -v "$nm_tool" >/dev/null 2>&1; then
		# tr -d '\r': llvm-nm terminates lines CRLF on a Windows host, so a
		# `$`-anchored match against a symbol name never fires and every
		# required symbol is reported missing while being plainly present.
		symbols=$("$nm_tool" --defined-only "$archive" 2>/dev/null | tr -d '\r')
		[ -n "$symbols" ] || {
			printf 'validate-crate-archive: %s defines no symbols\n' "$archive" >&2
			exit 1
		}
		# A here-string rather than `printf ... | grep -q`. Under `set -o
		# pipefail` that pipeline reports FAILURE on success: grep -q exits the
		# moment it matches, printf takes EPIPE, and the pipeline's status is
		# printf's. Every symbol was then reported missing while being present,
		# which is the most misleading direction for this check to fail in.
		for symbol in hl_engine_create hl_host_process_open hl_production_clock_gettime; do
			grep -Eq "[[:space:]]${symbol}$" <<<"$symbols" || {
				printf 'validate-crate-archive: %s is missing required symbol %s\n' \
					"$archive" "$symbol" >&2
				exit 1
			}
		done
		printf 'validated crate archive (structural; MSVC-ABI COFF): %s\n' "$archive"
	else
		# The byte-string fallback the deferral path below also uses: a symbol
		# name is present in the archive's string table whether or not a COFF
		# reader is installed.
		for symbol in hl_engine_create hl_host_process_open hl_production_clock_gettime; do
			grep -aq "$symbol" "$archive" || {
				printf 'validate-crate-archive: %s is missing required symbol %s\n' \
					"$archive" "$symbol" >&2
				exit 1
			}
		done
		printf 'validated crate archive (structural, no llvm-nm): %s\n' "$archive"
	fi
	exit 0
fi

# An archive can only be inspected and linked on its own kind of host: the
# Darwin half needs Apple libtool/nm/clang, the Linux half needs GNU nm and the
# aarch64 cross compiler. Neither host has the other's tools, so each defers the
# link test to the CI job that runs there. Demanding it on the wrong host made
# check-crate-archives unconditionally red no matter how fresh the archive was.
# The freshness digest and the recorded SHA-256 in check_crate_archives.sh are
# unaffected -- a stale archive still fails on both hosts.
host=$(uname -s)
defer_reason=
if [ "$target_os" = darwin ] && [ "$host" != Darwin ]; then
	# Prefer a real Darwin host when the bridge exists. Do not let Linux read
	# the archive while libtool is still publishing it through the shared mount.
	if command -v mac >/dev/null 2>&1; then
		exec mac "$root/tools/validate_crate_archive.sh" "$target" "$archive"
	fi
	defer_reason='no Darwin host; Mach-O link test deferred to macOS CI'
	symbol_prefix=_
elif [ "$target_os" = linux ] && [ "$host" != Linux ]; then
	defer_reason='no Linux host; ELF link test deferred to Linux CI'
	symbol_prefix=
fi

if [ -n "$defer_reason" ]; then
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
		grep -aq "$symbol_prefix$symbol" "$archive" || {
			printf 'validate-crate-archive: %s is missing required symbol %s\n' \
				"$archive" "$symbol" >&2
			exit 1
		}
	done
	printf 'validated crate archive (%s): %s\n' "$defer_reason" "$archive"
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
if [ "$target_os" = darwin ]; then
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
	# One variable per HOST CPU, matching the names flake.nix exports. The
	# non-native one is the devShell's cross gcc, so an x86_64 host can still
	# link-test the committed aarch64 archive.
	case "$target_arch" in
	aarch64) : "${AARCH64_LINUX_CC:=aarch64-linux-gnu-gcc}"; cc=$AARCH64_LINUX_CC ;;
	x86_64) : "${X86_64_LINUX_CC:=x86_64-linux-gnu-gcc}"; cc=$X86_64_LINUX_CC ;;
	esac
	linker=($cc -D_GNU_SOURCE -Iinclude -o "$scratch/link-test"
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
