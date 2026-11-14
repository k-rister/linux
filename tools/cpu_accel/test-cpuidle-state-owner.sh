#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_CPUIDLE_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_CPUIDLE_DURATION_MS:-5000}
state_index=${CPU_ACCEL_CPUIDLE_STATE_INDEX:-1}
trace_root=${TRACEFS:-/sys/kernel/tracing}
state_dir="/sys/devices/system/cpu/cpu$target_cpu/cpuidle/state$state_index"
event_instance="$trace_root/instances/cpu_accel_cpuidle_state_events_$$"
graph_instance="$trace_root/instances/cpu_accel_cpuidle_state_graph_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-cpuidle.XXXXXX)}
mkdir -p "$outdir"
fifo="$outdir/release.fifo"
run_pid=
event_trace_pid=
graph_trace_pid=
helper_pid=
dmesg_pid=
owner_started=0
state_changed=0
event_instance_created=0
graph_instance_created=0

fail()
{
	echo "cpu_accel cpuidle-state owner test: $* (logs: $outdir)" >&2
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
	if [ -n "$helper_pid" ]; then
		kill -TERM "$helper_pid" 2>/dev/null || true
		wait "$helper_pid" 2>/dev/null || true
		helper_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -n "$graph_trace_pid" ]; then
		echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
		echo >"$graph_instance/set_ftrace_pid" 2>/dev/null || true
		echo >"$graph_instance/set_ftrace_filter" 2>/dev/null || true
		echo nop >"$graph_instance/current_tracer" 2>/dev/null || true
		kill -TERM "$graph_trace_pid" 2>/dev/null || true
		wait "$graph_trace_pid" 2>/dev/null || true
		graph_trace_pid=
	fi
	if [ -n "$event_trace_pid" ]; then
		echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		kill -TERM "$event_trace_pid" 2>/dev/null || true
		wait "$event_trace_pid" 2>/dev/null || true
		event_trace_pid=
	fi
	if [ "$graph_instance_created" -eq 1 ]; then
		rmdir "$graph_instance" 2>/dev/null || true
		graph_instance_created=0
	fi
	if [ "$event_instance_created" -eq 1 ]; then
		rmdir "$event_instance" 2>/dev/null || true
		event_instance_created=0
	fi
	rm -f "$fifo"
	if [ "$state_changed" -eq 1 ]; then
		echo 0 >"$state_dir/disable" 2>/dev/null || true
		state_changed=0
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
[ -r "$state_dir/name" ] || fail "target CPU idle state is unavailable"
[ "$(cat "$state_dir/name")" != POLL ] || \
	fail "target idle state must not be POLL"
[ "$(cat "$state_dir/disable")" = 0 ] || \
	fail "target idle state must initially be enabled"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
for command in cat date dmesg grep sleep taskset; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw store_state_disable "$trace_root/available_filter_functions" || \
	fail "store_state_disable trace function is unavailable"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

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
printf '%s\n' store_state_disable >"$graph_instance/set_ftrace_filter"
echo function_graph >"$graph_instance/current_tracer"
echo 0 >"$graph_instance/tracing_on"

mkfifo "$fifo"
taskset -c "$work_cpu" sh -c \
	'read -r line < "$1"
	 printf "%s\n" 1 > "$2"' \
	sh "$fifo" "$state_dir/disable" >"$outdir/cpuidle.log" 2>&1 &
helper_pid=$!
sleep 0.01
echo "$helper_pid" >"$graph_instance/set_ftrace_pid"

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

echo 1 >"$event_instance/tracing_on"
echo 1 >"$graph_instance/tracing_on"
state_changed=1
start_ns=$(date +%s%N)
printf '1\n' >"$fifo"
if wait "$helper_pid"; then
	cpuidle_rc=0
else
	cpuidle_rc=$?
fi
helper_pid=
end_ns=$(date +%s%N)
cpuidle_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
echo >"$graph_instance/set_ftrace_pid"
echo 0 >"$graph_instance/tracing_on"

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
sleep 0.1
echo 0 >"$event_instance/tracing_on"
kill -TERM "$graph_trace_pid" "$event_trace_pid" "$dmesg_pid" \
	2>/dev/null || true
wait "$graph_trace_pid" 2>/dev/null || true
graph_trace_pid=
wait "$event_trace_pid" 2>/dev/null || true
event_trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

kernel_error_pattern='soft lockup|task .* blocked for more than|RCU.*stall'
kernel_error_pattern="$kernel_error_pattern|(^|[[:space:]])BUG:|Oops:|Kernel panic"
grep -Ei "$kernel_error_pattern" "$outdir/dmesg-new.log" \
	>"$outdir/kernel-errors.log" || true

[ "$cpuidle_rc" -eq 0 ] || fail "cpuidle state change failed"
[ "$(cat "$state_dir/disable")" = 1 ] || \
	fail "cpuidle state did not become disabled"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "cpuidle state change required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the cpuidle state change"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the cpuidle state change"
grep -q 'arch_ipi_count=0' "$outdir/ctl.log" || \
	fail "cpuidle state change sent an architecture IPI to the owner"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the cpuidle state change"

grep -Eq "^[[:space:]]*$work_cpu\\).*store_state_disable" \
	"$outdir/function-graph.log" || \
	fail "store_state_disable was not traced on the work CPU"
stop_line=$(grep -E "owner_stop_request:.*cpu=$target_cpu " \
	"$outdir/owner-trace.log" | tail -n 1 || true)
exit_line=$(grep -E \
	"owner_exit_complete:.*cpu=$target_cpu .*stop_requested=1" \
	"$outdir/owner-trace.log" | tail -n 1 || true)
[ -n "$stop_line" ] || fail "owner-stop trace is missing"
[ -n "$exit_line" ] || fail "owner-exit trace is missing"
printf '%s\n' "$stop_line" | \
	grep -q 'caller=smp_call_function_many_cond' || \
	fail "cpuidle state change did not use the waited call-function path"

stop_timestamp=$(printf '%s\n' "$stop_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v stop="$stop_timestamp" -v complete="$exit_timestamp" \
	'BEGIN { exit !(stop > 0 && complete > stop) }' || \
	fail "owner stop did not precede owner exit in the trace"

echo 0 >"$state_dir/disable"
state_changed=0
[ "$(cat "$state_dir/disable")" = 0 ] || \
	fail "failed to restore the original cpuidle state"

echo "cpu_accel cpuidle-state owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu state_index=$state_index state=$(cat "$state_dir/name") disable=0->1->0"
echo "cpuidle_elapsed_ms=$cpuidle_elapsed_ms"
echo "exit_line=$exit_line"
