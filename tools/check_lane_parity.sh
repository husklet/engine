#!/usr/bin/env bash
# Assert that every CI lane declared in cmake/CiLanes.cmake selects at least one
# test on this host. Registry-only by design: it catches the dangerous CTest
# behaviour where `ctest -L missing` exits 0, which would turn a converted
# workflow step silently green.
set -euo pipefail

if [ "$#" -ne 3 ]; then
	printf 'usage: %s <ctest> <build-dir> <Linux|Darwin>\n' "$0" >&2
	exit 2
fi

ctest=$1
build=$2
host=$3
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lanes_file=$root/cmake/CiLanes.cmake

# One `set(NAME` .. `)` block, one lane per line.
lanes_in() {
	awk -v name="$1" '
		$0 ~ "^set\\(" name "$" { on = 1; next }
		on && /^\)/ { exit }
		on { gsub(/[ \t]/, ""); if ($0 != "" && $0 !~ /^#/) print }
	' "$lanes_file"
}

case "$host" in
Linux)
	labels=$(lanes_in HL_CI_SHARDED_LINUX
		lanes_in HL_CI_DIRECT_LINUX
		lanes_in HL_CI_REGISTRY_LINUX)
	;;
Darwin)
	labels=$(lanes_in HL_CI_SHARDED_DARWIN
		lanes_in HL_CI_DIRECT_DARWIN
		lanes_in HL_CI_REGISTRY_DARWIN)
	;;
*)
	printf 'lane-parity: unsupported host %s\n' "$host" >&2
	exit 2
	;;
esac

if [ -z "$labels" ]; then
	printf 'lane-parity: parsed no lanes from %s\n' "$lanes_file" >&2
	exit 1
fi

status=0
count=0
for label in $labels; do
	count=$((count + 1))
	output=$("$ctest" --test-dir "$build" -N -L "^${label}$" 2>&1)
	n=$(printf '%s\n' "$output" |
		sed -n 's/^[[:space:]]*Total Tests: \([0-9][0-9]*\)$/\1/p' |
		tail -1)
	if [ -z "$n" ]; then
		printf 'lane-parity: could not enumerate label %s\n%s\n' \
			"$label" "$output" >&2
		status=1
	elif [ "$n" -eq 0 ]; then
		printf 'lane-parity: label %s selects zero tests on %s\n' \
			"$label" "$host" >&2
		status=1
	fi
done

[ "$status" -eq 0 ] || exit "$status"
printf 'lane-parity: %d declared lanes are non-empty on %s\n' "$count" "$host"
