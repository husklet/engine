#!/usr/bin/env bash
# Regenerate BOTH prebuilt crate archives from the current tree and rewrite
# pkgs/rust/assets/PROVENANCE.md. Invoked as `make refresh-crate-archives`.
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

: "${MAKE:=make}"
: "${BUILD:=build}"

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

if [ "$(uname -s)" != "Darwin" ] && ! command -v mac >/dev/null 2>&1; then
	printf 'refresh-crate-archives: the `mac` bridge is required to build the darwin archive\n' >&2
	printf 'refresh-crate-archives: run this from a host that can reach the mac, or set MAC=<command>\n' >&2
	exit 1
fi

jobs=$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)

printf 'refresh-crate-archives: building the linux-gnu archive\n'
"$MAKE" -j"$jobs" BUILD="$BUILD" package-embedded-linux

printf 'refresh-crate-archives: building the darwin archive (via the mac host)\n'
"$MAKE" -j"$jobs" BUILD="$BUILD" package-embedded-macos

install -m 0644 "$BUILD/package/linux-aarch64/libhl-engine.a" \
	pkgs/rust/assets/lib/aarch64-unknown-linux-gnu/libhl-engine.a
install -m 0644 "$BUILD/package/macos-aarch64/libhl-engine.a" \
	pkgs/rust/assets/lib/aarch64-apple-darwin/libhl-engine.a

commit=$(git rev-parse HEAD)
if ! git diff --quiet -- src include; then
	commit="$commit (with uncommitted changes under src/ or include/)"
fi
manifest=$(tools/crate_archive_manifest.sh)
linux_sha=$(sha256 pkgs/rust/assets/lib/aarch64-unknown-linux-gnu/libhl-engine.a | cut -d' ' -f1)
darwin_sha=$(sha256 pkgs/rust/assets/lib/aarch64-apple-darwin/libhl-engine.a | cut -d' ' -f1)
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
