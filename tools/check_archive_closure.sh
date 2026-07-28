#!/usr/bin/env bash
# Assert that no installed archive references an engine symbol nothing in the
# shipped product defines.
#
# A static archive keeps its dangling references quiet: the linker pulls members
# on demand, so an object full of unresolvable calls costs nothing until some
# unrelated symbol in the SAME object is referenced. That is how nine ARM64-only
# lowering objects sat in libhl-translator.a on an x86_64 host, and why the first
# link that force-loaded the archive (the packaged dual backend) failed with ~90
# undefined symbols instead of at the compile that created them.
#
# The check has two halves, and neither is an allowlist:
#   providers  -- symbols defined by the installed archives, and nothing else.
#                 There is no "but a build-tree binary defines it" escape: the
#                 published set has to close over itself.
#   toolchain  -- everything else is offered to the real linker against the
#                 system libraries only. Whatever it still cannot resolve is an
#                 engine symbol with no definition anywhere, and it is named.
#
# usage: check_archive_closure.sh <nm> <cc> <archive>...
set -euo pipefail

if [ "$#" -lt 3 ]; then
	printf 'usage: %s <nm> <cc> <archive>...\n' "$0" >&2
	exit 2
fi

nm=$1
cc=$2
shift 2

archives=("$@")

if [ "${#archives[@]}" -eq 0 ]; then
	printf 'archive-closure: no archives named\n' >&2
	exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# `nm --defined-only` prints "<value> <type> <name>"; undefined prints "U <name>"
# with no value. Mapping-symbol noise ($d/$x on aarch64) is local and never
# undefined, so it cannot reach the candidate set.
defined_symbols() { "$nm" --defined-only "$1" 2>/dev/null | awk 'NF == 3 { print $3 }'; }
undefined_symbols() { "$nm" --undefined-only "$1" 2>/dev/null | awk '$1 == "U" { print $2 }'; }

for artifact in "${archives[@]}"; do
	if [ ! -f "$artifact" ]; then
		printf 'archive-closure: %s does not exist\n' "$artifact" >&2
		exit 2
	fi
	defined_symbols "$artifact"
done | sort -u >"$work/provided"

for archive in "${archives[@]}"; do
	undefined_symbols "$archive" | sed "s|\$| ${archive}|"
done | sort -u >"$work/wanted"

awk '{ print $1 }' "$work/wanted" | sort -u >"$work/wanted-names"
comm -23 "$work/wanted-names" "$work/provided" >"$work/candidates"

if [ ! -s "$work/candidates" ]; then
	printf 'archive-closure: %d archives, every reference defined in the installed set\n' \
		"${#archives[@]}"
	exit 0
fi

# Force a reference to each candidate and let the linker rule on it. Declaring a
# data object as a function is fine here: only the name reaches the linker.
{
	index=0
	while read -r symbol; do
		printf 'extern void %s(void);\n' "$symbol"
		printf 'void *hl_probe_%d = (void *)&%s;\n' "$index" "$symbol"
		index=$((index + 1))
	done <"$work/candidates"
	printf 'int main(void) { return 0; }\n'
} >"$work/probe.c"

if "$cc" -o "$work/probe" "$work/probe.c" -pthread -lm -ldl -latomic >"$work/link.log" 2>&1; then
	printf 'archive-closure: %d archives, every reference resolved by the installed set or the toolchain\n' \
		"${#archives[@]}"
	exit 0
fi

sed -n "s/.*undefined reference to \`\([^']*\)'.*/\1/p" "$work/link.log" |
	sort -u >"$work/unresolved"

if [ ! -s "$work/unresolved" ]; then
	printf 'archive-closure: the probe link failed for a reason other than an undefined symbol:\n' >&2
	cat "$work/link.log" >&2
	exit 2
fi

printf 'archive-closure: %d engine symbols are referenced by an installed archive and defined nowhere:\n' \
	"$(wc -l <"$work/unresolved")" >&2
while read -r symbol; do
	printf '  %s\n' "$symbol" >&2
	grep -E "^${symbol} " "$work/wanted" | awk '{ printf "      wanted by %s\n", $2 }' >&2
done <"$work/unresolved"
exit 1
