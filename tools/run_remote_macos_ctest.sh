#!/usr/bin/env bash
# Configure, narrowly build, and run a Darwin CTest lane through OrbStack's
# `mac` transport. The checkout lives on the shared /Users volume.
set -euo pipefail

if [ "$#" -ne 3 ]; then
	printf 'usage: %s <source-dir> <build-dir> <ctest-label>\n' "$0" >&2
	exit 2
fi

source_dir=$1
build_dir=$2
label=$3
: "${MAC:=mac}"

if ! command -v "$MAC" >/dev/null 2>&1; then
	printf 'remote-macos-ctest: missing transport command %s\n' "$MAC" >&2
	exit 1
fi

"$MAC" zsh -lc '
	source_dir=$1
	build_dir=$2
	label=$3
	cd "$source_dir"
	nix --extra-experimental-features "nix-command flakes" develop --command sh -c '"'"'
		build_dir=$1
		label=$2
		cmake -G Ninja -B "$build_dir" -DHL_BUILD_TESTS=ON
		ninja -C "$build_dir" perf-macos-build
		ctest --test-dir "$build_dir" -L "^${label}$" \
			--no-tests=error --output-on-failure
	'"'"' hl-remote-inner "$build_dir" "$label"
' hl-remote "$source_dir" "$build_dir" "$label"
