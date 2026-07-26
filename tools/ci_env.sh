#!/usr/bin/env bash
# Dump the host facts that make a gate pass locally and fail on a runner, and
# emit them as a ::notice annotation. Annotations are readable through the public
# check-runs/<job_id>/annotations endpoint; job logs are not, so a difference that
# only exists on the runner is otherwise unobservable.
set -u

dump() {
	echo "user: $(id -un) uid=$(id -u) $( [ "$(id -u)" -eq 0 ] && echo root || echo non-root)"
	echo "uname: $(uname -a)"
	echo "cpus: nproc=$(nproc 2>/dev/null || echo ?) available=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo ?)"
	echo "nofile: soft=$(ulimit -n) hard=$(ulimit -Hn)"
	echo "nproc-rlimit: soft=$(ulimit -u) hard=$(ulimit -Hu)"
	echo "stack: soft=$(ulimit -s) hard=$(ulimit -Hs)"
	echo "memlock: soft=$(ulimit -l) hard=$(ulimit -Hl)"
	echo "as: soft=$(ulimit -v) hard=$(ulimit -Hv)"
	echo "HOME=${HOME:-} TMPDIR=${TMPDIR:-<unset>} PWD=$PWD"
	if [ -r /proc/meminfo ]; then
		grep -E '^(MemTotal|MemAvailable|SwapTotal)' /proc/meminfo | tr -s ' '
	else
		echo "memory: $(sysctl -n hw.memsize 2>/dev/null || echo ?) bytes"
	fi
	echo "--- df -T ---"
	df -T . /tmp "${TMPDIR:-/tmp}" 2>/dev/null || df . /tmp
	echo "--- sysctl ---"
	for knob in \
		fs/nr_open fs/file-max fs/aio-max-nr \
		fs/inotify/max_user_instances fs/inotify/max_user_watches fs/inotify/max_queued_events \
		kernel/pid_max kernel/threads-max kernel/randomize_va_space \
		kernel/yama/ptrace_scope kernel/apparmor_restrict_unprivileged_userns \
		vm/max_map_count vm/overcommit_memory; do
		[ -r "/proc/sys/$knob" ] && echo "$knob=$(cat "/proc/sys/$knob")"
	done
	echo "--- cgroup ---"
	for knob in memory.max memory.high pids.max cpu.max; do
		[ -r "/sys/fs/cgroup/$knob" ] && echo "$knob=$(cat "/sys/fs/cgroup/$knob")"
	done
	true
}

text=$(dump 2>&1)
printf '%s\n' "$text"

[ -n "${GITHUB_ACTIONS:-}" ] || exit 0
encoded=${text//'%'/'%25'}
encoded=${encoded//$'\n'/'%0A'}
encoded=${encoded//$'\r'/'%0D'}
echo "::notice title=Runner environment (${1:-host})::$encoded"
