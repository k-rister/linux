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
quiescent_arg=
quarantine_arg=
escape_retries=${CPU_ACCEL_ESCAPE_RETRIES:-1}

if [ "${CPU_ACCEL_REQUIRE_QUIESCENT:-0}" = 1 ]; then
	quiescent_arg=--require-quiescent
fi
if [ "${CPU_ACCEL_QUARANTINE_IRQS:-0}" = 1 ]; then
	quarantine_arg=--quarantine-irqs
fi

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
	output=$($tool run --cpu "$target_cpu" --duration-ms 20 --period-us 1000 \
		$quiescent_arg $quarantine_arg)
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
	echo "$output" | grep -q 'workqueue_executed=0' || \
		fail "workqueue executed on the accelerator CPU"
	if [ -n "$quarantine_arg" ]; then
		echo "$output" | grep -q 'irq_quarantined=[1-9][0-9]*' || \
			fail "IRQ quarantine did not move any IRQs"
		echo "$output" | grep -q 'irq_quarantine_blockers=0' || \
			fail "IRQ quarantine reported blockers"
	fi
	echo "$output" | grep -q 'backend=3' || \
		fail "x86 direct-APIC backend was not selected"
	echo "$output" | grep -q 'arch_counters_valid=1' || \
		fail "architecture counters were not reported on x86"
	run=$((run + 1))
done

memmove_output=$($tool run --cpu "$target_cpu" --duration-ms 20 \
	--period-us 1000 --workload memmove --work-bytes 4096 \
	$quiescent_arg $quarantine_arg)
echo "$memmove_output"
case "$memmove_output" in
	state=3\ *) ;;
	*) fail "memmove run did not complete" ;;
esac
echo "$memmove_output" | grep -q 'mode=0' || \
	fail "memmove run did not return to Linux mode"
echo "$memmove_output" | grep -q 'workload=1' || \
	fail "memmove workload was not selected"
echo "$memmove_output" | grep -q 'work_bytes=4096' || \
	fail "memmove workload size was not reported"
echo "$memmove_output" | grep -q 'work_iterations=[1-9][0-9]*' || \
	fail "memmove workload did not execute"
echo "$memmove_output" | grep -q 'context_switches=0' || \
	fail "memmove workload performed a context switch"
echo "$memmove_output" | grep -q 'migration_detected=0' || \
	fail "memmove workload migrated CPUs"

shared_output=$($tool run --cpu "$target_cpu" --duration-ms 20 \
	--period-us 1000 --workload shared-memmove --work-bytes 4096 \
	$quiescent_arg $quarantine_arg)
echo "$shared_output"
case "$shared_output" in
	state=3\ *) ;;
	*) fail "shared memmove run did not complete" ;;
esac
echo "$shared_output" | grep -q 'mode=0' || \
	fail "shared memmove run did not return to Linux mode"
echo "$shared_output" | grep -q 'workload=2' || \
	fail "shared memmove workload was not selected"
echo "$shared_output" | grep -q 'work_bytes=4096' || \
	fail "shared memmove workload size was not reported"
echo "$shared_output" | grep -q 'work_iterations=[1-9][0-9]*' || \
	fail "shared memmove workload did not execute"
echo "$shared_output" | grep -q 'shared_entry=0' || \
	fail "shared memmove entry was not reported"
echo "$shared_output" | grep -q 'shared_owner=3' || \
	fail "shared memmove did not retain COMPLETE ownership"
echo "$shared_output" | grep -q 'shared_epoch=[1-9][0-9]*' || \
	fail "shared memmove epoch was not reported"
echo "$shared_output" | grep -q 'context_switches=0' || \
	fail "shared memmove workload performed a context switch"
echo "$shared_output" | grep -q 'migration_detected=0' || \
	fail "shared memmove workload migrated CPUs"

case "$(uname -m)" in
x86_64)
	user_run=1
	while [ "$user_run" -le "$repeats" ]; do
		user_output=$($tool run --cpu "$target_cpu" --duration-ms 20 \
			--period-us 1000 --workload user-oslat \
			$quiescent_arg $quarantine_arg)
		echo "$user_output"
		case "$user_output" in
			state=3\ *) ;;
			*) fail "user-oslat run did not complete" ;;
		esac
		echo "$user_output" | grep -q 'mode=0' || \
			fail "user-oslat run did not return to Linux mode"
		echo "$user_output" | grep -q 'backend=2' || \
			fail "x86 ring-3 backend was not selected"
		echo "$user_output" | grep -q 'workload=3' || \
			fail "user-oslat workload was not selected"
		echo "$user_output" | grep -q 'samples=[1-9][0-9]*' || \
			fail "user-oslat workload did not produce samples"
		echo "$user_output" | grep -q 'user_active_start_ns=[1-9][0-9]*' || \
			fail "user-oslat active start timestamp was not recorded"
		echo "$user_output" | grep -q 'user_active_end_ns=[1-9][0-9]*' || \
			fail "user-oslat active end timestamp was not recorded"
		echo "$user_output" | grep -q 'user_active_ns=[1-9][0-9]*' || \
			fail "user-oslat active interval was not recorded"
		echo "$user_output" | grep -q 'context_switches=0' || \
			fail "user-oslat workload performed a context switch"
		echo "$user_output" | grep -q 'migration_detected=0' || \
			fail "user-oslat workload migrated CPUs"
		user_run=$((user_run + 1))
	done
	escape_output=$($tool run --cpu "$target_cpu" --duration-ms 5000 \
		--period-us 1000 --workload user-oslat --escape-after-ms 500 \
		--escape-retries "$escape_retries" \
		$quiescent_arg $quarantine_arg)
	echo "$escape_output"
	case "$escape_output" in
		state=7\ *) ;;
		*) fail "user-oslat escape run did not complete" ;;
	esac
	echo "$escape_output" | grep -q 'mode=0' || \
		fail "user-oslat escape did not return to Linux mode"
	echo "$escape_output" | grep -q 'backend=2' || \
		fail "user-oslat escape selected the wrong backend"
	echo "$escape_output" | grep -q 'user_escape_count=1' || \
		fail "user-oslat escape was not observed exactly once"
	echo "$escape_output" | grep -q 'recovery_state=2' || \
		fail "user-oslat escape did not report successful recovery"
	echo "$escape_output" | grep -q 'recovery_attempts=[1-9][0-9]*' || \
		fail "user-oslat escape attempts were not reported"
	echo "$escape_output" | grep -q 'user_active_ns=[1-9][0-9]*' || \
		fail "user-oslat escape did not report an active interval"
	echo "$escape_output" | grep -q 'context_switches=0' || \
		fail "user-oslat escape performed a context switch"
	echo "$escape_output" | grep -q 'migration_detected=0' || \
		fail "user-oslat escape migrated CPUs"
	hang_output=$($tool run --cpu "$target_cpu" --duration-ms 5000 \
		--period-us 1000 --workload user-hang --escape-after-ms 500 \
		--escape-retries "$escape_retries" \
		$quiescent_arg $quarantine_arg)
	echo "$hang_output"
	case "$hang_output" in
		state=7\ *) ;;
		*) fail "user-hang escape did not complete" ;;
	esac
	echo "$hang_output" | grep -q 'mode=0' || \
		fail "user-hang escape did not return to Linux mode"
	echo "$hang_output" | grep -q 'backend=2' || \
		fail "user-hang selected the wrong backend"
	echo "$hang_output" | grep -q 'workload=4' || \
		fail "user-hang workload was not selected"
	echo "$hang_output" | grep -q 'user_escape_count=1' || \
		fail "user-hang escape was not observed exactly once"
	echo "$hang_output" | grep -q 'recovery_state=2' || \
		fail "user-hang escape did not report successful recovery"
	echo "$hang_output" | grep -q 'recovery_attempts=[1-9][0-9]*' || \
		fail "user-hang escape attempts were not reported"
	echo "$hang_output" | grep -q 'context_switches=0' || \
		fail "user-hang escape performed a context switch"
	echo "$hang_output" | grep -q 'migration_detected=0' || \
		fail "user-hang escape migrated CPUs"
	;;
esac

$tool run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--persistent $quiescent_arg $quarantine_arg >"$stop_output" 2>&1 &
run_pid=$!
sleep 0.1
[ "$(cat "$online_file")" = 1 ] || fail "target CPU went offline during accelerator run"
kill -TERM "$run_pid" 2>/dev/null || true
wait "$run_pid"
cat "$stop_output"
grep -q '^state=4 ' "$stop_output" || fail "STOP did not produce state=4"
grep -q 'mode=0' "$stop_output" || fail "STOP did not return to Linux mode"

output=$($tool run --cpu "$target_cpu" --duration-ms 100 \
	--period-us 1000 --persistent $quiescent_arg $quarantine_arg)
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
