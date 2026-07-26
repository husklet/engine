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
		# last_file, not FILENAME: the final step of a file is flushed at
		# FNR==1 of the next one, which would name the wrong workflow.
		step_files[steps] = last_file
		step_jobs[steps] = job
		step_names[steps] = step_name
		step_runs[steps] = step_run
		step_timeouts[steps] = step_timeout
		step_jobidx[steps] = jobs
		step_name = ""; step_run = ""; step_uses = ""; step_timeout = 0
	}
	function number(value) {
		match(value, /[0-9]+/)
		return substr(value, RSTART, RLENGTH) + 0
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
		last_file = FILENAME
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
		job_timeouts[jobs] = number(line)
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
		step_timeout = number(line)
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
			if (step_runs[i] ~ /(nix build|nix develop|cargo )/ &&
			    !step_timeouts[i])
				bad("I5 " step_files[i] " step `" step_names[i] "` has no timeout")
			if (step_runs[i] ~ /attempt 1/ &&
			    (step_runs[i] !~ /both attempts/ || step_runs[i] !~ /exit 1/))
				bad("I6 " step_files[i] " step `" step_names[i] "` does not check retry 2")
			# A retried step must bound BOTH attempts against one deadline:
			# a step the runner kills on its own timeout prints no ::error.
			if (step_runs[i] ~ /attempt 1/ &&
			    (step_runs[i] !~ /deadline=/ || step_runs[i] !~ /run_bounded/))
				bad("I16 " step_files[i] " step `" step_names[i] "` retries without a step deadline")
			# Every test-lane gate must name its own failure: the job-log
			# endpoint needs admin rights, so a step that only exits
			# non-zero is undiagnosable from the public check-run data.
			if (step_runs[i] ~ /(nix build|nix develop|cargo )/ &&
			    step_runs[i] !~ /ci_run\.sh/ && step_runs[i] !~ /::error/ &&
			    step_runs[i] !~ /for attempt in/)
				bad("I17 " step_files[i] " step `" step_names[i] "` reports no ::error on failure")
			# Raising a bucket timeout must raise the arithmetic with it: the
			# deadline plus its 60s reporting margin has to fit the step
			# timeout, and the step timeout has to fit the job timeout.
			if (match(step_runs[i], /SECONDS \+ [0-9]+/)) {
				secs = substr(step_runs[i], RSTART + 10, RLENGTH - 10) + 0
				limit = step_timeouts[i] * 60
				if (limit <= 0 || secs + 60 > limit)
					bad("I18 " step_files[i] " step `" step_names[i] "` deadline " \
					    secs "s+60s exceeds its " step_timeouts[i] "min timeout")
			}
			jt = job_timeouts[step_jobidx[i]]
			if (jt > 0 && step_timeouts[i] > jt)
				bad("I18 " step_files[i] " step `" step_names[i] "` timeout exceeds job `" \
				    step_jobs[i] "` timeout")
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

	# checkout<=v4, nix-installer<=v21 and cache-nix-action<=v6 run on node20.
	if grep -Eq 'actions/checkout@v[1-4]([^0-9]|$)|nix-installer-action@v([1-9]|1[0-9]|2[01])([^0-9]|$)|nix-installer-action@main|cache-nix-action@(v[1-6]([^0-9]|$)|main)' \
		"$wfdir"/*.yml; then
		printf '%s\n' 'VIOLATION: I9 a workflow action still targets the Node 20 generation' >&2
		exit 1
	fi

	# I10/I11 apply to linux.yml only. Per-SHA groups preserve a verdict for every
	# main commit, which is right when the lane can keep up. The macOS lane cannot:
	# its shards are 3-7 min of work but wait hours for a runner, so per-SHA groups
	# stopped superseding anything and the queue grew until NO commit got a verdict
	# (measured: jobs waiting 9h). Coalescing there trades per-commit verdicts for
	# a timely verdict on the tip, which is the only one anyone acts on.
	if ! grep -Fq "cancel-in-progress: \${{ github.ref != 'refs/heads/main' }}" \
		"$wfdir/linux.yml"; then
		printf 'VIOLATION: I10 %s may cancel a main-branch verdict\n' "$wfdir/linux.yml" >&2
		exit 1
	fi
	if ! grep -Fq "github.ref == 'refs/heads/main' && github.sha || github.ref" \
		"$wfdir/linux.yml"; then
		printf 'VIOLATION: I11 %s supersedes queued main-branch verdicts\n' "$wfdir/linux.yml" >&2
		exit 1
	fi
	# The macOS lane must still coalesce by ref rather than run unbounded copies.
	if ! grep -Fq 'group: mac-${{ github.workflow }}-${{ github.ref }}' "$wfdir/mac.yml"; then
		printf 'VIOLATION: I10b %s must coalesce macOS runs by ref\n' "$wfdir/mac.yml" >&2
		exit 1
	fi

	# I13-I15: the sharded compat matrices are the only place a suite runs, so a
	# lane cmake/CiLanes.cmake declares but no shard names is silently unrun.
	# cmake/CiLanes.cmake is the source of truth; gate.ci-lane-parity separately
	# proves each declared lane is a NON-EMPTY CTest selection on its host.
	lanes_in() {
		awk -v name="$1" '
		$0 ~ "^set\\(" name "$" { on = 1; next }
		on && /^\)/ { exit }
		on { gsub(/[ \t]/, ""); if ($0 != "" && $0 !~ /^#/) print }
		' cmake/CiLanes.cmake
	}
	shard_targets() {
		sed -n 's/^ *targets: *//p' "$1" | tr ' ' '\n' | sed '/^$/d'
	}
	# Both directions: a declared lane must be sharded exactly once, and no shard
	# may name a lane that is not declared.
	check_shards() {
		wf=$1
		declared=$2
		id=$3
		shards=$(shard_targets "$wf")
		for lane in $declared; do
			count=$(printf '%s\n' "$shards" | grep -Fxc -- "$lane" || true)
			if [ "$count" -eq 0 ]; then
				printf 'VIOLATION: %s %s runs no shard for `%s`\n' \
					"$id" "$wf" "$lane" >&2
				return 1
			fi
			# Consolidating buckets must MOVE a lane, not copy it: a lane named
			# twice burns a scarce runner on work already covered.
			if [ "$count" -gt 1 ]; then
				printf 'VIOLATION: %s %s runs `%s` in %s shards\n' \
					"$id" "$wf" "$lane" "$count" >&2
				return 1
			fi
		done
		for lane in $shards; do
			if ! printf '%s\n' $declared | grep -Fqx -- "$lane"; then
				printf 'VIOLATION: %s %s shards `%s`, undeclared in cmake/CiLanes.cmake\n' \
					"$id" "$wf" "$lane" >&2
				return 1
			fi
		done
	}
	check_shards "$wfdir/mac.yml" "$(lanes_in HL_CI_SHARDED_DARWIN)" I13 || exit 1
	check_shards "$wfdir/linux.yml" "$(lanes_in HL_CI_SHARDED_LINUX)" I14 || exit 1

	# I15: `ctest -L <label>` EXITS 0 when the label matches nothing. Every
	# label-driven step must pass --no-tests=error or a dropped lane reports
	# green. `-N` is listing, not running, and is exempt.
	if awk '/^[ \t]*#/ { next }
		/ctest/ && / -L / && !/--no-tests=error/ && !/ -N / {
			print "  " FILENAME ": " $0 > "/dev/stderr"; bad = 1
		}
		END { exit !bad }' "$wfdir"/*.yml; then
		printf '%s\n' 'VIOLATION: I15 a `ctest -L` step lacks --no-tests=error' >&2
		exit 1
	fi

	full_line=$(grep -nF 'name: Full Rust integration suite' "$wfdir/linux.yml" |
		cut -d: -f1)
	fresh_line=$(grep -nF 'name: Check the committed crate archives are intact' \
		"$wfdir/linux.yml" | cut -d: -f1)
	if [ -z "$full_line" ] || [ -z "$fresh_line" ] ||
		[ "$fresh_line" -le "$full_line" ]; then
		printf '%s\n' 'VIOLATION: I12 archive freshness masks the full integration gate' >&2
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
		'--target check-crate-archives' \
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
