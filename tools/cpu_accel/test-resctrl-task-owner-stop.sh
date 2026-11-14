#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_RESCTRL_TASK_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_RESCTRL_TASK_DURATION_MS:-5000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
resctrl_root=${CPU_ACCEL_RESCTRL_FS:-/sys/fs/resctrl}
event_instance="$trace_root/instances/cpu_accel_resctrl_task_events_$$"
graph_instance="$trace_root/instances/cpu_accel_resctrl_task_graph_$$"
group="$resctrl_root/cpu-accel-resctrl-task-$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-resctrl-task.XXXXXX)}
mkdir -p "$outdir"
run_pid=
event_trace_pid=
graph_trace_pid=
dmesg_pid=
resctrl_mounted_by_test=0
group_created=0
event_instance_created=0
graph_instance_created=0
owner_started=0
run_rc=0
task_rc=0

fail()
{
	echo "cpu_accel resctrl-task owner-stop test: $* (logs: $outdir)" >&2
	exit 1
}

cpu_is_online()
{
	[ "$1" -eq 0 ] && return 0
	[ "$(cat "/sys/devices/system/cpu/cpu$1/online")" = 1 ]
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$graph_trace_pid" ]; then
		echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
		kill -TERM "$graph_trace_pid" 2>/dev/null || true
		wait "$graph_trace_pid" 2>/dev/null || true
		graph_trace_pid=
		echo >"$graph_instance/set_ftrace_filter" 2>/dev/null || true
		echo nop >"$graph_instance/current_tracer" 2>/dev/null || true
	fi
	if [ -n "$event_trace_pid" ]; then
		echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		kill -TERM "$event_trace_pid" 2>/dev/null || true
		wait "$event_trace_pid" 2>/dev/null || true
		event_trace_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ "$graph_instance_created" -eq 1 ]; then
		rmdir "$graph_instance" 2>/dev/null || true
		graph_instance_created=0
	fi
	if [ "$event_instance_created" -eq 1 ]; then
		rmdir "$event_instance" 2>/dev/null || true
		event_instance_created=0
	fi
	if [ "$group_created" -eq 1 ]; then
		rmdir "$group" 2>/dev/null || true
		group_created=0
	fi
	if [ "$resctrl_mounted_by_test" -eq 1 ] && mountpoint -q "$resctrl_root"; then
		umount "$resctrl_root" 2>/dev/null || true
		resctrl_mounted_by_test=0
	fi
	if [ "$owner_started" -eq 1 ]; then
		"$tool" reset >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
for command in cat date dmesg grep mountpoint sleep taskset; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw _update_task_closid_rmid "$trace_root/available_filter_functions" || \
	fail "_update_task_closid_rmid trace function is unavailable"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

if ! mountpoint -q "$resctrl_root"; then
	mount -t resctrl cpu-accel "$resctrl_root" || fail "could not mount resctrl"
	resctrl_mounted_by_test=1
fi
[ -r "$resctrl_root/schemata" ] || fail "resctrl schemata is unavailable"
[ -w "$resctrl_root/tasks" ] || fail "resctrl task interface is unavailable"

mkdir "$group"
group_created=1
cp "$resctrl_root/schemata" "$group/schemata" || \
	fail "could not initialize the resctrl group schema"
[ -w "$group/tasks" ] || fail "resctrl group task interface is unavailable"

mkdir "$event_instance"
event_instance_created=1
echo 4096 >"$event_instance/buffer_size_kb"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	echo 1 >"$event_instance/events/$event/enable"
done
echo 0 >"$event_instance/tracing_on"
cat "$event_instance/trace_pipe" >"$outdir/owner-trace.log" &
event_trace_pid=$!

mkdir "$graph_instance"
graph_instance_created=1
echo 4096 >"$graph_instance/buffer_size_kb"
echo >"$graph_instance/set_ftrace_pid"
printf '%s\n' _update_task_closid_rmid >"$graph_instance/set_ftrace_filter"
echo function_graph >"$graph_instance/current_tracer"
echo 0 >"$graph_instance/tracing_on"
cat "$graph_instance/trace_pipe" >"$outdir/function-graph.log" &
graph_trace_pid=$!
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

rm -f "$outdir/active"
CPU_ACCEL_ACTIVE_MARKER="$outdir/active" "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--workload user-oslat >"$outdir/ctl.log" 2>&1 &
run_pid=$!
owner_started=1

ready=0
for attempt in $(seq 1 500); do
	if [ -f "$outdir/active" ]; then
		ready=1
		break
	fi
	kill -0 "$run_pid" 2>/dev/null || break
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry marker timed out"

owner_pid=$(cat "/proc/$run_pid/task/$run_pid/children")
owner_pid=${owner_pid%% *}
[ -n "$owner_pid" ] || fail "could not identify the isolated owner task"

echo 1 >"$event_instance/tracing_on"
echo 1 >"$graph_instance/tracing_on"
start_ns=$(date +%s%N)
if taskset -c "$work_cpu" sh -c 'printf "%s\n" "$1" >"$2"' \
	sh "$owner_pid" "$group/tasks" >"$outdir/resctrl-task.log" 2>&1; then
	task_rc=0
else
	task_rc=$?
fi
end_ns=$(date +%s%N)
task_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
echo 0 >"$graph_instance/tracing_on"
sleep 0.1
kill -TERM "$graph_trace_pid" 2>/dev/null || true
wait "$graph_trace_pid" 2>/dev/null || true
graph_trace_pid=
echo >"$graph_instance/set_ftrace_filter"
echo nop >"$graph_instance/current_tracer"

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
sleep 0.1
echo 0 >"$event_instance/tracing_on"
kill -TERM "$event_trace_pid" "$dmesg_pid" 2>/dev/null || true
wait "$event_trace_pid" 2>/dev/null || true
event_trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

kernel_error_pattern='soft lockup|task .* blocked for more than|RCU.*stall'
kernel_error_pattern="$kernel_error_pattern|(^|[[:space:]])BUG:|Oops:|Kernel panic"
grep -Ei "$kernel_error_pattern" "$outdir/dmesg-new.log" \
	>"$outdir/kernel-errors.log" || true

[ "$task_rc" -eq 0 ] || fail "resctrl task-group change failed"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$task_elapsed_ms" -lt 2000 ] || \
	fail "resctrl task-group change waited too long (${task_elapsed_ms} ms)"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "resctrl task-group change required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the resctrl task-group change"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the resctrl task-group change"
grep -q 'arch_ipi_count=0' "$outdir/ctl.log" || \
	fail "resctrl task-group change sent an architecture IPI to the owner"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the resctrl task-group change"

grep -Eq "^[[:space:]]*$target_cpu\\).*_update_task_closid_rmid" \
	"$outdir/function-graph.log" || \
	fail "_update_task_closid_rmid was not traced on the target CPU"
stop_line=$(grep -E "owner_stop_request:.*cpu=$target_cpu " \
	"$outdir/owner-trace.log" | tail -n 1 || true)
exit_line=$(grep -E \
	"owner_exit_complete:.*cpu=$target_cpu .*stop_requested=1" \
	"$outdir/owner-trace.log" | tail -n 1 || true)
[ -n "$stop_line" ] || fail "owner-stop trace is missing"
[ -n "$exit_line" ] || fail "owner-exit trace is missing"
printf '%s\n' "$stop_line" | \
	grep -q 'caller=__smp_call_function_single' || \
	fail "resctrl task-group change did not use the waited single-CPU path"

stop_timestamp=$(printf '%s\n' "$stop_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v stop="$stop_timestamp" -v complete="$exit_timestamp" \
	'BEGIN { exit !(stop > 0 && complete > stop) }' || \
	fail "owner stop did not precede owner exit in the trace"

rmdir "$group"
group_created=0

echo "cpu_accel resctrl-task owner-stop test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu task_pid=$owner_pid"
echo "task_elapsed_ms=$task_elapsed_ms"
echo "stop_line=$stop_line"
echo "exit_line=$exit_line"
