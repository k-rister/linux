#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpus=${CPU_ACCEL_PHASE10_CPUS:-5,7}
duration_ms=${CPU_ACCEL_PHASE10_DURATION_MS:-1000}
period_us=${CPU_ACCEL_PHASE10_PERIOD_US:-1000}
join_delay_ms=$((duration_ms + 100))
normal_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)
failure_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)
pipeline_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)
pipeline_failure_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)

fail()
{
	echo "cpu_accel phase10 SMP gate: $*" >&2
	for file in "$normal_output" "$failure_output" "$pipeline_output" \
		   "$pipeline_failure_output"; do
		[ ! -f "$file" ] || cat "$file" >&2
	done
	rm -f "$normal_output" "$failure_output" "$pipeline_output" \
	       "$pipeline_failure_output"
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	rm -f "$normal_output" "$failure_output" "$pipeline_output" \
	       "$pipeline_failure_output"
	exit "$status"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
for command in grep sed; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done

worker_count=$(printf '%s\n' "$target_cpus" | tr ',' '\n' | wc -l)
[ "$worker_count" -ge 2 ] && [ "$worker_count" -le 16 ] || \
	fail "CPU list must contain 2 to 16 CPUs"
case ",$target_cpus," in
	*,0,*) fail "CPU 0 must remain in Linux" ;;
esac
for cpu in $(printf '%s\n' "$target_cpus" | tr ',' ' '); do
	[ "$(cat "/sys/devices/system/cpu/cpu$cpu/online")" = 1 ] || \
		fail "target CPU $cpu is offline"
done

"$tool" run --cpus "$target_cpus" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-oslat \
	--smp-join-delay-ms "$join_delay_ms" \
	>"$normal_output" 2>&1 || \
	fail "multi-CPU user-oslat run failed"
grep -q '^state=0 mode=0 ' "$normal_output" || \
	fail "multi-CPU run did not reset to Linux mode"
worker_lines=$(grep -c '^worker=' "$normal_output" || true)
[ "$worker_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count healthy worker records, found $worker_lines"
successful_lines=$(grep -c '^worker=.* state=3 result=0 ' \
	"$normal_output" || true)
[ "$successful_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count successful workers, found $successful_lines"
for pattern in 'context_switches=0' 'migration_detected=0' \
	       'user_escape_count=0'; do
	matching_lines=$(grep -c "^worker=.* $pattern" "$normal_output" || true)
	[ "$matching_lines" -eq "$worker_count" ] || \
		fail "healthy worker telemetry mismatch for $pattern"
done
for name in samples hist_total; do
	values=$(grep '^worker=.* state=3 result=0 ' "$normal_output" |
		sed -n "s/.*[[:space:]]$name=\([0-9][0-9]*\).*/\1/p")
	value_count=0
	for value in $values; do
		value_count=$((value_count + 1))
		[ "$value" -gt 0 ] || \
			fail "healthy worker telemetry did not report $name"
	done
	[ "$value_count" -eq "$worker_count" ] || \
		fail "expected $worker_count healthy $name records"
done

"$tool" run --cpus "$target_cpus" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-pipeline \
	--queue-capacity 1 >"$pipeline_output" 2>&1 || \
	fail "multi-CPU user-pipeline run failed"
grep -q '^state=0 mode=0 ' "$pipeline_output" || \
	fail "user-pipeline run did not reset to Linux mode"
worker_lines=$(grep -c '^worker=' "$pipeline_output" || true)
[ "$worker_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count pipeline worker records, found $worker_lines"
successful_lines=$(grep -c '^worker=.* state=3 result=0 ' \
	"$pipeline_output" || true)
[ "$successful_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count successful pipeline workers, found $successful_lines"
queue_count=$((worker_count - 1))
queue_lines=$(grep -c '^queue=' "$pipeline_output" || true)
[ "$queue_lines" -eq "$queue_count" ] || \
	fail "expected $queue_count pipeline queue records, found $queue_lines"
queue_pressure=0
queue_index=0
while [ "$queue_index" -lt "$queue_count" ]; do
	queue_line=$(grep "^queue=$queue_index " "$pipeline_output") || \
		fail "missing pipeline queue record $queue_index"
	enqueued=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]enqueued=\([0-9][0-9]*\).*/\1/p')
	dequeued=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]dequeued=\([0-9][0-9]*\).*/\1/p')
	full_events=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]full_events=\([0-9][0-9]*\).*/\1/p')
	empty_events=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]empty_events=\([0-9][0-9]*\).*/\1/p')
	backlog=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]backlog=\([0-9][0-9]*\).*/\1/p')
	producer_checksum=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]producer_checksum=\([0-9][0-9]*\).*/\1/p')
	consumer_checksum=$(printf '%s\n' "$queue_line" |
		sed -n 's/.*[[:space:]]consumer_checksum=\([0-9][0-9]*\).*/\1/p')
	[ -n "$enqueued" ] && [ "$enqueued" -gt 0 ] || \
		fail "pipeline queue $queue_index did not enqueue records"
	[ "$enqueued" -eq "$dequeued" ] || \
		fail "pipeline queue $queue_index enqueued $enqueued but dequeued $dequeued"
	[ "$backlog" -eq 0 ] || \
		fail "pipeline queue $queue_index ended with backlog $backlog"
	[ "$producer_checksum" -eq "$consumer_checksum" ] || \
		fail "pipeline queue $queue_index checksum mismatch"
	queue_pressure=$((queue_pressure + full_events + empty_events))
	queue_index=$((queue_index + 1))
done
[ "$queue_pressure" -gt 0 ] || \
	fail "pipeline queues reported no full or empty events"
for pattern in 'context_switches=0' 'migration_detected=0' \
	       'user_escape_count=0'; do
	matching_lines=$(grep -c "^worker=.* $pattern" "$pipeline_output" || true)
	[ "$matching_lines" -eq "$worker_count" ] || \
		fail "pipeline worker telemetry mismatch for $pattern"
done
for name in samples hist_total; do
	values=$(grep '^worker=.* state=3 result=0 ' "$pipeline_output" |
		sed -n "s/.*[[:space:]]$name=\([0-9][0-9]*\).*/\1/p")
	value_count=0
	for value in $values; do
		value_count=$((value_count + 1))
		[ "$value" -gt 0 ] || \
			fail "pipeline worker telemetry did not report $name"
	done
	[ "$value_count" -eq "$worker_count" ] || \
		fail "expected $worker_count pipeline $name records"
done

CPU_ACCEL_UAPI_SMP_CPUS="$target_cpus" "$script_dir/test-uapi-lifecycle" || \
	fail "SMP manager-death test failed"

"$tool" run --cpus "$target_cpus" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-oslat \
	--smp-fail-worker 1 >"$failure_output" 2>&1 || \
	fail "SMP one-worker forced-stop test failed"
grep -q '^worker=1 .* state=7 result=0 .*user_escape_count=1' \
	"$failure_output" || \
	fail "forced-stop worker did not report an escaped result"
grep -q '^state=0 mode=0 ' "$failure_output" || \
	fail "one-worker forced stop did not reset the domain to Linux mode"
worker_lines=$(grep -c '^worker=' "$failure_output" || true)
[ "$worker_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count forced-stop worker records, found $worker_lines"
successful_lines=$(grep -c '^worker=.* state=3 result=0 ' \
	"$failure_output" || true)
expected_successes=$((worker_count - 1))
[ "$successful_lines" -eq "$expected_successes" ] || \
	fail "expected $expected_successes surviving workers, found $successful_lines"
for pattern in 'context_switches=0' 'migration_detected=0'; do
	matching_lines=$(grep -c "^worker=.* $pattern" "$failure_output" || true)
	[ "$matching_lines" -eq "$worker_count" ] || \
		fail "forced-stop worker telemetry mismatch for $pattern"
done
matching_lines=$(grep -c '^worker=.* state=3 .*user_escape_count=0' \
	"$failure_output" || true)
[ "$matching_lines" -eq "$expected_successes" ] || \
	fail "forced-stop surviving worker telemetry mismatch"
for name in samples hist_total; do
	values=$(grep '^worker=.* state=3 result=0 ' "$failure_output" |
		sed -n "s/.*[[:space:]]$name=\([0-9][0-9]*\).*/\1/p")
	value_count=0
	for value in $values; do
		value_count=$((value_count + 1))
		[ "$value" -gt 0 ] || \
			fail "surviving worker telemetry did not report $name"
	done
	[ "$value_count" -eq "$expected_successes" ] || \
		fail "expected $expected_successes surviving $name records"
done

"$tool" run --cpus "$target_cpus" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-pipeline \
	--queue-capacity 1 --smp-fail-worker 1 \
	>"$pipeline_failure_output" 2>&1 || \
	fail "user-pipeline one-worker forced-stop test failed"
grep -q '^worker=1 .* state=7 result=0 .*user_escape_count=1' \
	"$pipeline_failure_output" || \
	fail "forced-stop pipeline worker did not report an escaped result"
grep -q '^state=0 mode=0 ' "$pipeline_failure_output" || \
	fail "user-pipeline forced stop did not reset the domain to Linux mode"
worker_lines=$(grep -c '^worker=' "$pipeline_failure_output" || true)
[ "$worker_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count pipeline forced-stop worker records, found $worker_lines"
successful_lines=$(grep -c '^worker=.* state=3 result=0 ' \
	"$pipeline_failure_output" || true)
expected_successes=$((worker_count - 1))
[ "$successful_lines" -eq "$expected_successes" ] || \
	fail "expected $expected_successes surviving pipeline workers, found $successful_lines"
queue_lines=$(grep -c '^queue=' "$pipeline_failure_output" || true)
[ "$queue_lines" -eq "$queue_count" ] || \
	fail "expected $queue_count forced-stop pipeline queue records, found $queue_lines"
for pattern in 'context_switches=0' 'migration_detected=0'; do
	matching_lines=$(grep -c "^worker=.* $pattern" \
		"$pipeline_failure_output" || true)
	[ "$matching_lines" -eq "$worker_count" ] || \
		fail "pipeline forced-stop telemetry mismatch for $pattern"
done
matching_lines=$(grep -c '^worker=.* state=3 .*user_escape_count=0' \
	"$pipeline_failure_output" || true)
[ "$matching_lines" -eq "$expected_successes" ] || \
	fail "pipeline forced-stop surviving worker telemetry mismatch"

echo "cpu_accel phase10 SMP gate passed"
