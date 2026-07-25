#!/usr/bin/env bash
# Run a production CLI/config launch from a guest-visible /tmp workspace.
set -euo pipefail

if [ "$#" -ne 5 ]; then
	printf 'usage: %s <cli|config> <runner> <engine> <guest> <expected-exit>\n' "$0" >&2
	exit 2
fi

mode=$1
runner=$2
engine=$3
guest=$4
expected=$5
workspace=$(mktemp -d "${TMPDIR:-/tmp}/hl-direct-launch.XXXXXX")
trap 'rm -rf "$workspace"' EXIT HUP INT TERM

guest_copy=$workspace/guest
cp "$guest" "$guest_copy"
chmod 0700 "$guest_copy"
cd "$workspace"

case "$mode" in
cli)
	"$runner" env "$engine" "$guest_copy" "$expected"
	;;
config)
	"$runner" env "$engine" "$guest_copy" "$expected"
	;;
*)
	printf 'direct-launch: unsupported mode %s\n' "$mode" >&2
	exit 2
	;;
esac
