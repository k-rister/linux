#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-2}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
seccomp_tool=${CPU_ACCEL_SECCOMP_CHURN:-}
seccomp_cpus=${CPU_ACCEL_SECCOMP_CPUS:-3-7}
seccomp_workers=${CPU_ACCEL_SECCOMP_WORKERS:-4}
seccomp_duration_ms=${CPU_ACCEL_SECCOMP_DURATION_MS:-8000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
event_instance="$trace_root/instances/cpu_accel_ftrace_$$"
graph_instance="$trace_root/instances/cpu_accel_fgraph_$$"
trace_pid=
run_pid=
seccomp_pid=
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-ftrace-maintenance.XXXXXX)}
graph_rc=0
run_rc=0
seccomp_rc=0

fail()
{
	echo "cpu_accel ftrace maintenance test: $* (logs: $outdir)" >&2
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$seccomp_pid" ]; then
		wait "$seccomp_pid" 2>/dev/null || true
		seccomp_pid=
	fi
	if [ -n "$trace_pid" ]; then
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ -d "$event_instance" ]; then
		echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		rmdir "$event_instance" 2>/dev/null || true
	fi
	if [ -d "$graph_instance" ]; then
		echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
		timeout --kill-after=2s 12s rmdir "$graph_instance" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
case "$target_cpu" in
	''|*[!0-9]*) fail "CPU must be a nonnegative integer" ;;
esac
case "$escape_ms" in
	''|*[!0-9]*) fail "escape delay must be a positive integer" ;;
esac
[ "$escape_ms" -gt 0 ] || fail "escape delay must be positive"
[ "$escape_ms" -lt 5000 ] || fail "escape delay must be shorter than the workload"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

if [ -n "$seccomp_tool" ]; then
	[ -x "$seccomp_tool" ] || \
		fail "seccomp churn tool is not executable: $seccomp_tool"
	command -v taskset >/dev/null 2>&1 || \
		fail "taskset is required for seccomp churn"
	taskset -c "$seccomp_cpus" true >/dev/null 2>&1 || \
		fail "seccomp CPU list is unavailable: $seccomp_cpus"
	[ -r /proc/sys/net/core/bpf_jit_enable ] || \
		fail "cannot verify that BPF JIT is enabled"
	jit_enabled=$(cat /proc/sys/net/core/bpf_jit_enable)
	case "$jit_enabled" in
		1|2) ;;
		*) fail "BPF JIT is disabled (bpf_jit_enable=$jit_enabled)" ;;
	esac
fi

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$event_instance" "$graph_instance"
echo 4096 >"$event_instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$event_instance/events/cpu_accel/$event/enable"
done
cat "$event_instance/trace_pipe" >"$outdir/owner-trace.log" &
trace_pid=$!
echo 1 >"$event_instance/tracing_on"

# Removing this instance unregisters function-graph tracing. Its ftrace
# shutdown path enters the accelerator maintenance gate while the owner runs.
echo function_graph >"$graph_instance/current_tracer"
echo 1 >"$graph_instance/tracing_on"

# Optionally overlap BPF JIT allocation and filter teardown with owner
# maintenance, matching the seccomp/ftrace contention seen in VM reports.
if [ -n "$seccomp_tool" ]; then
	taskset -c "$seccomp_cpus" "$seccomp_tool" "$seccomp_workers" \
		"$seccomp_duration_ms" >"$outdir/seccomp.log" 2>&1 &
	seccomp_pid=$!
	sleep 0.2
fi

"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload user-hang --escape-after-ms "$escape_ms" --escape-retries 1 \
	>"$outdir/ctl.log" 2>&1 &
run_pid=$!
sleep 0.5

if timeout --kill-after=2s 12s rmdir "$graph_instance" \
	>"$outdir/rmdir.log" 2>&1; then
	graph_instance=
else
	graph_rc=$?
fi

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
if [ -n "$seccomp_pid" ]; then
	if wait "$seccomp_pid"; then
		seccomp_rc=0
	else
		seccomp_rc=$?
	fi
	seccomp_pid=
fi

echo 0 >"$event_instance/tracing_on"
kill -TERM "$trace_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
dmesg | grep -Ei 'soft lockup|INFO: task .* blocked for more than|RCU.*stall|(^|[[:space:]])BUG:|Oops:|Kernel panic' \
	>"$outdir/kernel-errors.log" || true

[ "$graph_rc" -eq 0 ] || fail "function_graph instance teardown failed or timed out"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
if [ -n "$seccomp_tool" ]; then
	[ "$seccomp_rc" -eq 0 ] || fail "seccomp JIT churn failed"
	grep -q '^PASS: workers=' "$outdir/seccomp.log" || \
		fail "seccomp JIT churn did not report PASS"
	awk -v expected="$seccomp_workers" \
		'/^worker=[0-9]+ filters=[1-9][0-9]*$/ { count++ } \
		 END { exit count != expected }' "$outdir/seccomp.log" || \
		fail "not all seccomp workers installed filters"
fi
grep -q 'state=7 ' "$outdir/ctl.log" || fail "owner did not report ESCAPED"
grep -q 'recovery_state=2' "$outdir/ctl.log" || \
	fail "owner escape recovery did not complete"
if ! awk -v target="$target_cpu" '
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
	if (number($0, "cpu") == target &&
	    index($0, "stop_cb=1") && index($0, "sent=1")) {
		stop_owner = number($0, "owner")
		stop_line = NR
	}
}
/owner_exit_complete:/ {
	if (number($0, "cpu") == target &&
	    number($0, "owner") == stop_owner &&
	    index($0, "stop_requested=1"))
		exit_line = NR
}
END {
	if (stop_owner != "" && stop_line && exit_line > stop_line)
		exit 0
	exit 1
}' "$outdir/owner-trace.log"; then
	cat "$outdir/owner-trace.log" >&2
	fail "trace did not show the matching owner stop and exit"
fi
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: function_graph teardown stopped the active owner through the maintenance gate"
cat "$outdir/ctl.log"
cat "$outdir/owner-trace.log"
if [ -n "$seccomp_tool" ]; then
	cat "$outdir/seccomp.log"
fi
echo "logs: $outdir"
