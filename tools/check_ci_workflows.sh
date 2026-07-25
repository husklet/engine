#!/usr/bin/env sh
# Structural assertions for the project workflows. Keep this POSIX-only: it
# runs in the Nix unit derivation and on both Linux and macOS.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
wfdir=.github/workflows
mode=${1:-invariants}

case "$mode" in
invariants)
	awk '
	function bad(message) {
		failures++
		print "VIOLATION: " message > "/dev/stderr"
	}
	function flush_step() {
		if (step_name == "" && step_run == "" && step_uses == "") return
		steps++
		step_files[steps] = FILENAME
		step_jobs[steps] = job
		step_names[steps] = step_name
		step_runs[steps] = step_run
		step_timeouts[steps] = step_timeout
		step_name = ""; step_run = ""; step_uses = ""; step_timeout = 0
	}
	function indentation(value) {
		match(value, /^ */)
		return RLENGTH
	}
	FNR == 1 {
		flush_step()
		in_jobs = 0; in_steps = 0; in_run = 0; job = ""
		files[FILENAME] = 1
	}
	{
		line = $0
		sub(/\r$/, "", line)
	}
	line ~ /^[ \t]*continue-on-error[ \t]*:/ {
		bad("I1 " FILENAME ": continue-on-error masks failures")
	}
	line ~ /--skip[ =]/ && line !~ /^[ \t]*#/ {
		bad("I2 " FILENAME ": --skip masks test coverage")
	}
	line ~ /^jobs:[ \t]*$/ {
		in_jobs = 1; saw_jobs[FILENAME] = 1
		next
	}
	!in_jobs { next }
	line ~ /^  [A-Za-z0-9_-]+:[ \t]*$/ {
		flush_step()
		in_steps = 0
		job = line
		sub(/^  /, "", job); sub(/:.*$/, "", job)
		jobs++
		job_files[jobs] = FILENAME
		job_names[jobs] = job
		job_timeouts[jobs] = 0
		job_calls[jobs] = 0
		next
	}
	line ~ /^    timeout-minutes:/ && !in_steps {
		job_timeouts[jobs] = 1
		next
	}
	line ~ /^    uses:/ && !in_steps {
		job_calls[jobs] = 1
		next
	}
	line ~ /^    steps:[ \t]*$/ {
		flush_step(); in_steps = 1
		next
	}
	in_steps && line ~ /^      - / {
		flush_step()
		value = line; sub(/^      - /, "", value)
		if (value ~ /^name:/) {
			step_name = value; sub(/^name:[ \t]*/, "", step_name)
		} else if (value ~ /^uses:/) {
			step_uses = value
		} else if (value ~ /^run:/) {
			step_run = value; in_run = 1
		}
		next
	}
	in_steps && line ~ /^        name:/ {
		step_name = line; sub(/^ *name:[ \t]*/, "", step_name)
		next
	}
	in_steps && line ~ /^        uses:/ {
		step_uses = line
		next
	}
	in_steps && line ~ /^        timeout-minutes:/ {
		step_timeout = 1
		next
	}
	in_steps && line ~ /^        run:/ {
		step_run = line; in_run = 1
		next
	}
	in_steps && in_run && indentation(line) >= 10 {
		step_run = step_run " " line
		next
	}
	in_steps && line ~ /^        [A-Za-z-]+:/ {
		in_run = 0
		next
	}
	END {
		flush_step()
		for (file in files)
			if (!saw_jobs[file])
				bad("I3 " file ": jobs block was not parsed")
		for (i = 1; i <= jobs; i++)
			if (!job_timeouts[i] && !job_calls[i])
				bad("I4 " job_files[i] " job `" job_names[i] "` has no timeout")
		for (i = 1; i <= steps; i++) {
			if (step_runs[i] ~ /(nix build|nix develop|make |cargo )/ &&
			    !step_timeouts[i])
				bad("I5 " step_files[i] " step `" step_names[i] "` has no timeout")
			if (step_runs[i] ~ /attempt 1/ &&
			    (step_runs[i] !~ /both attempts/ || step_runs[i] !~ /exit 1/))
				bad("I6 " step_files[i] " step `" step_names[i] "` does not check retry 2")
		}
		if (jobs < 7)
			bad("I7 parsed only " jobs " jobs; expected at least 7")
		if (failures) {
			print "check-ci-workflows: " failures " violation(s)" > "/dev/stderr"
			exit 1
		}
		print "check-ci-workflows: " jobs " jobs, " steps " steps; invariants hold"
	}' "$wfdir"/*.yml

	if grep -Eq '\[[^]]*\$st[^]]*\][[:space:]]*&&[[:space:]]*break' \
		"$wfdir/linux.yml" "$wfdir/mac.yml"; then
		printf '%s\n' 'VIOLATION: I8 compatibility loops stop after the first failing suite' >&2
		exit 1
	fi

	for workflow in "$wfdir/linux.yml" "$wfdir/mac.yml"; do
		if ! grep -Fq "cancel-in-progress: \${{ github.ref != 'refs/heads/main' }}" \
			"$workflow"; then
			printf 'VIOLATION: I9 %s may cancel a main-branch verdict\n' "$workflow" >&2
			exit 1
		fi
		if ! grep -Fq "github.ref == 'refs/heads/main' && github.sha || github.ref" \
			"$workflow"; then
			printf 'VIOLATION: I10 %s supersedes queued main-branch verdicts\n' "$workflow" >&2
			exit 1
		fi
	done

	full_line=$(grep -nF 'name: Full Rust integration suite' "$wfdir/linux.yml" |
		cut -d: -f1)
	fresh_line=$(grep -nF 'name: Check the committed crate archives match the C sources' \
		"$wfdir/linux.yml" | cut -d: -f1)
	if [ -z "$full_line" ] || [ -z "$fresh_line" ] ||
		[ "$fresh_line" -le "$full_line" ]; then
		printf '%s\n' 'VIOLATION: I11 archive freshness masks the full integration gate' >&2
		exit 1
	fi
	;;

publish-gate)
	pub=$wfdir/publish.yml
	failures=0
	for pattern in \
		'uses: ./.github/workflows/linux.yml' \
		'uses: ./.github/workflows/mac.yml' \
		'needs: [linux, mac]' \
		'make check-crate-archives' \
		'--test packaged_archive'
	do
		if ! grep -Fq -- "$pattern" "$pub"; then
			printf 'VIOLATION: P1 %s lacks `%s`\n' "$pub" "$pattern" >&2
			failures=1
		fi
	done
	[ "$failures" -eq 0 ] || exit 1
	printf '%s\n' 'check-ci-workflows: publish is gated by both hosts and exact archive checks'
	;;

*)
	printf 'usage: %s [invariants|publish-gate]\n' "$0" >&2
	exit 2
	;;
esac
