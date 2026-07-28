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
# This compares three things against the generated block in
# pkgs/rust/assets/PROVENANCE.md:
#   1. the source manifest hash (see tools/crate_archive_manifest.sh) -- catches
#      "a .c file changed and nobody regenerated the archives";
#   2. the SHA-256 of each committed archive -- catches an archive edited or
#      replaced without updating its recorded provenance;
#   3. the build stamp carried by every object inside each archive (see
#      tools/gen_archive_stamp.sh) -- catches an archive that does not come from
#      the manifest it is filed under. 1 and 2 are both satisfied by an archive
#      built from other sources: a refresh in an incremental build directory
#      that failed to recompile a changed file produced exactly that, and the
#      resulting archive passed this gate while failing the crate's terminal
#      test. The stamp is compiled in, so it cannot be forged by rewriting
#      PROVENANCE.md.
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

remediation() {
	printf '\n'
	cat <<'EOF'
The crate at pkgs/rust/ ships PREBUILT static archives; it does not compile
src/. They must be regenerated whenever any C source or header changes:

    cmake --build build --target refresh-crate-archives

The macOS archive must be built on Apple silicon. Both host archives must be
refreshed before publishing the crate.
EOF
}

# --skip-freshness: run every integrity check but report a stale source manifest
# instead of failing. The archives are a committed build artifact, so any src/
# commit invalidates the digest and only a dual-host workstation can regenerate
# it -- making this unsatisfiable on the push lanes, where it was red on every
# commit. Freshness is enforced where it decides what ships: publish.yml, which
# runs this script without the flag. Corruption, a replaced archive and a failed
# link still fail everywhere.
freshness=enforce
for argument in "$@"; do
	case $argument in
	--skip-freshness) freshness=report ;;
	*)
		printf 'usage: %s [--skip-freshness]\n' "$0" >&2
		exit 2
		;;
	esac
done

status=0
report_error() {
	printf '\nERROR: %s\n' "$1" >&2
	status=1
}

recorded_manifest=$(field 'source-manifest')
[ -n "$recorded_manifest" ] || report_error "$provenance records no source-manifest hash"

actual_manifest=$(tools/crate_archive_manifest.sh)

if [ "$recorded_manifest" != "$actual_manifest" ]; then
	printf 'source manifest recorded: %s\n' "$recorded_manifest"
	printf 'source manifest actual:   %s\n' "$actual_manifest"
	if [ "$freshness" = enforce ]; then
		report_error 'C sources changed since the committed crate archives were built.'
	else
		printf '::notice title=Crate archives are stale::regenerate before releasing (source manifest %s, recorded %s)\n' \
			"$actual_manifest" "$recorded_manifest"
	fi
fi

for target in aarch64-unknown-linux-gnu aarch64-apple-darwin; do
	archive="pkgs/rust/assets/lib/$target/libhl-engine.a"
	if [ ! -f "$archive" ]; then
		report_error "missing archive $archive"
		continue
	fi
	recorded=$(field "$target")
	actual=$(sha256 "$archive" | cut -d' ' -f1)
	if [ -z "$recorded" ]; then
		report_error "$provenance records no SHA-256 for $target"
		continue
	fi
	if [ "$recorded" != "$actual" ]; then
		printf '%s recorded: %s\n' "$target" "$recorded"
		printf '%s actual:   %s\n' "$target" "$actual"
		report_error "committed $target archive does not match its recorded SHA-256."
	fi
done

# Every object in a shipped archive force-includes the generated stamp header,
# so the archive states which source manifest it was compiled against. This is
# an INTEGRITY check, not a freshness one: it compares archive bytes against the
# recorded manifest, never against the working tree, so it holds on every push
# lane exactly as it holds at release time. It runs on both hosts because it
# only reads bytes -- no target-specific tooling.
check_stamp() {
	local target=$1 archive=$2 stamps total members unique
	stamps=$(LC_ALL=C grep -oa 'HL-ARCHIVE-SOURCE-MANIFEST:[0-9a-f]\{64\}' "$archive" |
		sed 's/^[^:]*://' | LC_ALL=C sort || true)
	total=$(printf '%s' "$stamps" | grep -c . || true)
	if [ "$total" -eq 0 ]; then
		report_error "$target archive carries no build stamp; it was not produced by this build system."
		return
	fi
	unique=$(printf '%s\n' "$stamps" | uniq)
	if [ "$(printf '%s\n' "$unique" | grep -c .)" -ne 1 ]; then
		printf '%s stamps found:\n%s\n' "$target" "$unique"
		report_error "$target archive mixes objects built from different sources (stale objects in the build directory)."
		return
	fi
	if [ "$unique" != "$recorded_manifest" ]; then
		printf '%s stamped manifest: %s\n' "$target" "$unique"
		printf '%s recorded manifest: %s\n' "$target" "$recorded_manifest"
		report_error "$target archive was built from sources other than the manifest it is recorded under."
		return
	fi
	# An unstamped member would carry no claim at all, so require one per member.
	# BSD ar lists the Mach-O symbol table as a member; GNU ar hides it.
	members=$(ar -t "$archive" | grep -v '^__\.SYMDEF' | grep -c . || true)
	if [ "$total" -ne "$members" ]; then
		report_error "$target archive has $members members but $total build stamps; some object bypassed the stamp."
	fi
}

for target in aarch64-unknown-linux-gnu aarch64-apple-darwin; do
	archive="pkgs/rust/assets/lib/$target/libhl-engine.a"
	if [ -f "$archive" ]; then
		check_stamp "$target" "$archive"
	fi
done

for target in aarch64-unknown-linux-gnu aarch64-apple-darwin; do
	archive="pkgs/rust/assets/lib/$target/libhl-engine.a"
	if [ -f "$archive" ] &&
		! tools/validate_crate_archive.sh "$target" "$archive"; then
		report_error "$target archive failed structural/link validation."
	fi
done

if [ "$status" -ne 0 ]; then
	remediation
	exit 1
fi

if [ "$recorded_manifest" != "$actual_manifest" ]; then
	printf 'crate archives are intact but STALE; the release path enforces freshness\n'
else
	printf 'crate archives are current (source manifest %s)\n' "$actual_manifest"
fi
