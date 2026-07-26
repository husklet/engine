#!/usr/bin/env bash
# engine in engine: run a chain `<engine> <engine> [<engine>...] <guest>` and check the
# INNERMOST guest's stdout and exit status. Each engine but the last is the next link's host.
#
#   nested_engine_gate.sh <cross-tree|-> <fix-command> <expected-stdout> <expected-status> -- <link>...
#
# Exit 77 (the gate's SKIP_RETURN_CODE) when a link under <cross-tree> is absent: the
# foreign-host engines come from a second toolchain and a second configure that no default
# build of this tree produces. A link missing ANYWHERE ELSE is a hard failure -- a skip that
# could swallow a broken build of this tree would be worse than no gate at all.
set -uo pipefail

if [ "$#" -lt 6 ]; then
	printf 'usage: %s <cross-tree|-> <fix-command> <expected-stdout> <expected-status> -- <engine>... <guest>\n' "$0" >&2
	exit 2
fi
cross=$1
fix=$2
expected=$3
want=$4
sep=$5
shift 5
if [ "$sep" != "--" ]; then
	printf 'nested-engine: expected `--` before the chain, got `%s`\n' "$sep" >&2
	exit 2
fi
if [ "$#" -lt 3 ]; then
	printf 'nested-engine: a chain needs at least two engines and a guest\n' >&2
	exit 2
fi

if [ "$cross" != "-" ]; then
	for link in "$@"; do
		case $link in
		"$cross"/*) ;;
		*) continue ;;
		esac
		[ -x "$link" ] && continue
		cat >&2 <<EOF
nested-engine: SKIPPED, this gate did NOT run and is NOT green.
nested-engine:   missing: $link
nested-engine: It comes from the foreign-host cross build tree $cross, built with a second
nested-engine: toolchain that no default build of this tree produces. Build it with:
nested-engine:   $fix
EOF
		exit 77
	done
fi
for link in "$@"; do
	if [ ! -x "$link" ]; then
		printf 'nested-engine: %s is missing or not executable; build this tree first\n' "$link" >&2
		exit 1
	fi
done

out=$(mktemp "${TMPDIR:-/tmp}/hl-nested.XXXXXX")
trap 'rm -f "$out"' EXIT HUP INT TERM
"$@" >"$out"
got=$?
if [ "$got" -ne "$want" ]; then
	printf 'nested-engine: %s\nnested-engine: exit %s, expected %s\n' "$*" "$got" "$want" >&2
	exit 1
fi
if ! diff -u "$expected" "$out" >&2; then
	printf 'nested-engine: %s\nnested-engine: stdout differs from %s\n' "$*" "$expected" >&2
	exit 1
fi
printf 'nested-engine: %s -> exit %s, stdout matches %s\n' "$*" "$got" "$expected"
