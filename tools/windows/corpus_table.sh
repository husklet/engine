#!/usr/bin/env bash
# Tabulate one corpus-logs/ directory into per-suite pass/fail plus a total.
set -u
DIR="${1:-corpus-logs}"
printf '%-16s %7s %7s %7s %7s\n' suite passed active failed skipped
printf '%-16s %7s %7s %7s %7s\n' ---------------- ------- ------- ------- -------
tp=0; ta=0; tf=0; ts=0
for f in "$DIR"/*.log; do
    [ -e "$f" ] || continue
    label="$(basename "$f" .log)"
    line=$(grep -h "active x86_64 cases passed" "$f" | tail -1)
    [ -n "$line" ] || { printf '%-16s %7s\n' "$label" "NO-SUMMARY"; continue; }
    p=$(echo "$line" | sed -n 's/.*: \([0-9]*\) of \([0-9]*\) active.*/\1/p')
    a=$(echo "$line" | sed -n 's/.*: \([0-9]*\) of \([0-9]*\) active.*/\2/p')
    fl=$(echo "$line" | sed -n 's/.*, \([0-9]*\) FAILED.*/\1/p')
    sk=$(echo "$line" | sed -n 's/.*FAILED; \([0-9]*\) skipped.*/\1/p')
    printf '%-16s %7s %7s %7s %7s\n' "$label" "$p" "$a" "$fl" "$sk"
    tp=$((tp+p)); ta=$((ta+a)); tf=$((tf+fl)); ts=$((ts+sk))
done
printf '%-16s %7s %7s %7s %7s\n' ---------------- ------- ------- ------- -------
printf '%-16s %7s %7s %7s %7s\n' TOTAL "$tp" "$ta" "$tf" "$ts"
[ "$ta" -gt 0 ] && printf 'pass rate: %s%%\n' "$(awk -v p="$tp" -v a="$ta" 'BEGIN{printf "%.1f", 100*p/a}')"
