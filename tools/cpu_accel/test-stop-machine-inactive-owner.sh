#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-2}
hotplug_cpu=${CPU_ACCEL_HOTPLUG_CPU:-}
duration_ms=${CPU_ACCEL_DURATION_MS:-5000}
run_output=$(mktemp)
hotplug_output=$(mktemp)
active_marker=$(mktemp)
run_pid=
hotplug_file=
hotplug_was_online=

fail()
{
	echo "cpu_accel inactive stop-machine test: $*" >&2
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ "$hotplug_was_online" = 1 ] && [ -n "$hotplug_file" ] &&
	   [ -e "$hotplug_file" ] && [ "$(cat "$hotplug_file")" = 0 ]; then
		echo 1 >"$hotplug_file" 2>/dev/null || true
	fi
	rm -f "$run_output" "$hotplug_output" "$active_marker"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
command -v timeout >/dev/null 2>&1 || fail "timeout is required"

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
	--period-us 1000 --workload user-oslat >"$run_output" 2>&1 &
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

# CPU startup cannot sleep waiting for an owner. Its stop-machine try gate
# must reject the operation with -EBUSY and leave the CPU offline.
if timeout 4s sh -c 'echo 1 >"$1"' sh "$hotplug_file" \
	>"$hotplug_output" 2>&1; then
	cat "$active_marker" "$run_output" "$hotplug_output"
	fail "inactive-CPU stop-machine request succeeded while an owner was active"
else
	hotplug_rc=$?
fi
[ "$hotplug_rc" -ne 124 ] || fail "inactive-CPU request timed out instead of rejecting"
grep -Eiq 'busy|temporarily unavailable' "$hotplug_output" || {
	cat "$hotplug_output"
	fail "inactive-CPU request failed for a reason other than -EBUSY"
}
[ "$(cat "$hotplug_file")" = 0 ] || \
	fail "test CPU came online despite the active owner"
kill -0 "$run_pid" 2>/dev/null || \
	fail "owner run ended before the busy rejection was verified"

if ! wait "$run_pid"; then
	run_pid=
	cat "$run_output"
	fail "owner run failed"
fi
run_pid=
grep -q '^state=3 ' "$run_output" || fail "owner run did not complete"
grep -q 'user_active_ns=[1-9][0-9]*' "$run_output" || \
	fail "owner run did not enter ring 3"

# Once ownership ends, the same CPU-up request should succeed.
echo 1 >"$hotplug_file" || fail "CPU $hotplug_cpu did not come online after owner exit"
[ "$(cat "$hotplug_file")" = 1 ] || \
	fail "CPU $hotplug_cpu remained offline after owner exit"

cat "$run_output"
echo "cpu_accel: inactive-CPU stop-machine rejected with -EBUSY while owner active; CPU online succeeded after owner exit"
