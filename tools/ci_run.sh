#!/usr/bin/env bash
# Run one CI gate, bound it, and on failure emit a ::error annotation naming the
# cause. Without this a step reports only "Process completed with exit code N":
# the job-log endpoint requires admin rights, so the failing test is invisible
# in the public check-run data and unreachable from a notification.
#
# usage: tools/ci_run.sh <bound-seconds|0> <title> -- <command> [args...]
set -uo pipefail

bound=$1
title=$2
shift 2
[ "${1:-}" != -- ] || shift

log=$(mktemp "${TMPDIR:-/tmp}/hl-ci-gate.XXXXXX")
trap 'rm -f "$log"' EXIT

# Output to a FILE, never a pipe: these gates spawn guests that outlive their
# parent and inherit stdout, so a pipe reader can wait for an EOF that never
# comes. A bound of 0 means "rely on the step timeout".
status=0
if [ "$bound" -gt 0 ]; then
	"$@" >"$log" 2>&1 </dev/null &
	pid=$! waited=0
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$waited" -ge "$bound" ]; then
			kill -TERM "$pid" 2>/dev/null
			sleep 10
			kill -KILL "$pid" 2>/dev/null
			wait "$pid" 2>/dev/null
			status=124
			break
		fi
		sleep 5
		waited=$((waited + 5))
	done
	[ "$status" -eq 124 ] || wait "$pid" || status=$?
else
	"$@" >"$log" 2>&1 </dev/null || status=$?
fi

tail -n 2000 "$log"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
	tail -c 200000 "$log" >>"$GITHUB_STEP_SUMMARY" || true
fi

[ "$status" -ne 0 ] || exit 0

# Prefer the lines that name a failing test or gate; fall back to the tail. A
# panic line alone does not say what broke, so keep the four lines after it: for
# cargo that is the assertion, its left/right values, and the note. `---- x ----`
# and `test result: FAILED` carry the failing test's name and the pass/fail
# counts; SKIP lines say which runtime self-skips fired on this host.
detail=$(awk '
	/^(test .+ (FAILED|panicked)|test result: FAILED|failures:|---- .+ ----|error(\[|:)|ERROR:|The following tests FAILED|VIOLATION:)/ { print; next }
	/^[[:space:]]*(SKIP|assertion) /                                                                                              { print; next }
	/^[[:space:]]+[0-9]+ - .+(Failed|Timeout)/                                                                                    { print; next }
	/^make(\[[0-9]+\])?: \*\*\*/                                                                                            { print; next }
	/^thread .+ panicked/ { print; window = 4; next }
	window > 0            { print; window--; next }
' "$log" | head -n 60)
[ -n "$detail" ] || detail=$(tail -c 1500 "$log")
[ -n "$detail" ] || detail='the gate produced no output'
detail=${detail//'%'/'%25'}
detail=${detail//$'\n'/'%0A'}
detail=${detail//$'\r'/'%0D'}
if [ "$status" -eq 124 ]; then
	echo "::error title=$title TIMED OUT after ${bound}s::$detail"
else
	echo "::error title=$title failed (exit $status)::$detail"
fi
exit "$status"
