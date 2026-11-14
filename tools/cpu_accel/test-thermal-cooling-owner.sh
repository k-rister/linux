#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_THERMAL_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_THERMAL_DURATION_MS:-5000}
cooling_state=${CPU_ACCEL_THERMAL_COOLING_STATE:-2}
trace_root=${TRACEFS:-/sys/kernel/tracing}
event_instance="$trace_root/instances/cpu_accel_thermal_events_$$"
graph_instance="$trace_root/instances/cpu_accel_thermal_graph_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-thermal.XXXXXX)}
mkdir -p "$outdir"
run_pid=
event_trace_pid=
graph_trace_pid=
dmesg_pid=
event_instance_created=0
graph_instance_created=0
owner_started=0
cooling_changed=0
cooling_device=
set_state_function=
original_max_freq=0
new_max_freq=0

fail()
{
	echo "cpu_accel thermal-cooling owner test: $* (logs: $outdir)" >&2
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
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -n "$event_trace_pid" ]; then
		if [ "$event_instance_created" -eq 1 ]; then
			echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		fi
		kill -TERM "$event_trace_pid" 2>/dev/null || true
		wait "$event_trace_pid" 2>/dev/null || true
		event_trace_pid=
	fi
	if [ -n "$graph_trace_pid" ]; then
		if [ "$graph_instance_created" -eq 1 ]; then
			echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
			echo >"$graph_instance/set_ftrace_pid" 2>/dev/null || true
			echo >"$graph_instance/set_ftrace_filter" 2>/dev/null || true
			echo nop >"$graph_instance/current_tracer" 2>/dev/null || true
		fi
		kill -TERM "$graph_trace_pid" 2>/dev/null || true
		wait "$graph_trace_pid" 2>/dev/null || true
		graph_trace_pid=
	fi
	if [ "$event_instance_created" -eq 1 ]; then
		rmdir "$event_instance" 2>/dev/null || true
		event_instance_created=0
	fi
	if [ "$graph_instance_created" -eq 1 ]; then
		rmdir "$graph_instance" 2>/dev/null || true
		graph_instance_created=0
	fi
	if [ "$cooling_changed" -eq 1 ] && [ -n "$cooling_device" ]; then
		echo 0 >"$cooling_device/cur_state" 2>/dev/null || true
		cooling_changed=0
	fi
	if [ "$owner_started" -eq 1 ]; then
		"$tool" reset >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ "$cooling_state" -gt 0 ] || fail "cooling state must be nonzero"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
for command in cat date dmesg grep sleep taskset timeout; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done
set_state_function=$(grep -E '^cpufreq_set_cur_state(\.|$)' \
	"$trace_root/available_filter_functions" | head -n 1 || true)
[ -n "$set_state_function" ] || \
	fail "cpufreq_set_cur_state trace function is unavailable"
grep -qw freq_qos_update_request "$trace_root/available_filter_functions" || \
	fail "freq_qos_update_request trace function is unavailable"

for device in /sys/class/thermal/cooling_device*; do
	physical_node=$(readlink -f "$device/device/physical_node" 2>/dev/null || true)
	if [ "$(basename "$physical_node")" = "cpu$target_cpu" ]; then
		cooling_device=$device
		break
	fi
done
[ -n "$cooling_device" ] || \
	fail "no Processor cooling device maps to CPU $target_cpu"
[ "$(cat "$cooling_device/type")" = Processor ] || \
	fail "CPU cooling device is not a Processor device"
[ "$(cat "$cooling_device/cur_state")" = 0 ] || \
	fail "target CPU cooling state must initially be zero"
[ "$(cat "$cooling_device/max_state")" -ge "$cooling_state" ] || \
	fail "requested cooling state exceeds the device maximum"
[ -r "/sys/devices/system/cpu/cpu$target_cpu/cpufreq/scaling_max_freq" ] || \
	fail "target CPU frequency policy is unavailable"

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
printf '%s\n' "$set_state_function freq_qos_update_request" \
	>"$graph_instance/set_ftrace_filter"
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

original_max_freq=$(taskset -c "$work_cpu" cat \
	"/sys/devices/system/cpu/cpu$target_cpu/cpufreq/scaling_max_freq")
echo 1 >"$event_instance/tracing_on"
echo 1 >"$graph_instance/tracing_on"
start_ns=$(date +%s%N)
if timeout --kill-after=2s 10s taskset -c "$work_cpu" sh -c \
	'echo $$ >"$1/set_ftrace_pid"
	 printf "%s\n" "$2" >"$3"' \
	sh "$graph_instance" "$cooling_state" "$cooling_device/cur_state" \
	>"$outdir/cooling.log" 2>&1; then
	cooling_rc=0
else
	cooling_rc=$?
fi
end_ns=$(date +%s%N)
cooling_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
echo >"$graph_instance/set_ftrace_pid"
echo 0 >"$graph_instance/tracing_on"
cooling_changed=1
sleep 0.1
new_max_freq=$(taskset -c "$work_cpu" cat \
	"/sys/devices/system/cpu/cpu$target_cpu/cpufreq/scaling_max_freq")

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

[ "$cooling_rc" -eq 0 ] || fail "thermal cooling state change failed"
[ "$(cat "$cooling_device/cur_state")" = "$cooling_state" ] || \
	fail "thermal cooling state did not change"
[ "$new_max_freq" -lt "$original_max_freq" ] || \
	fail "thermal cooling did not lower the target CPU frequency ceiling"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "thermal cooling required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the thermal cooling update"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the thermal cooling update"
grep -q 'arch_ipi_count=0' "$outdir/ctl.log" || \
	fail "thermal cooling sent an architecture IPI to the owner"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the thermal cooling update"

grep -Eq "^[[:space:]]*$work_cpu\\).*cpufreq_set_cur_state" \
	"$outdir/function-graph.log" || \
	fail "cpufreq_set_cur_state was not traced on the work CPU"
grep -Eq "^[[:space:]]*$work_cpu\\).*freq_qos_update_request" \
	"$outdir/function-graph.log" || \
	fail "freq_qos_update_request was not traced on the work CPU"
stop_line=$(grep -E "owner_stop_request:.*cpu=$target_cpu " \
	"$outdir/owner-trace.log" | tail -n 1 || true)
exit_line=$(grep -E \
	"owner_exit_complete:.*cpu=$target_cpu .*stop_requested=1" \
	"$outdir/owner-trace.log" | tail -n 1 || true)
[ -n "$stop_line" ] || fail "owner-stop trace is missing"
[ -n "$exit_line" ] || fail "owner-exit trace is missing"
printf '%s\n' "$stop_line" | \
	grep -q 'caller=smp_call_function_many_cond' || \
	fail "thermal cooling did not use the waited call-function path"

stop_timestamp=$(printf '%s\n' "$stop_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v stop="$stop_timestamp" -v complete="$exit_timestamp" \
	'BEGIN { exit !(stop > 0 && complete > stop) }' || \
	fail "owner stop did not precede owner exit in the trace"

echo 0 >"$cooling_device/cur_state"
cooling_changed=0
[ "$(cat "$cooling_device/cur_state")" = 0 ] || \
	fail "failed to restore the original cooling state"

echo "cpu_accel thermal-cooling owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu state=0->$cooling_state->0"
echo "cooling_elapsed_ms=$cooling_elapsed_ms"
echo "max_freq=$original_max_freq->$new_max_freq"
echo "stop_line=$stop_line"
echo "exit_line=$exit_line"
