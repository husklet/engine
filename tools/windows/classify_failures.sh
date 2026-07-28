#!/usr/bin/env bash
# For one suite log, print each failing case with the FIRST line where the
# engine's stdout diverges from the golden. That first divergence is almost
# always the name of the missing capability, so the output groups by cause.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${BUILD:-$ROOT/build-corpus}"
ENGINE="${ENGINE:-$BUILD/windows-production/hl-engine-windows-x86_64.exe}"
LOG="$1"

grep -E "stdout differs from|exit mismatch|timed out" "$LOG" | while IFS= read -r line; do
    guest="${line%%: *}"
    name="${guest##*/x86_64/}"
    case "$line" in
        *"stdout differs from"*)
            golden="${line##*stdout differs from }"
            got=$(timeout 20 "$ENGINE" "$guest" 2>/dev/null)
            exp=$(cat "$golden" 2>/dev/null)
            # first differing line
            d=$(diff <(printf '%s\n' "$exp") <(printf '%s\n' "$got") 2>/dev/null | grep '^[<>]' | head -2 | tr '\n' '|')
            printf 'DIFF   %-34s %s\n' "$name" "$d"
            ;;
        *"exit mismatch"*)
            printf 'EXIT   %-34s %s\n' "$name" "${line#*: }"
            ;;
        *"timed out"*)
            printf 'HANG   %-34s\n' "$name"
            ;;
    esac
done
