#!/usr/bin/env bash
# Assert that every historical Makefile CI lane has a non-empty CTest label on
# the current host. This is intentionally registry-based: it catches the
# dangerous CTest behaviour where `ctest -L missing` exits successfully.
set -euo pipefail

if [ "$#" -ne 3 ]; then
	printf 'usage: %s <ctest> <build-dir> <Linux|Darwin>\n' "$0" >&2
	exit 2
fi

ctest=$1
build=$2
host=$3

both=(
	unit
	compat-abi compat-completeness compat-filesystem compat-ipc
	compat-isolation compat-libc compat-memory compat-network compat-posix
	compat-process compat-procfs compat-signals compat-syscall
	compat-syscall-edges compat-threads compat-time
	compat-native package embedding
)
linux=(
	production production-full-aarch64 production-full-x86_64
	production-config lifecycle checkpoint checkpoint-io dynamic-e2e
	integration e2e-oracle isa-fuzz perf-linux perf-native
)
darwin=(
	macos e2e-mac perf-macos compat-abi-corpus compat-core-abi
	compat-core-syscall compat-core-regress compat-core-workload
	compat-isa-x86-64 compat-isa-aarch64 compat-soak compat-extended
	compat-direct
)

case "$host" in
Linux)
	labels=("${both[@]}" "${linux[@]}")
	;;
Darwin)
	labels=("${both[@]}" "${darwin[@]}")
	;;
*)
	printf 'lane-parity: unsupported host %s\n' "$host" >&2
	exit 2
	;;
esac

status=0
for label in "${labels[@]}"; do
	output=$("$ctest" --test-dir "$build" -N -L "^${label}$" 2>&1)
	count=$(printf '%s\n' "$output" |
		sed -n 's/^[[:space:]]*Total Tests: \([0-9][0-9]*\)$/\1/p' |
		tail -1)
	if [ -z "$count" ]; then
		printf 'lane-parity: could not enumerate label %s\n%s\n' \
			"$label" "$output" >&2
		status=1
	elif [ "$count" -eq 0 ]; then
		printf 'lane-parity: label %s selects zero tests on %s\n' \
			"$label" "$host" >&2
		status=1
	fi
done

if [ "$status" -ne 0 ]; then
	exit "$status"
fi
printf 'lane-parity: %d required labels are non-empty on %s\n' \
	"${#labels[@]}" "$host"
