#!/usr/bin/env bash
# Print the source manifest hash for the prebuilt crate archives.
#
# The crate at pkgs/rust/ does not compile src/: build.rs links the static
# archives committed under pkgs/rust/assets/lib/, and `cargo publish` ships
# those exact bytes. The manifest hash is a content digest of every C source
# and header that feeds those archives, so a changed .c file changes the hash
# and `tools/check_crate_archives.sh` can tell that the committed archives no
# longer correspond to the tree.
#
# Byte-comparing a rebuilt archive would be the stronger check, but the build
# is not reproducible across checkouts: the compiler records the absolute
# source directory (DW_AT_comp_dir) in every object, so two clones of the same
# commit at different paths produce different bytes. A content digest of the
# inputs has no such dependence.
#
# The file list comes from the CMake definition used by the production unity
# object dependencies, so it cannot drift from what the archive rules consume.
# It is a deliberate over-approximation: a macOS-only source change also
# invalidates the Linux archive. Both archives are regenerated together, so
# that costs nothing and never under-reports.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

# macOS ships `shasum`, Linux ships `sha256sum`; both print "<hash>  <path>".
sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

source_list=$(mktemp "${TMPDIR:-/tmp}/hl-archive-sources.XXXXXX")
trap 'rm -f "$source_list"' EXIT
cmake -DHL_SOURCE_DIR="$root" -DHL_OUTPUT="$source_list" \
	-P tools/export_archive_sources.cmake
if [ ! -s "$source_list" ]; then
	printf 'crate-archive-manifest: CMake listed no archive sources\n' >&2
	exit 1
fi

# LC_ALL=C keeps the ordering locale-independent; the per-file digests make the
# hash sensitive to content, and the paths make it sensitive to files being
# added, removed or renamed.
#
# The trailing carriage return is stripped because CMake's file(WRITE) uses the
# PLATFORM newline, so the list arrives CRLF-terminated on a Windows host and
# every path would otherwise carry a literal CR -- sha256sum then reports "no
# such file" for every entry. Stripping it also keeps the digest identical on
# all three hosts, which matters more than the failure: a manifest that differed
# by host would make every cross-host freshness check disagree.
files=()
while read -r file; do
	file=${file%$'\r'}
	[ -n "$file" ] || continue
	files+=("$file")
done < <(LC_ALL=C sort -u "$source_list")

sha256 "${files[@]}" | sha256 | cut -d' ' -f1
