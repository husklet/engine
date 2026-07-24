#!/usr/bin/env bash
# Fail if the prebuilt archives committed under pkgs/rust/assets/lib/ no longer
# correspond to the C sources in this tree.
#
# `cargo publish` ships those archives verbatim; the crate never compiles src/.
# A stale archive still links and still launches guests, so neither `cargo
# build` nor pkgs/rust/tests/packaged_archive.rs notices that it is missing
# every engine change made since it was built. Releases 0.1.17, 0.1.18 and
# 0.1.26 shipped an archive 478 commits behind the headers for that reason.
#
# This compares two things against the generated block in
# pkgs/rust/assets/PROVENANCE.md:
#   1. the source manifest hash (see tools/crate_archive_manifest.sh) -- catches
#      "a .c file changed and nobody regenerated the archives";
#   2. the SHA-256 of each committed archive -- catches an archive edited or
#      replaced without updating its recorded provenance.
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

provenance=pkgs/rust/assets/PROVENANCE.md

sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

field() {
	sed -n "s/^$1: //p" "$provenance" | head -1
}

fail() {
	printf '\n'
	printf 'ERROR: %s\n' "$1"
	cat <<'EOF'

The crate at pkgs/rust/ ships PREBUILT static archives; it does not compile
src/. They must be regenerated whenever any C source or header changes:

    make refresh-crate-archives      # needs an aarch64 Linux host and the mac

See DOCS.md ("Prebuilt crate archives") for the macOS half, which must be
built on Apple silicon (from this checkout: `mac make ...`, /Users/x/dd is
shared with the mac).
EOF
	exit 1
}

recorded_manifest=$(field 'source-manifest')
[ -n "$recorded_manifest" ] || fail "$provenance records no source-manifest hash"

actual_manifest=$(tools/crate_archive_manifest.sh)

if [ "$recorded_manifest" != "$actual_manifest" ]; then
	printf 'source manifest recorded: %s\n' "$recorded_manifest"
	printf 'source manifest actual:   %s\n' "$actual_manifest"
	fail 'C sources changed since the committed crate archives were built.'
fi

status=0
for target in aarch64-unknown-linux-gnu aarch64-apple-darwin; do
	archive="pkgs/rust/assets/lib/$target/libhl-engine.a"
	[ -f "$archive" ] || fail "missing archive $archive"
	recorded=$(field "$target")
	actual=$(sha256 "$archive" | cut -d' ' -f1)
	if [ "$recorded" != "$actual" ]; then
		printf '%s recorded: %s\n' "$target" "$recorded"
		printf '%s actual:   %s\n' "$target" "$actual"
		status=1
	fi
done
[ "$status" -eq 0 ] || fail 'a committed archive does not match its recorded SHA-256.'

printf 'crate archives are current (source manifest %s)\n' "$actual_manifest"
