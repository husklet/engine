#!/usr/bin/env bash
# Regenerate BOTH prebuilt crate archives from the current tree and rewrite
# pkgs/rust/assets/PROVENANCE.md.
#
# The crate at pkgs/rust/ never compiles src/: build.rs links the committed
# archives and `cargo publish` ships those bytes. Run this whenever a C source
# or header changes, and commit the result together with the source change.
#
# Requirements:
#   * an aarch64 Linux host for the linux-gnu archive;
#   * an Apple silicon mac for the darwin archive. On a Linux workstation the
#     mac is reached through the `mac` command bridge (MAC=mac, the default off
#     Darwin) over the shared /Users/x/dd checkout. If `mac` is unavailable the
#     compile fails immediately with "mac: command not found" -- there is no
#     silent fallback and no way to refresh only half the pair.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

: "${BUILD:=build}"
: "${MAC:=mac}"
: "${CMAKE:=cmake}"
: "${NINJA:=ninja}"

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

if [ "$(uname -s)" != "Darwin" ] && ! command -v "$MAC" >/dev/null 2>&1; then
	printf 'refresh-crate-archives: the `mac` bridge is required to build the darwin archive\n' >&2
	printf 'refresh-crate-archives: run this from a host that can reach the mac, or set MAC=<command>\n' >&2
	exit 1
fi

linux_build=$BUILD/crate-archive-linux
mac_build=$BUILD/crate-archive-macos

printf 'refresh-crate-archives: building the linux-gnu archive\n'
"$CMAKE" -S "$root" -B "$linux_build" -G Ninja -DHL_BUILD_TESTS=OFF
"$NINJA" -C "$linux_build" hl-engine-activation

printf 'refresh-crate-archives: building the darwin archive (via the mac host)\n'
if [ "$(uname -s)" = "Darwin" ]; then
	"$CMAKE" -S "$root" -B "$mac_build" -G Ninja -DHL_BUILD_TESTS=ON
	"$NINJA" -C "$mac_build" hl-engine-dual
else
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
fi

linux_asset=pkgs/rust/assets/lib/aarch64-unknown-linux-gnu/libhl-engine.a
darwin_asset=pkgs/rust/assets/lib/aarch64-apple-darwin/libhl-engine.a
linux_staging=$linux_asset.tmp
darwin_staging=$darwin_asset.tmp

# Publish and validate through a sibling file, then rename. Most importantly,
# the Darwin copy, inspection, and rename all happen on the Mac: a Linux copy
# from the shared mount can race the producer's libtool write and preserve a
# truncated archive that still has the expected path and provenance.
install -m 0644 "$linux_build/package/linux-aarch64/libhl-engine.a" "$linux_staging"
tools/validate_crate_archive.sh aarch64-unknown-linux-gnu "$linux_staging"
mv -f "$linux_staging" "$linux_asset"

if [ "$(uname -s)" = "Darwin" ]; then
	install -m 0644 "$mac_build/package/macos-aarch64/libhl-engine.a" "$darwin_staging"
	tools/validate_crate_archive.sh aarch64-apple-darwin "$darwin_staging"
	mv -f "$darwin_staging" "$darwin_asset"
else
	"$MAC" install -m 0644 "$mac_build/package/macos-aarch64/libhl-engine.a" "$darwin_staging"
	"$MAC" tools/validate_crate_archive.sh aarch64-apple-darwin "$darwin_staging"
	"$MAC" mv -f "$darwin_staging" "$darwin_asset"
fi

# Revalidate the exact final paths that cargo packages and build.rs links.
tools/validate_crate_archive.sh aarch64-unknown-linux-gnu "$linux_asset"
tools/validate_crate_archive.sh aarch64-apple-darwin "$darwin_asset"

commit=$(git rev-parse HEAD)
if ! git diff --quiet -- src include; then
	commit="$commit (with uncommitted changes under src/ or include/)"
fi
manifest=$(tools/crate_archive_manifest.sh)
linux_sha=$(sha256 "$linux_asset" | cut -d' ' -f1)
if [ "$(uname -s)" = "Darwin" ]; then
	darwin_sha=$(shasum -a 256 "$darwin_asset" | cut -d' ' -f1)
else
	darwin_sha=$("$MAC" shasum -a 256 "$darwin_asset" | cut -d' ' -f1)
fi
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
