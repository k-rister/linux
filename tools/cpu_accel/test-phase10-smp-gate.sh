#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpus=${CPU_ACCEL_PHASE10_CPUS:-5,7}
duration_ms=${CPU_ACCEL_PHASE10_DURATION_MS:-1000}
period_us=${CPU_ACCEL_PHASE10_PERIOD_US:-1000}
normal_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)
failure_output=$(mktemp /tmp/cpu-accel-phase10-smp-gate.XXXXXX)

fail()
{
	echo "cpu_accel phase10 SMP gate: $*" >&2
	for file in "$normal_output" "$failure_output"; do
		[ ! -f "$file" ] || cat "$file" >&2
	done
	rm -f "$normal_output" "$failure_output"
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	rm -f "$normal_output" "$failure_output"
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

CPU_ACCEL_UAPI_SMP_CPUS="$target_cpus" "$script_dir/test-uapi-lifecycle" || \
	fail "SMP manager-death test failed"

"$tool" run --cpus "$target_cpus" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-oslat \
	--smp-fail-worker 1 >"$failure_output" 2>&1 || \
	fail "SMP one-worker failure test failed"
grep -q '^worker=1 .* state=5 result=-' "$failure_output" || \
	fail "failed worker did not report an error result"
grep -q '^state=0 mode=0 ' "$failure_output" || \
	fail "one-worker failure did not reset the domain to Linux mode"
worker_lines=$(grep -c '^worker=' "$failure_output" || true)
[ "$worker_lines" -eq "$worker_count" ] || \
	fail "expected $worker_count failure-run worker records, found $worker_lines"
successful_lines=$(grep -c '^worker=.* state=3 result=0 ' \
	"$failure_output" || true)
expected_successes=$((worker_count - 1))
[ "$successful_lines" -eq "$expected_successes" ] || \
	fail "expected $expected_successes surviving workers, found $successful_lines"
for pattern in 'context_switches=0' 'migration_detected=0' \
	       'user_escape_count=0'; do
	matching_lines=$(grep -c "^worker=.* $pattern" "$failure_output" || true)
	[ "$matching_lines" -eq "$worker_count" ] || \
		fail "failure-run worker telemetry mismatch for $pattern"
done
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

echo "cpu_accel phase10 SMP gate passed"
