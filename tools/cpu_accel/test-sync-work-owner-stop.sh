#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
prime_tool=${CPU_ACCEL_SYNC_WAIT_PRIME:-$script_dir/test-sync-wait-prime}
target_cpu=${CPU_ACCEL_CPU:-2}
work_cpu=${CPU_ACCEL_WORK_CPU:-3}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_sync_wait_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-sync-wait.XXXXXX)}
trace_pid=
dmesg_pid=
prime_pid=
run_pid=

fail()
{
	echo "cpu_accel synchronous-work owner-stop test: $* (logs: $outdir)" >&2
	if [ -f "$outdir/trace.log" ]; then
		grep -E 'owner_stop_request:|owner_exit_complete:' "$outdir/trace.log" \
			| tail -n 20 >&2 || true
	fi
	if [ -f "$outdir/kernel-errors.log" ]; then
		cat "$outdir/kernel-errors.log" >&2
	fi
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$prime_pid" ]; then
		kill -TERM "$prime_pid" 2>/dev/null || true
		wait "$prime_pid" 2>/dev/null || true
		prime_pid=
	fi
	if [ -n "$trace_pid" ]; then
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -d "$instance" ]; then
		echo 0 >"$instance/tracing_on" 2>/dev/null || true
		rmdir "$instance" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -x "$prime_tool" ] || fail "page-prime helper is not executable: $prime_tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -w /proc/sys/vm/stat_refresh ] || fail "vm.stat_refresh is unavailable"
[ -w /proc/sys/vm/drop_caches ] || fail "drop_caches is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw x86_cpu_accel_user_enter "$trace_root/available_filter_functions" || \
	fail "owner entry function is not traceable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done
echo x86_cpu_accel_user_enter >"$instance/set_ftrace_filter"
echo function_graph >"$instance/current_tracer"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
echo 1 >"$instance/tracing_on"
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

# Leave a small anonymous-page batch on the target CPU so drop_caches must
# queue and wait for lru_add_drain_per_cpu() there.
"$prime_tool" "$target_cpu" 8 >"$outdir/prime.log" 2>&1 &
prime_pid=$!
ready=0
for n in $(seq 1 300); do
	if grep -q '^ready ' "$outdir/prime.log"; then
		ready=1
		break
	fi
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "target CPU LRU pages were not primed"

run_case()
{
	name=$1
	sysctl_path=$2
	expected_caller=$3
	entry_count=$(grep -Fc 'x86_cpu_accel_user_enter();' "$outdir/trace.log" || true)

	"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
		--workload user-hang --escape-after-ms "$escape_ms" \
		--escape-retries 1 >"$outdir/$name-ctl.log" 2>&1 &
	run_pid=$!
	ready=0
	for n in $(seq 1 300); do
		count=$(grep -Fc 'x86_cpu_accel_user_enter();' "$outdir/trace.log" || true)
		if [ "$count" -gt "$entry_count" ]; then
			ready=1
			break
		fi
		sleep 0.01
	done
	[ "$ready" -eq 1 ] || fail "$name owner-entry trace timed out"

	start_ns=$(date +%s%N)
	if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
		sh -c "echo 1 > '$sysctl_path'" >"$outdir/$name-trigger.log" 2>&1; then
		helper_rc=0
	else
		helper_rc=$?
	fi
	end_ns=$(date +%s%N)
	helper_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
	if wait "$run_pid"; then
		run_rc=0
	else
		run_rc=$?
	fi
	run_pid=

	[ "$helper_rc" -eq 0 ] || fail "$name trigger failed or timed out"
	[ "$run_rc" -eq 0 ] || fail "$name owner run failed"
	grep -q 'state=7 ' "$outdir/$name-ctl.log" || \
		fail "$name owner did not report ESCAPED"
	grep -q 'recovery_state=2' "$outdir/$name-ctl.log" || \
		fail "$name owner escape recovery did not complete"
	[ "$helper_elapsed_ms" -lt 2000 ] || \
		fail "$name trigger waited ${helper_elapsed_ms} ms for timed owner escape"

	if ! awk -v target="$target_cpu" -v caller="$expected_caller" '
function number(line, name, rest, pos)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	rest = substr(line, pos + length(name) + 1)
	sub(/[^0-9].*$/, "", rest)
	return rest
}
/owner_stop_request:/ {
	if (number($0, "cpu") == target && index($0, "caller=" caller) &&
	    index($0, "stop_cb=1") && index($0, "sent=1")) {
		owner = number($0, "owner")
		stop_line = NR
	}
}
/owner_exit_complete:/ {
	if (number($0, "cpu") == target && number($0, "owner") == owner &&
	    index($0, "stop_requested=1"))
		exit_line = NR
}
END {
	exit !(owner != "" && stop_line && exit_line > stop_line)
}' "$outdir/trace.log"; then
	fail "$name trace did not pair owner stop with owner exit"
	fi
	echo "PASS: $name stopped CPU $target_cpu owner in ${helper_elapsed_ms} ms"
}

run_case vmstat /proc/sys/vm/stat_refresh schedule_on_each_cpu
run_case lru /proc/sys/vm/drop_caches __lru_add_drain_all

echo 0 >"$instance/tracing_on"
kill -TERM "$trace_pid" "$dmesg_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

grep -Ei 'soft lockup|task .* blocked for more than|RCU.*stall|(^|[[:space:]])BUG:|Oops:|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: synchronous per-CPU work and LRU drain stop active owners"
echo "logs: $outdir"
