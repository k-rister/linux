#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-2}
hotplug_cpu=${CPU_ACCEL_HOTPLUG_CPU:-}
duration_ms=${CPU_ACCEL_DURATION_MS:-5000}
run_output=$(mktemp)
hotplug_output=$(mktemp)
hotplug_status=$(mktemp)
active_marker=$(mktemp)
trace_output=$(mktemp)
run_pid=
hotplug_pid=
hotplug_file=
hotplug_was_online=
trace_dir=
trace_probe_group=

fail()
{
	echo "cpu_accel inactive stop-machine test: $*" >&2
	exit 1
}

case "$duration_ms" in
	''|*[!0-9]*) fail "duration must be an integer number of milliseconds" ;;
esac
[ "$duration_ms" -gt 500 ] && [ "$duration_ms" -le 5000 ] || \
	fail "duration must be between 501 and 5000 milliseconds"
escape_after_ms=$((duration_ms - 500))
max_active_ns=$((duration_ms * 500000))

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$hotplug_pid" ]; then
		wait "$hotplug_pid" 2>/dev/null || true
		hotplug_pid=
	fi
	if [ "$hotplug_was_online" = 1 ] && [ -n "$hotplug_file" ] &&
	   [ -e "$hotplug_file" ] && [ "$(cat "$hotplug_file")" = 0 ]; then
		echo 1 >"$hotplug_file" 2>/dev/null || true
	fi
	if [ -n "$trace_dir" ] && [ -d "$trace_dir" ]; then
		echo 0 >"$trace_dir/tracing_on" 2>/dev/null || true
		echo nop >"$trace_dir/current_tracer" 2>/dev/null || true
		for event in begin begin_done try stop; do
			echo 0 >"$trace_dir/events/$trace_probe_group/$event/enable" \
				2>/dev/null || true
		done
		for event in owner_stop_request owner_exit_complete; do
			echo 0 >"$trace_dir/events/cpu_accel/$event/enable" \
				2>/dev/null || true
		done
		rmdir "$trace_dir" 2>/dev/null || true
	fi
	if [ -n "$trace_probe_group" ]; then
		printf '%s\n' "-:$trace_probe_group/begin" >>"$trace_root/kprobe_events" 2>/dev/null || true
		printf '%s\n' "-:$trace_probe_group/begin_done" >>"$trace_root/kprobe_events" 2>/dev/null || true
		printf '%s\n' "-:$trace_probe_group/try" >>"$trace_root/kprobe_events" 2>/dev/null || true
		printf '%s\n' "-:$trace_probe_group/stop" >>"$trace_root/kprobe_events" 2>/dev/null || true
	fi
	rm -f "$run_output" "$hotplug_output" "$hotplug_status" \
		"$active_marker" "$trace_output"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
command -v timeout >/dev/null 2>&1 || fail "timeout is required"

trace_root=/sys/kernel/tracing
[ -d "$trace_root" ] || trace_root=/sys/kernel/debug/tracing
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
[ -w "$trace_root/instances" ] || fail "tracefs instances are not writable"
[ -w "$trace_root/kprobe_events" ] || fail "tracefs kprobe events are unavailable"
trace_dir=$trace_root/instances/cpu-accel-stop-machine-$$
mkdir "$trace_dir" || fail "could not create a private trace instance"
echo 0 >"$trace_dir/tracing_on"
trace_probe_group=cpu_accel_stop_machine_$$
printf 'p:%s/begin x86_cpu_accel_cpu_up_maintenance_begin\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register CPU-up maintenance probe"
printf 'r:%s/begin_done x86_cpu_accel_cpu_up_maintenance_begin\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register CPU-up maintenance return probe"
printf 'r:%s/try x86_cpu_accel_maintenance_try_begin_inactive retval=$retval\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register accelerator-maintenance return probe"
printf 'r:%s/stop stop_machine_from_inactive_cpu retval=$retval\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register inactive-CPU stop-machine return probe"
[ -e "$trace_dir/events/$trace_probe_group/begin/enable" ] || \
	fail "CPU-up maintenance event is unavailable in trace instance"
[ -e "$trace_dir/events/$trace_probe_group/begin_done/enable" ] || \
	fail "CPU-up maintenance return event is unavailable in trace instance"
[ -e "$trace_dir/events/$trace_probe_group/try/enable" ] || \
	fail "accelerator-maintenance return event is unavailable in trace instance"
[ -e "$trace_dir/events/$trace_probe_group/stop/enable" ] || \
	fail "stop-machine return event is unavailable in trace instance"
echo 1 >"$trace_dir/events/$trace_probe_group/begin/enable"
echo 1 >"$trace_dir/events/$trace_probe_group/begin_done/enable"
echo 1 >"$trace_dir/events/$trace_probe_group/try/enable"
echo 1 >"$trace_dir/events/$trace_probe_group/stop/enable"
for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_dir/events/cpu_accel/$event/enable" ] || \
		fail "CPU accelerator $event tracepoint is unavailable"
	echo 1 >"$trace_dir/events/cpu_accel/$event/enable"
done

target_online=/sys/devices/system/cpu/cpu${target_cpu}/online
[ -e "$target_online" ] || fail "target CPU $target_cpu has no online control"
[ "$(cat "$target_online")" = 1 ] || fail "target CPU $target_cpu is offline"

if [ -z "$hotplug_cpu" ]; then
	for online in /sys/devices/system/cpu/cpu[1-9]*/online; do
		[ -e "$online" ] || continue
		cpu=${online%/online}
		cpu=${cpu##*cpu}
		if [ "$cpu" != "$target_cpu" ] && [ "$(cat "$online")" = 1 ]; then
			hotplug_cpu=$cpu
			break
		fi
	done
fi
[ -n "$hotplug_cpu" ] || fail "no online non-boot CPU is available for the rejection test"
[ "$hotplug_cpu" != "$target_cpu" ] || fail "hotplug CPU must differ from target CPU"
hotplug_file=/sys/devices/system/cpu/cpu${hotplug_cpu}/online
[ -e "$hotplug_file" ] || fail "CPU $hotplug_cpu has no online control"
hotplug_was_online=$(cat "$hotplug_file")
[ "$hotplug_was_online" = 1 ] || fail "CPU $hotplug_cpu must start online"

# Prepare the inactive CPU before entering the accelerator owner.
echo 0 >"$hotplug_file" || fail "could not offline test CPU $hotplug_cpu"
[ "$(cat "$hotplug_file")" = 0 ] || fail "test CPU $hotplug_cpu did not go offline"

rm -f "$active_marker"
CPU_ACCEL_ACTIVE_MARKER="$active_marker" \
	"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us 1000 --workload user-oslat \
	--escape-after-ms "$escape_after_ms" >"$run_output" 2>&1 &
run_pid=$!

# The control process publishes this marker only after the shared ABI reports
# ring-3 RUNNING state, accelerator mode, and a nonzero active-start timestamp.
active_wait=0
while [ ! -s "$active_marker" ]; do
	kill -0 "$run_pid" 2>/dev/null || {
		cat "$run_output"
		fail "owner run ended before entering ring 3"
	}
	[ "$active_wait" -lt 300 ] || {
		cat "$run_output"
		fail "owner did not enter ring 3 within 3 seconds"
	}
	sleep 0.01
	active_wait=$((active_wait + 1))
done
[ "$(cat "$active_marker")" = active ] || \
	fail "owner active marker has unexpected contents"

# CPU-up must request an NMI escape and drain the owner before taking the
# hotplug lock. Its later inactive-CPU stop-machine rendezvous is allowed only
# for this CPU-up window. The timed escape is a fallback if CPU-up fails to
# stop the owner, and the active-time bound below distinguishes the two cases.
echo >"$trace_dir/trace"
echo 1 >"$trace_dir/tracing_on"
rm -f "$hotplug_status"
hotplug_timeout_s=$((duration_ms / 1000 + 10))
(
	if timeout "${hotplug_timeout_s}s" sh -c 'echo 1 >"$1"' sh \
		"$hotplug_file"; then
		hotplug_rc=0
	else
		hotplug_rc=$?
	fi
	printf '%s\n' "$hotplug_rc" >"$hotplug_status"
	exit "$hotplug_rc"
) >"$hotplug_output" 2>&1 &
hotplug_pid=$!

# Wait until the hotplug thread reaches the maintenance gate. Keep the owner
# active long enough to prove CPU-up is waiting before it reaches AP startup.
request_wait=0
while ! grep -Eq 'begin:' "$trace_dir/trace"; do
	[ ! -s "$hotplug_status" ] || {
		cat "$trace_dir/trace" "$hotplug_output"
		fail "CPU-up completed without entering the maintenance gate"
	}
	[ "$request_wait" -lt 300 ] || {
		cat "$trace_dir/trace" "$hotplug_output"
		fail "CPU-up did not reach the maintenance gate"
	}
	sleep 0.01
	request_wait=$((request_wait + 1))
done
echo "cpu_accel: CPU-up entered maintenance while the ring-3 owner was active"

if ! wait "$run_pid"; then
	run_pid=
	cat "$run_output"
	fail "owner run failed"
fi
run_pid=
grep -q '^state=7 ' "$run_output" || \
	fail "maintenance did not escape the active ring-3 owner"
grep -q 'mode=0' "$run_output" || \
	fail "stopped owner did not return to Linux mode"
grep -q 'recovery_state=2 recovery_error=0' "$run_output" || \
	fail "owner did not recover cleanly after the NMI escape"
grep -q 'user_escape_count=1' "$run_output" || \
	fail "owner did not record exactly one NMI escape"
active_ns=$(sed -n 's/.*user_active_ns=\([0-9][0-9]*\).*/\1/p' "$run_output")
[ -n "$active_ns" ] || fail "owner run did not report ring-3 active time"
[ "$active_ns" -lt "$max_active_ns" ] || \
	fail "CPU-up did not stop the owner before the timed escape fallback"

# Once ownership ends, the same CPU-up request should succeed.
if wait "$hotplug_pid"; then
	hotplug_rc=0
else
	hotplug_rc=$?
fi
hotplug_pid=
[ -s "$hotplug_status" ] || fail "CPU-up request ended without recording its result"
[ "$(cat "$hotplug_status")" = 0 ] || {
	cat "$trace_dir/trace" "$hotplug_output"
	fail "CPU-up request failed after owner exit"
}
[ "$hotplug_rc" -eq 0 ] || fail "CPU-up request timed out or failed (status $hotplug_rc)"
[ "$(cat "$hotplug_file")" = 1 ] || \
	fail "CPU $hotplug_cpu remained offline after owner exit"

echo 0 >"$trace_dir/tracing_on"
cat "$trace_dir/trace" >"$trace_output"
awk '
/begin:/ && !begin { begin = NR }
/owner_stop_request/ && !stop_request { stop_request = NR }
/owner_exit_complete/ && !owner_exit { owner_exit = NR }
/begin_done:/ && !begin_done { begin_done = NR }
/try:/ && !try_begin { try_begin = NR }
/stop:/ && !stop_done { stop_done = NR }
END {
	if (!(begin && stop_request && owner_exit && begin_done && try_begin &&
	      stop_done && begin < stop_request && stop_request < owner_exit &&
	      owner_exit < begin_done && begin_done < try_begin &&
	      try_begin < stop_done))
		exit 1
}' "$trace_output" || {
	cat "$trace_output"
	fail "trace did not prove owner exit before CPU-up rendezvous"
}
grep -Eq 'try:.*<- x86_cpu_accel_maintenance_try_begin_inactive.*retval=0x1([[:space:]]|$)' \
	"$trace_output" || {
	cat "$trace_output"
	fail "CPU-up inactive-CPU reservation did not succeed after owner exit"
}
grep -Eq 'stop:.*<- stop_machine_from_inactive_cpu.*retval=0x0+([[:space:]]|$)' \
	"$trace_output" || {
	cat "$trace_output"
	fail "inactive-CPU stop-machine did not complete successfully after owner exit"
}

cat "$run_output"
cat "$trace_output"
echo "cpu_accel: owner exited before CPU-up rendezvous; CPU online succeeded"
