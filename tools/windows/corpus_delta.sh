#!/usr/bin/env bash
# Which CASES changed verdict between two corpus-logs directories.
# Usage: corpus_delta.sh <before-dir> <after-dir>
set -u
BEFORE="${1:-corpus-logs}"
AFTER="${2:-corpus-logs-fixed}"

names () { # suite-log -> sorted failing case names
    grep -hE "stdout differs from|exit mismatch|timed out" "$1" 2>/dev/null |
        sed 's|: stdout differs.*||; s|: exit mismatch.*||; s|: timed out.*||; s|.*/x86_64/||' | sort -u
}

fixed_total=0; broke_total=0
for f in "$BEFORE"/*.log; do
    [ -e "$f" ] || continue
    label="$(basename "$f" .log)"
    g="$AFTER/$label.log"
    [ -e "$g" ] || continue
    # Only compare COMPLETED suites. A log still being written has recorded only
    # the failures found so far, so diffing it reports every not-yet-run case as
    # "fixed" -- which is how a partial signals.log once claimed +19.
    for h in "$f" "$g"; do
        grep -q "active x86_64 cases passed" "$h" || { echo "== $label: SKIPPED (log incomplete: $h)"; continue 2; }
    done
    fixed=$(comm -23 <(names "$f") <(names "$g"))
    broke=$(comm -13 <(names "$f") <(names "$g"))
    nf=$(printf '%s' "$fixed" | grep -c . )
    nb=$(printf '%s' "$broke" | grep -c . )
    [ "$nf" -eq 0 ] && [ "$nb" -eq 0 ] && continue
    echo "== $label: +$nf fixed, -$nb newly failing"
    [ "$nf" -gt 0 ] && echo "$fixed" | sed 's/^/   FIXED  /'
    [ "$nb" -gt 0 ] && echo "$broke" | sed 's/^/   BROKE  /'
    fixed_total=$((fixed_total+nf)); broke_total=$((broke_total+nb))
done
echo
echo "TOTAL fixed=$fixed_total newly-failing=$broke_total"
