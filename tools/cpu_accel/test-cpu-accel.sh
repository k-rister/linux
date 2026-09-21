#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-1}
online_file=/sys/devices/system/cpu/cpu${target_cpu}/online
stop_output=$(mktemp)
invalid_output=$(mktemp)

fail()
{
	echo "cpu_accel: $*" >&2
	exit 1
}

cleanup()
{
	rm -f "$stop_output" "$invalid_output"
}
trap cleanup EXIT

[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"

output=$($tool run --cpu "$target_cpu" --duration-ms 20 --period-us 1000)
echo "$output"
case "$output" in
	state=3\ *) ;;
	*) fail "normal run did not complete" ;;
esac

$tool run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--persistent >"$stop_output" 2>&1 &
run_pid=$!
sleep 0.1
[ "$(cat "$online_file")" = 1 ] || fail "target CPU went offline during accelerator run"
kill -TERM "$run_pid"
wait "$run_pid"
cat "$stop_output"
grep -q '^state=4 ' "$stop_output" || fail "STOP did not produce state=4"

output=$($tool run --cpu "$target_cpu" --duration-ms 100 \
	--period-us 1000 --persistent)
echo "$output"
case "$output" in
	state=6\ *) ;;
	*) fail "watchdog run did not produce state=6" ;;
esac

if $tool run --cpu 0 --duration-ms 10 --period-us 1000 \
	>"$invalid_output" 2>&1; then
	fail "CPU 0 was accepted as an accelerator target"
fi
cat "$invalid_output"

[ "$(cat "$online_file")" = 1 ] || fail "target CPU is not online after accelerator run"
echo "cpu_accel: lifecycle, normal, STOP, watchdog, invalid-target, and online-state tests passed"
