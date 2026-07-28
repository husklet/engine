#!/usr/bin/env bash
# Regenerate the prebuilt crate archives under pkgs/rust/assets/lib/ and rewrite
# pkgs/rust/assets/PROVENANCE.md.
#
# The crate at pkgs/rust/ never compiles src/: build.rs links the committed
# archives and `cargo publish` ships those bytes. Run this whenever a C source
# or header changes, and commit the result together with the source change.
#
# The three halves are independently runnable so no single machine has to be
# both an aarch64 Linux host and an Apple silicon mac:
#
#   --linux       build+install this host CPU's NATIVE {aarch64,x86_64}-unknown-
#                 linux-gnu archive (needs a Linux host)
#   --windows     build+install x86_64-pc-windows-msvc (needs a Windows host with
#                 clang and the Visual Studio Build Tools). NOT part of the
#                 no-flags default and NOT covered by --all: see the note below.
#   --darwin      build+install aarch64-apple-darwin (needs Darwin, or the `mac`
#                 bridge over a shared checkout)
#   --emit PATH   with --darwin: write the built archive to PATH instead of
#                 installing it, so it can be carried to another host
#   --from PATH   with --darwin: install PATH's bytes instead of building
#   --provenance  rewrite the generated block from the PUBLISHED archives already
#                 in the tree, then run the freshness check
#
# No flags means --linux --darwin --provenance, the original one-shot dual-host
# behaviour.
#
# PUBLISHED is narrower than SUPPORTED: only the two aarch64 archives are in
# PROVENANCE.md; the x86_64 Linux one and the Windows one are LOCAL build
# products.
#
# The Windows archive is local for a reason that will not change soon, so it is
# worth stating rather than leaving to be rediscovered. It is ~24 MB, the same
# order as the other two, and crates.io's limit is 10 MiB COMPRESSED for the
# whole package; the two published archives already bring that to ~4.9 MB, and a
# third leaves no room. It is also not yet publishable on its own terms: it is
# built without src/core/activation.c, which has no Windows arm, so it is
# missing the nine hl_activation_* entry points pkgs/rust/src/ffi.rs declares and
# any link against it from Rust fails on exactly those names. Hence: buildable
# on demand, never committed (pkgs/rust/assets/lib/.gitignore whitelists only
# the two published archives), and absent from --all and from the no-flags
# default so that a routine refresh on a Unix host cannot be blamed for skipping
# it.
#
# Split flow: on the mac, `--darwin --emit /somewhere/libhl-engine.a`; carry the
# file over; on the Linux host, `--linux` then `--darwin --from <file>` then
# `--provenance`.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

: "${BUILD:=build}"
: "${MAC:=mac}"
: "${CMAKE:=cmake}"
: "${NINJA:=ninja}"

do_linux=0
do_darwin=0
do_windows=0
do_provenance=0
darwin_from=
darwin_emit=
allow_unvalidated=0

while [ "$#" -gt 0 ]; do
	case $1 in
	--linux) do_linux=1 ;;
	--darwin) do_darwin=1 ;;
	--windows) do_windows=1 ;;
	--provenance) do_provenance=1 ;;
	# --windows is deliberately NOT here; see the header note.
	--all) do_linux=1 do_darwin=1 do_provenance=1 ;;
	--from)
		shift
		darwin_from=${1:?--from needs a path}
		;;
	--emit)
		shift
		darwin_emit=${1:?--emit needs a path}
		;;
	# Records a darwin archive that this host cannot inspect. The gate in
	# tools/check_crate_archives.sh still validates it wherever it can run.
	--allow-unvalidated-darwin) allow_unvalidated=1 ;;
	-h | --help)
		sed -n '2,30p' "$0"
		exit 0
		;;
	*)
		printf 'refresh-crate-archives: unknown argument: %s\n' "$1" >&2
		exit 2
		;;
	esac
	shift
done

if [ "$do_linux$do_darwin$do_windows$do_provenance" = "0000" ]; then
	do_linux=1
	do_darwin=1
	do_provenance=1
fi

if [ -n "$darwin_from" ] && [ -n "$darwin_emit" ]; then
	printf 'refresh-crate-archives: --from and --emit are mutually exclusive\n' >&2
	exit 2
fi
if [ "$do_darwin" = 0 ] && { [ -n "$darwin_from" ] || [ -n "$darwin_emit" ]; }; then
	printf 'refresh-crate-archives: --from/--emit require --darwin\n' >&2
	exit 2
fi

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

is_darwin() { [ "$(uname -s)" = Darwin ]; }
have_mac() { command -v "$MAC" >/dev/null 2>&1; }

# The host CPU, normalised as CMakeLists.txt normalises HL_HOST_ARCH. --linux
# builds a NATIVE archive, so the host CPU decides both the package directory and
# the asset triple: a guard testing only is_darwin() would let an x86_64 host
# install its archive over the published aarch64 asset.
host_arch=
case "$(uname -m)" in
aarch64 | arm64) host_arch=aarch64 ;;
x86_64 | amd64) host_arch=x86_64 ;;
esac

linux_target=$host_arch-unknown-linux-gnu
linux_asset=pkgs/rust/assets/lib/$linux_target/libhl-engine.a
darwin_asset=pkgs/rust/assets/lib/aarch64-apple-darwin/libhl-engine.a
# What PROVENANCE.md certifies and `cargo publish` ships; never host-derived.
published_linux_asset=pkgs/rust/assets/lib/aarch64-unknown-linux-gnu/libhl-engine.a
windows_asset=pkgs/rust/assets/lib/x86_64-pc-windows-msvc/hl-engine.lib
linux_build=$BUILD/crate-archive-linux
mac_build=$BUILD/crate-archive-macos
windows_build=$BUILD/crate-archive-windows

# Publish through a sibling file, then rename. Most importantly, the Darwin
# copy, inspection, and rename all happen on the Mac: a Linux copy from the
# shared mount can race the producer's libtool write and preserve a truncated
# archive that still has the expected path and provenance.

if [ "$do_linux" = 1 ]; then
	if [ "$(uname -s)" != Linux ] || [ -z "$host_arch" ]; then
		printf 'refresh-crate-archives: --linux builds a NATIVE archive; this host is %s/%s\n' \
			"$(uname -s)" "$(uname -m)" >&2
		printf 'refresh-crate-archives: run it on a Linux host whose CPU is aarch64 or x86_64\n' >&2
		exit 1
	fi
	printf 'refresh-crate-archives: building the %s archive\n' "$linux_target"
	"$CMAKE" -S "$root" -B "$linux_build" -G Ninja -DHL_BUILD_TESTS=OFF
	"$NINJA" -C "$linux_build" hl-engine-activation
	mkdir -p "$(dirname -- "$linux_asset")"
	# package/linux-<arch> is HL_PACKAGE_ARCH_DIR; it encodes the HOST platform.
	install -m 0644 "$linux_build/package/linux-$host_arch/libhl-engine.a" "$linux_asset.tmp"
	tools/validate_crate_archive.sh "$linux_target" "$linux_asset.tmp"
	mv -f "$linux_asset.tmp" "$linux_asset"
	tools/validate_crate_archive.sh "$linux_target" "$linux_asset"
fi

if [ "$do_windows" = 1 ]; then
	case "$(uname -s)" in
	MINGW* | MSYS* | CYGWIN* | Windows_NT) ;;
	*)
		printf 'refresh-crate-archives: --windows needs a Windows host; this one is %s\n' \
			"$(uname -s)" >&2
		printf 'refresh-crate-archives: the archive is MSVC-ABI COFF and needs the VS Build Tools\n' >&2
		exit 1
		;;
	esac
	printf 'refresh-crate-archives: building the x86_64-pc-windows-msvc archive\n'
	# The MSVC-ABI toolchain, NOT cmake/toolchains/x86_64-windows.cmake: rustup's
	# default host triple here is x86_64-pc-windows-msvc, and a mingw-w64 archive
	# cannot be linked into that ABI at all.
	"$CMAKE" -S "$root" -B "$windows_build" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchains/x86_64-windows-msvc.cmake" \
		-DHL_BUILD_TESTS=OFF
	"$NINJA" -C "$windows_build" hl-engine-crate-archive
	mkdir -p "$(dirname -- "$windows_asset")"
	install -m 0644 "$windows_build/package/windows-x86_64/hl-engine.lib" "$windows_asset.tmp"
	tools/validate_crate_archive.sh x86_64-pc-windows-msvc "$windows_asset.tmp"
	mv -f "$windows_asset.tmp" "$windows_asset"
	printf 'refresh-crate-archives: installed %s (local only; not published)\n' "$windows_asset"
fi

if [ "$do_darwin" = 1 ] && [ -z "$darwin_from" ]; then
	printf 'refresh-crate-archives: building the darwin archive\n'
	if is_darwin; then
		"$CMAKE" -S "$root" -B "$mac_build" -G Ninja -DHL_BUILD_TESTS=ON
		"$NINJA" -C "$mac_build" hl-engine-dual
	elif have_mac; then
		"$MAC" zsh -lc '
			root=$1
			build=$2
			cd "$root"
			nix --extra-experimental-features "nix-command flakes" develop --command sh -c '"'"'
				root=$1
				build=$2
				cmake -S "$root" -B "$build" -G Ninja -DHL_BUILD_TESTS=ON
				ninja -C "$build" hl-engine-dual
			'"'"' hl-refresh-inner "$root" "$build"
		' hl-refresh "$root" "$mac_build"
	else
		printf 'refresh-crate-archives: --darwin needs Darwin, the `mac` bridge, or --from\n' >&2
		exit 1
	fi
	built=$mac_build/package/macos-aarch64/libhl-engine.a
	if [ -n "$darwin_emit" ]; then
		if is_darwin; then
			install -m 0644 "$built" "$darwin_emit"
		else
			"$MAC" install -m 0644 "$root/$built" "$darwin_emit"
		fi
		printf 'refresh-crate-archives: emitted %s\n' "$darwin_emit"
	else
		darwin_from=$built
	fi
fi

if [ "$do_darwin" = 1 ] && [ -n "$darwin_from" ]; then
	if is_darwin; then
		install -m 0644 "$darwin_from" "$darwin_asset.tmp"
		tools/validate_crate_archive.sh aarch64-apple-darwin "$darwin_asset.tmp"
		mv -f "$darwin_asset.tmp" "$darwin_asset"
	elif have_mac; then
		"$MAC" install -m 0644 "$darwin_from" "$root/$darwin_asset.tmp"
		"$MAC" tools/validate_crate_archive.sh aarch64-apple-darwin "$darwin_asset.tmp"
		"$MAC" mv -f "$root/$darwin_asset.tmp" "$root/$darwin_asset"
	elif [ "$allow_unvalidated" = 1 ]; then
		install -m 0644 "$darwin_from" "$darwin_asset.tmp"
		mv -f "$darwin_asset.tmp" "$darwin_asset"
		printf 'refresh-crate-archives: WARNING recorded %s unvalidated; a Darwin\n' "$darwin_asset" >&2
		printf 'refresh-crate-archives: WARNING host must still run check-crate-archives\n' >&2
	else
		printf 'refresh-crate-archives: cannot validate the darwin archive on this host\n' >&2
		printf 'refresh-crate-archives: install the `mac` bridge, or pass --allow-unvalidated-darwin\n' >&2
		exit 1
	fi
fi

if [ "$do_provenance" = 1 ]; then
	# The block certifies the PUBLISHED archives, which a non-aarch64 host
	# cannot have produced: it would restate a stale digest against a fresh
	# manifest.
	if [ "$host_arch" != aarch64 ]; then
		printf 'refresh-crate-archives: --provenance certifies the aarch64 archives; this host is %s\n' \
			"$(uname -m)" >&2
		printf 'refresh-crate-archives: run it on the aarch64 host that produced them\n' >&2
		exit 1
	fi
	for asset in "$published_linux_asset" "$darwin_asset"; do
		[ -s "$asset" ] || {
			printf 'refresh-crate-archives: %s is missing; build it first\n' "$asset" >&2
			exit 1
		}
	done

	commit=$(git rev-parse HEAD)
	if ! git diff --quiet -- src include; then
		commit="$commit (with uncommitted changes under src/ or include/)"
	fi
	manifest=$(tools/crate_archive_manifest.sh)
	linux_sha=$(sha256 "$published_linux_asset" | cut -d' ' -f1)
	darwin_sha=$(sha256 "$darwin_asset" | cut -d' ' -f1)
	abi=$(sed -n 's/^#define HL_CONFIG_ABI \([0-9]*\).*/\1/p' include/hl/config.h | head -1)

	provenance=pkgs/rust/assets/PROVENANCE.md
	python3 - "$provenance" "$commit" "$manifest" "$linux_sha" "$darwin_sha" "$abi" <<'PY'
import sys

path, commit, manifest, linux_sha, darwin_sha, abi = sys.argv[1:7]
begin = "<!-- BEGIN GENERATED ARCHIVE PROVENANCE -->"
end = "<!-- END GENERATED ARCHIVE PROVENANCE -->"

block = "\n".join([
    begin,
    "",
    "```",
    f"source-commit: {commit}",
    f"config-abi: {abi}",
    f"source-manifest: {manifest}",
    f"aarch64-unknown-linux-gnu: {linux_sha}",
    f"aarch64-apple-darwin: {darwin_sha}",
    "```",
    "",
    end,
])

text = open(path, encoding="utf-8").read()
if begin not in text or end not in text:
    sys.exit(f"{path}: missing generated provenance markers")
head, _, rest = text.partition(begin)
_, _, tail = rest.partition(end)
open(path, "w", encoding="utf-8").write(head + block + tail)
PY

	printf 'refresh-crate-archives: updated %s\n' "$provenance"
	tools/check_crate_archives.sh
fi
