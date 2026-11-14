#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCEL_TESTCTL:-./cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-2}
duration_ms=${CPU_ACCEL_DURATION_MS:-3000}
run_output=$(mktemp)
active_marker=$(mktemp)
run_pid=
target_offline=0

fail()
{
	echo "cpu_accel CPU-down owner test: $*" >&2
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ "$target_offline" -eq 1 ]; then
		echo 1 >"/sys/devices/system/cpu/cpu$target_cpu/online" \
			2>/dev/null || true
	fi
	rm -f "$run_output" "$active_marker"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
command -v taskset >/dev/null 2>&1 || fail "taskset is required"

case "$duration_ms" in
	''|*[!0-9]*) fail "duration must be an integer number of milliseconds" ;;
esac
[ "$duration_ms" -ge 2000 ] && [ "$duration_ms" -le 5000 ] || \
	fail "duration must be between 2000 and 5000 milliseconds"

target_online="/sys/devices/system/cpu/cpu$target_cpu/online"
[ -e "$target_online" ] || fail "target CPU $target_cpu has no online control"
[ "$(cat "$target_online")" = 1 ] || fail "target CPU $target_cpu is offline"

rm -f "$active_marker"
CPU_ACCEL_ACTIVE_MARKER="$active_marker" \
	"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us 1000 --workload user-cyclictest >"$run_output" 2>&1 &
run_pid=$!

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

hotplug_start_ns=$(date +%s%N)
hotplug_timeout_s=$((duration_ms / 1000 + 10))
target_offline=1
if ! timeout --kill-after=5s "${hotplug_timeout_s}s" \
	taskset -c 0 sh -c 'echo 0 > "$1"' sh "$target_online"; then
	wait "$run_pid" 2>/dev/null || true
	run_pid=
	cat "$run_output"
	fail "CPU offlining failed or timed out during accelerator ownership"
fi
hotplug_end_ns=$(date +%s%N)

if ! wait "$run_pid"; then
	run_pid=
	cat "$run_output"
	fail "owner run failed while CPU offlining waited"
fi
run_pid=

cat "$run_output"
grep -q '^state=3 ' "$run_output" || \
	fail "CPU-down owner run did not complete"
grep -q 'mode=0' "$run_output" || \
	fail "CPU-down owner run did not return to Linux mode"
grep -q 'user_active_ns=[1-9][0-9]*' "$run_output" || \
	fail "CPU-down owner run did not enter ring 3"

hotplug_wait_ms=$(((hotplug_end_ns - hotplug_start_ns) / 1000000))
[ "$hotplug_wait_ms" -ge 1000 ] || \
	fail "CPU offlining did not wait for the active owner"
[ "$(cat "$target_online")" = 0 ] || \
	fail "CPU offlining did not take the target CPU offline"

echo 1 >"$target_online"
target_offline=0
[ "$(cat "$target_online")" = 1 ] || \
	fail "target CPU did not return online after CPU-down test"

echo "hotplug_wait_ms=$hotplug_wait_ms"
echo "cpu_accel: owner drained before CPU offlining (${hotplug_wait_ms} ms)"
