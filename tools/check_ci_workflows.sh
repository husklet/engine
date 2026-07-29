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
	# One workflow file per HOST TOKEN (HL_CI_HOSTS in cmake/CiLanes.cmake).
	# The mapping is not derivable from the token -- linux.yml and mac.yml
	# predate the (OS, CPU) token and are not named for one -- so it is stated
	# here, once, with a violating default arm. Every check below that needs
	# "the workflow for host X" goes through this rather than a literal, which
	# is what stops a new host from being invisible to a guard that was written
	# when there were two.
	workflow_for() {
		case "$1" in
		Linux-aarch64) printf '%s\n' "$wfdir/linux.yml" ;;
		Linux-x86_64) printf '%s\n' "$wfdir/linux-x86_64.yml" ;;
		Darwin-aarch64) printf '%s\n' "$wfdir/mac.yml" ;;
		Windows-x86_64) printf '%s\n' "$wfdir/windows-x86_64.yml" ;;
		*) return 1 ;;
		esac
	}
	check_shards "$wfdir/mac.yml" "$(lanes_in HL_CI_SHARDED_DARWIN)" I13 || exit 1
	check_shards "$wfdir/linux.yml" "$(lanes_in HL_CI_SHARDED_LINUX)" I14 || exit 1

	# I19: cross-host parity. I13 and I14 each compare ONE host's declared list
	# against ONE workflow, so both hold for a lane that is simply absent from
	# HL_CI_SHARDED_LINUX -- which is how compat-abi-corpus, compat-core-*,
	# compat-isa-* and compat-soak (~270 cases) ran on macOS only for the life
	# of the matrix, and why smcprecise could fail deterministically on the
	# Linux engine with a manifest note claiming it passed. Asymmetry is now
	# legal only when HL_CI_SHARDED_HOST_ONLY declares it.
	#
	# A host is the (OS, CPU) pair, and parity holds only between the hosts
	# that shard compat -- HL_CI_COMPAT_HOSTS. Requiring it of Linux-x86_64
	# would mean exempting all 24 lanes; I20 keeps it honest instead.
	hosts=$(lanes_in HL_CI_HOSTS)
	compat_hosts=$(lanes_in HL_CI_COMPAT_HOSTS)
	linux_lanes=$(lanes_in HL_CI_SHARDED_LINUX)
	darwin_lanes=$(lanes_in HL_CI_SHARDED_DARWIN)
	host_only=$(lanes_in HL_CI_SHARDED_HOST_ONLY)
	has() { printf '%s\n' $2 | grep -Fqx -- "$1"; }
	parity=0
	for host in $compat_hosts; do
		has "$host" "$hosts" && continue
		printf 'VIOLATION: I19 `%s` is in HL_CI_COMPAT_HOSTS but not HL_CI_HOSTS\n' \
			"$host" >&2
		parity=1
	done
	# ONE sharded lane list per host OS, so a second compat host on the same OS
	# would leave it unable to say which host it describes. Split the list
	# rather than letting this guard pick one.
	sole_host_for() {
		set -- $(printf '%s\n' $compat_hosts | grep -e "^$1-" || true)
		[ "$#" -eq 1 ] || return 1
		printf '%s\n' "$1"
	}
	sole_or_fail() {
		if ! sole_host_for "$1"; then
			printf 'VIOLATION: I19 HL_CI_COMPAT_HOSTS must name exactly one %s host while HL_CI_SHARDED_%s is keyed by OS\n' \
				"$1" "$2" >&2
			return 1
		fi
	}
	linux_host=$(sole_or_fail Linux LINUX) || parity=1
	darwin_host=$(sole_or_fail Darwin DARWIN) || parity=1
	for lane in $darwin_lanes; do
		has "$lane" "$linux_lanes" && continue
		if ! has "$darwin_host:$lane" "$host_only"; then
			printf 'VIOLATION: I19 `%s` is sharded on %s only; declare `%s:%s` in HL_CI_SHARDED_HOST_ONLY or shard it on Linux\n' \
				"$lane" "$darwin_host" "$darwin_host" "$lane" >&2
			parity=1
		fi
	done
	for lane in $linux_lanes; do
		has "$lane" "$darwin_lanes" && continue
		if ! has "$linux_host:$lane" "$host_only"; then
			printf 'VIOLATION: I19 `%s` is sharded on %s only; declare `%s:%s` in HL_CI_SHARDED_HOST_ONLY or shard it on Darwin\n' \
				"$lane" "$linux_host" "$linux_host" "$lane" >&2
			parity=1
		fi
	done
	# A stale exemption is as dangerous as a missing lane: it silently blesses
	# an asymmetry that no longer exists, or one that never did.
	for entry in $host_only; do
		host=${entry%%:*}
		lane=${entry#*:}
		if ! has "$host" "$compat_hosts"; then
			printf 'VIOLATION: I19 `%s` names no compat host; use <host-token>:<lane> from HL_CI_COMPAT_HOSTS\n' \
				"$entry" >&2
			parity=1
			continue
		fi
		case "${host%%-*}" in
		Linux) own=$linux_lanes; other=$darwin_lanes ;;
		Darwin) own=$darwin_lanes; other=$linux_lanes ;;
		*)
			printf 'VIOLATION: I19 `%s` names a host OS with no HL_CI_SHARDED_* list\n' \
				"$entry" >&2
			parity=1
			continue
			;;
		esac
		if ! has "$lane" "$own"; then
			printf 'VIOLATION: I19 `%s` exempts `%s`, absent from HL_CI_SHARDED_%s\n' \
				"$entry" "$lane" "${host%%-*}" >&2
			parity=1
		fi
		if has "$lane" "$other"; then
			printf 'VIOLATION: I19 `%s` is stale: `%s` is sharded on both hosts\n' \
				"$entry" "$lane" >&2
			parity=1
		fi
	done
	# I20: a host token absent from HL_CI_COMPAT_HOSTS shards nothing, so its
	# workflow must name no lane. I13/I14 cannot see this -- each looks at one
	# other file. It stops applying to a host once that host's token is added
	# to HL_CI_COMPAT_HOSTS, which is why that addition must land in the same
	# change as the sharded matrix job it turns the guard off for.
	#
	# This used to test the literal `Linux-x86_64`, which meant the guard was
	# not off for a new host -- it had never been ON. A third token could be
	# declared, get a workflow, and be checked by nothing at all: not by I13 or
	# I14 (hardcoded to two files), and not by I20 (hardcoded to one token).
	# Iterating is what makes the guarantee automatic for every future host.
	for host in $hosts; do
		wf=$(workflow_for "$host") || {
			printf 'VIOLATION: I20 `%s` is in HL_CI_HOSTS but workflow_for() in %s maps no workflow file to it; a host token no guard can locate is unguarded\n' \
				"$host" "$0" >&2
			parity=1
			continue
		}
		if [ ! -f "$wf" ]; then
			printf 'VIOLATION: I20 `%s` is declared in HL_CI_HOSTS but %s does not exist; one workflow file per host token\n' \
				"$host" "$wf" >&2
			parity=1
			continue
		fi
		# I21: a declared host must declare lanes. gate.ci-lane-parity proves
		# each declared lane is a NON-EMPTY CTest selection, but it can only do
		# that ON that host -- so it is exactly the check that is missing while
		# a new host has no runner, or has one whose configure drops the gate.
		# This runs on Linux and macOS on every push, so the wiring of a host
		# token is guarded from the moment it is written, by machines that
		# already exist. Empty here means lane-parity-gate would exit 1 with
		# "parsed no lanes" the first time the new runner ran it.
		os=${host%%-*}
		upper=$(printf '%s\n' "$os" | tr 'a-z' 'A-Z')
		declared=$(lanes_in "HL_CI_SHARDED_$upper"
			lanes_in "HL_CI_DIRECT_$upper"
			lanes_in "HL_CI_REGISTRY_$upper")
		if [ -z "$declared" ]; then
			printf 'VIOLATION: I21 `%s` is declared in HL_CI_HOSTS but HL_CI_{SHARDED,DIRECT,REGISTRY}_%s declare no lane between them; the token would be checked by nothing\n' \
				"$host" "$upper" >&2
			parity=1
		fi
		has "$host" "$compat_hosts" && continue
		check_shards "$wf" "" I20 || parity=1
	done
	[ "$parity" -eq 0 ] || exit 1

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
