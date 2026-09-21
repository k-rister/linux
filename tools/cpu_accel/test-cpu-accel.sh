#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-1}
online_file=/sys/devices/system/cpu/cpu${target_cpu}/online
stop_output=$(mktemp)
invalid_output=$(mktemp)
load_pid=0
repeats=${CPU_ACCEL_REPEATS:-1}
load_cpus=${CPU_ACCEL_LOAD_CPUS:-}

fail()
{
	echo "cpu_accel: $*" >&2
	exit 1
}

cleanup()
{
	if [ "$load_pid" -ne 0 ]; then
		kill "$load_pid" 2>/dev/null || true
	fi
	rm -f "$stop_output" "$invalid_output"
}
trap cleanup EXIT

[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"

case "$repeats" in
	''|*[!0-9]*|0) fail "CPU_ACCEL_REPEATS must be a positive integer" ;;
esac

if [ -n "$load_cpus" ]; then
	command -v taskset >/dev/null 2>&1 || fail "taskset is required for CPU load"
	taskset -c "$load_cpus" sh -c 'while :; do :; done' \
		>/dev/null 2>&1 &
	load_pid=$!
fi

run=1
while [ "$run" -le "$repeats" ]; do
	output=$($tool run --cpu "$target_cpu" --duration-ms 20 --period-us 1000)
	echo "$output"
	case "$output" in
		state=3\ *) ;;
		*) fail "normal run did not complete" ;;
	esac
	echo "$output" | grep -q 'mode=0' || \
		fail "normal run did not return to Linux mode"
	echo "$output" | grep -q 'lifecycle_entry_ns=[1-9][0-9]*' || \
		fail "lifecycle entry timestamp was not recorded"
	echo "$output" | grep -q 'lifecycle_exit_ns=[1-9][0-9]*' || \
		fail "lifecycle exit timestamp was not recorded"
	echo "$output" | grep -q 'context_switches=0' || \
		fail "accelerator workload performed a context switch"
	echo "$output" | grep -q 'migration_detected=0' || \
		fail "accelerator workload migrated CPUs"
	echo "$output" | grep -q 'timer_softirq_count=' || \
		fail "timer softirq telemetry was not reported"
	echo "$output" | grep -q 'rcu_softirq_count=' || \
		fail "RCU softirq telemetry was not reported"
	echo "$output" | grep -q 'workqueue_queued=' || \
		fail "workqueue queue telemetry was not reported"
	echo "$output" | grep -q 'workqueue_executed=' || \
		fail "workqueue execution telemetry was not reported"
	echo "$output" | grep -q 'backend=1' || \
		fail "x86 staged backend was not selected"
	echo "$output" | grep -q 'arch_counters_valid=1' || \
		fail "architecture counters were not reported on x86"
	run=$((run + 1))
done

$tool run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--persistent >"$stop_output" 2>&1 &
run_pid=$!
sleep 0.1
[ "$(cat "$online_file")" = 1 ] || fail "target CPU went offline during accelerator run"
kill -TERM "$run_pid"
wait "$run_pid"
cat "$stop_output"
grep -q '^state=4 ' "$stop_output" || fail "STOP did not produce state=4"
grep -q 'mode=0' "$stop_output" || fail "STOP did not return to Linux mode"

output=$($tool run --cpu "$target_cpu" --duration-ms 100 \
	--period-us 1000 --persistent)
echo "$output"
case "$output" in
	state=6\ *) ;;
	*) fail "watchdog run did not produce state=6" ;;
esac
echo "$output" | grep -q 'mode=0' || \
	fail "watchdog did not return to Linux mode"

if $tool run --cpu 0 --duration-ms 10 --period-us 1000 \
	>"$invalid_output" 2>&1; then
	fail "CPU 0 was accepted as an accelerator target"
fi
cat "$invalid_output"

[ "$(cat "$online_file")" = 1 ] || fail "target CPU is not online after accelerator run"
echo "cpu_accel: lifecycle, normal x${repeats}, STOP, watchdog, invalid-target, and online-state tests passed"
