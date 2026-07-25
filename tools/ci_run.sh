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

tail -n 400 "$log"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
	tail -c 200000 "$log" >>"$GITHUB_STEP_SUMMARY" || true
fi

[ "$status" -ne 0 ] || exit 0

# Prefer the lines that name a failing test or gate; fall back to the tail.
detail=$(grep -aE '^(test .+ (FAILED|panicked)|failures:|error(\[|:)|thread .+ panicked|The following tests FAILED|[[:space:]]+[0-9]+ - .+(Failed|Timeout)|VIOLATION:|make(\[[0-9]+\])?: \*\*\*)' \
	"$log" | head -n 25)
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
