#!/usr/bin/env bash
# Assert that every CI lane declared in cmake/CiLanes.cmake selects at least one
# test on this host. Registry-only by design: it catches the dangerous CTest
# behaviour where `ctest -L missing` exits 0, which would turn a converted
# workflow step silently green.
set -euo pipefail

if [ "$#" -ne 4 ]; then
	printf 'usage: %s <ctest> <build-dir> <os> <cpu>\n' "$0" >&2
	exit 2
fi

ctest=$1
build=$2
os=$3
cpu=$4
# The host is the (OS, CPU) pair -- two Linux host CPUs register different tests.
# HL_CI_HOSTS declares the token.
host=$os-$cpu
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

# An unknown host must be rejected rather than silently checking nothing.
if ! lanes_in HL_CI_HOSTS | grep -Fqx -- "$host"; then
	printf 'lane-parity: %s is not declared in HL_CI_HOSTS (%s)\n' \
		"$host" "$lanes_file" >&2
	exit 2
fi

case "$os" in
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
Windows)
	# Two of these three are empty today and that is not a defect: a Windows
	# host shards no compat lane (no guest corpus can be built or supplied
	# there yet) and registers no guest-backed suite to reserve. The union is
	# what must be non-empty, and the check below enforces exactly that -- so
	# the first Windows declaration has to carry a real, green lane in
	# HL_CI_DIRECT_WINDOWS rather than an empty placeholder.
	labels=$(lanes_in HL_CI_SHARDED_WINDOWS
		lanes_in HL_CI_DIRECT_WINDOWS
		lanes_in HL_CI_REGISTRY_WINDOWS)
	;;
*)
	printf 'lane-parity: unsupported host OS %s\n' "$os" >&2
	exit 2
	;;
esac

# The lists above are per host OS. Drop the lanes HL_CI_HOST_CPU_ONLY reserves for
# a DIFFERENT CPU of this OS, and reject an entry naming an undeclared host: a
# stale exemption stops checking a lane that applies here.
cpu_only=$(lanes_in HL_CI_HOST_CPU_ONLY)
for entry in $cpu_only; do
	if ! lanes_in HL_CI_HOSTS | grep -Fqx -- "${entry%%:*}"; then
		printf 'lane-parity: %s names no declared host; use <host-token>:<lane>\n' \
			"$entry" >&2
		exit 2
	fi
done
if [ -n "$cpu_only" ]; then
	kept=
	for label in $labels; do
		# A lane may be reserved to SEVERAL tokens: test "named at all,
		# but not for this host", not "some entry differs".
		reserved=0
		mine=0
		for entry in $cpu_only; do
			[ "${entry#*:}" = "$label" ] || continue
			reserved=1
			[ "${entry%%:*}" = "$host" ] && mine=1
		done
		if [ "$reserved" -eq 1 ] && [ "$mine" -eq 0 ]; then
			continue
		fi
		kept="$kept $label"
	done
	labels=$kept
fi

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
