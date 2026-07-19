#!/usr/bin/env bash
# Execute the complete curated LTP matrix in bounded category batches. Each
# worker owns its logs/results; the final table is assembled deterministically.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${LTP_OUT:-$HERE/out}"
BIN_ROOT="${LTP_BIN_ROOT:-$OUT/bin}"
JOBS="${LTP_JOBS:-8}"
WORK="$OUT/runs"
mkdir -p "$WORK"

mapfile -t categories < <(awk '!/^($|#)/ {print $1}' "$HERE/tests.list" | sort -u)

run_category() {
    local category="$1"
    local dir="$WORK/$category" only
    mkdir -p "$dir"
    only="$(awk -v category="$category" '$1 == category {sub(/^.*\//, "", $3); sub(/\.c$/, "", $3); printf "%s ", $3}' "$HERE/tests.list")"
    LTP_OUT="$dir" \
    LTP_BIN_ROOT="$BIN_ROOT" \
    LTP_ONLY="$only" \
    LTP_CATEGORY="$category" \
    LTP_ARCHES="${LTP_ARCHES:-arm64 x86_64}" \
    LTP_TIMEOUT="${LTP_TIMEOUT:-20}" \
    HL_ENGINE_DIR="${HL_ENGINE_DIR:-}" \
    HL_ENGINE_RUNNER="${HL_ENGINE_RUNNER:-}" \
        "$HERE/run.sh" >"$dir/run.log" 2>&1
}
export -f run_category
export HERE OUT BIN_ROOT WORK
export LTP_ARCHES LTP_TIMEOUT HL_ENGINE_DIR HL_ENGINE_RUNNER

printf '%s\n' "${categories[@]}" | xargs -P "$JOBS" -n 1 bash -c 'run_category "$1"' _ || true

result="$OUT/results.tsv"
printf 'arch\tcategory\ttest\toracle\tdd\tstatus\tdd_exit\n' >"$result"
for category in "${categories[@]}"; do
    if [ -f "$WORK/$category/results.tsv" ]; then
        tail -n +2 "$WORK/$category/results.tsv" >>"$result"
    fi
done

actual=$(($(wc -l < "$result") - 1))
arches=$(wc -w <<<"${LTP_ARCHES:-arm64 x86_64}")
expected=$(($(awk '!/^($|#)/ {n++} END {print n}' "$HERE/tests.list") * arches))
echo "[ltp] complete registrations: $actual/$expected"
awk -F '\t' 'NR > 1 {count[$6]++} END {for (status in count) printf "  %-14s %d\n", status, count[status]}' "$result" | sort
echo "[ltp] full table: $result"
if [ "$actual" -ne "$expected" ]; then
    echo "[ltp] ERROR: one or more workers ended before recording every registration" >&2
    exit 1
fi
