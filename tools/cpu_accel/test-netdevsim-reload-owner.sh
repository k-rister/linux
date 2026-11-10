#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_NETDEVSIM_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_NETDEVSIM_DURATION_MS:-5000}
netdevsim_id=${CPU_ACCEL_NETDEVSIM_ID:-$$}
trace_root=${TRACEFS:-/sys/kernel/tracing}
event_instance="$trace_root/instances/cpu_accel_netdevsim_events_$$"
graph_instance="$trace_root/instances/cpu_accel_netdevsim_graph_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-netdevsim.XXXXXX)}
mkdir -p "$outdir"
run_pid=
event_trace_pid=
graph_trace_pid=
dmesg_pid=
device_created=0
owner_started=0

fail()
{
	echo "cpu_accel netdevsim-reload owner test: $* (logs: $outdir)" >&2
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
		if [ -d "$event_instance" ]; then
			echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		fi
		kill -TERM "$event_trace_pid" 2>/dev/null || true
		wait "$event_trace_pid" 2>/dev/null || true
		event_trace_pid=
	fi
	if [ -n "$graph_trace_pid" ]; then
		if [ -d "$graph_instance" ]; then
			echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
			echo >"$graph_instance/set_ftrace_pid" 2>/dev/null || true
			echo >"$graph_instance/set_ftrace_filter" 2>/dev/null || true
			echo nop >"$graph_instance/current_tracer" 2>/dev/null || true
		fi
		kill -TERM "$graph_trace_pid" 2>/dev/null || true
		wait "$graph_trace_pid" 2>/dev/null || true
		graph_trace_pid=
	fi
	if [ -d "$event_instance" ]; then
		rmdir "$event_instance" 2>/dev/null || true
	fi
	if [ -d "$graph_instance" ]; then
		rmdir "$graph_instance" 2>/dev/null || true
	fi
	if [ "$device_created" -eq 1 ]; then
		echo "$netdevsim_id" >/sys/bus/netdevsim/del_device \
			2>/dev/null || true
		device_created=0
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
[ -w /sys/bus/netdevsim/new_device ] || \
	fail "netdevsim new_device control is unavailable"
[ -w /sys/bus/netdevsim/del_device ] || \
	fail "netdevsim del_device control is unavailable"
[ ! -e "/sys/bus/netdevsim/devices/netdevsim$netdevsim_id" ] || \
	fail "temporary netdevsim ID already exists: $netdevsim_id"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
for command in awk dmesg devlink grep modprobe taskset timeout; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
for symbol in nsim_dev_reload_down nsim_dev_reload_up \
	unregister_netdevice register_netdevice; do
	grep -Eq " $symbol([[:space:]]*\[netdevsim\])?$" /proc/kallsyms || \
		fail "required symbol is unavailable: $symbol"
done
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

modprobe netdevsim || fail "netdevsim module is unavailable"
echo "$netdevsim_id 1" >/sys/bus/netdevsim/new_device || \
	fail "could not create temporary netdevsim device"
device_created=1
netdevsim_sysfs="/sys/bus/netdevsim/devices/netdevsim$netdevsim_id"
netdevsim_debugfs="/sys/kernel/debug/netdevsim/netdevsim$netdevsim_id"
[ -d "$netdevsim_sysfs/net" ] || fail "temporary netdevsim port was not created"
[ -w "$netdevsim_debugfs/fail_reload" ] || \
	fail "netdevsim fail_reload control is unavailable"
echo y >"$netdevsim_debugfs/fail_reload"
graph_filter='nsim_dev_reload_down nsim_dev_reload_up '\
'unregister_netdevice register_netdevice'

mkdir "$event_instance"
echo 4096 >"$event_instance/buffer_size_kb"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	echo 1 >"$event_instance/events/$event/enable"
done
echo 0 >"$event_instance/tracing_on"
cat "$event_instance/trace_pipe" >"$outdir/owner-trace.log" &
event_trace_pid=$!

mkdir "$graph_instance"
echo 4096 >"$graph_instance/buffer_size_kb"
echo >"$graph_instance/set_ftrace_pid"
printf '%s\n' "$graph_filter" >"$graph_instance/set_ftrace_filter"
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

echo 1 >"$event_instance/tracing_on"
echo 1 >"$graph_instance/tracing_on"
fault_start_ns=$(date +%s%N)
if timeout --kill-after=2s 10s taskset -c "$work_cpu" sh -c \
	'echo $$ >"$1/set_ftrace_pid"
	 exec devlink dev reload "$2"' \
	sh "$graph_instance" "netdevsim/netdevsim$netdevsim_id" \
	>"$outdir/reload-fault.log" 2>&1; then
	fault_rc=0
else
	fault_rc=$?
fi
fault_end_ns=$(date +%s%N)
fault_elapsed_ms=$(((fault_end_ns - fault_start_ns) / 1000000))
echo >"$graph_instance/set_ftrace_pid"

echo n >"$netdevsim_debugfs/fail_reload"

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=

reload_start_ns=$(date +%s%N)
if timeout --kill-after=2s 10s taskset -c "$work_cpu" sh -c \
	'echo $$ >"$1/set_ftrace_pid"
	 exec devlink dev reload "$2"' \
	sh "$graph_instance" "netdevsim/netdevsim$netdevsim_id" \
	>"$outdir/reload-success.log" 2>&1; then
	reload_rc=0
else
	reload_rc=$?
fi
reload_end_ns=$(date +%s%N)
reload_elapsed_ms=$(((reload_end_ns - reload_start_ns) / 1000000))
echo >"$graph_instance/set_ftrace_pid"
echo 0 >"$graph_instance/tracing_on"
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

[ "$fault_rc" -ne 0 ] || fail "netdevsim reload unexpectedly succeeded"
[ "$reload_rc" -eq 0 ] || fail "netdevsim recovery reload failed"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ -d "$netdevsim_sysfs/net" ] || fail "netdevsim port was not recreated"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'mode=0' "$outdir/ctl.log" || \
	fail "owner did not return to Linux mode"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "netdevsim reload fault required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during netdevsim reload fault"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during netdevsim reload fault"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during netdevsim reload fault"

for symbol in nsim_dev_reload_down nsim_dev_reload_up \
	unregister_netdevice register_netdevice; do
	grep -Eq "^[[:space:]]*$work_cpu\\).*$symbol" \
		"$outdir/function-graph.log" || \
		fail "$symbol was not traced on the work CPU"
done

stop_line=$(grep -E "owner_stop_request:.*cpu=$target_cpu " \
	"$outdir/owner-trace.log" | tail -n 1 || true)
exit_line=$(grep -E \
	"owner_exit_complete:.*cpu=$target_cpu .*stop_requested=1" \
	"$outdir/owner-trace.log" | tail -n 1 || true)
if [ -n "$stop_line" ]; then
	[ -n "$exit_line" ] || fail "owner-exit trace is missing"
	stop_timestamp=$(printf '%s\n' "$stop_line" | \
		awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
	exit_timestamp=$(printf '%s\n' "$exit_line" | \
		awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
	awk -v stop="$stop_timestamp" -v complete="$exit_timestamp" \
		'BEGIN { exit !(stop > 0 && complete > stop) }' || \
		fail "owner stop did not precede owner exit in the trace"
fi

echo "cpu_accel netdevsim-reload owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu netdevsim_id=$netdevsim_id"
echo "fault_elapsed_ms=$fault_elapsed_ms fault_rc=$fault_rc"
echo "reload_elapsed_ms=$reload_elapsed_ms reload_rc=$reload_rc"
if [ -n "$stop_line" ]; then
	echo "stop_line=$stop_line"
	echo "exit_line=$exit_line"
else
	echo "owner_stop_request=none"
fi
