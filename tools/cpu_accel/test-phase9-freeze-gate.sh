#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_FREEZE_GATE_CPU:-5}
duration_ms=${CPU_ACCEL_FREEZE_GATE_DURATION_MS:-1000}
period_us=${CPU_ACCEL_FREEZE_GATE_PERIOD_US:-1000}
outdir=$(mktemp -d /tmp/cpu-accel-phase9-freeze-gate.XXXXXX)
baseline_output="$outdir/abi23.txt"
candidate_output="$outdir/abi24.txt"

fail()
{
	echo "cpu_accel phase9 freeze gate: $*" >&2
	[ ! -f "$baseline_output" ] || cat "$baseline_output" >&2
	[ ! -f "$candidate_output" ] || cat "$candidate_output" >&2
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	rm -rf "$outdir"
	exit "$status"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ "$target_cpu" -ne 0 ] || fail "target CPU must differ from CPU 0"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"
for command in cat grep sed; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done

CPU_ACCEL_UAPI_CPU=$target_cpu "$script_dir/test-uapi-contract" || \
	fail "ABI contract test failed"
CPU_ACCEL_UAPI_CPU=$target_cpu "$script_dir/test-uapi-lifecycle" || \
	fail "UAPI lifecycle test failed"

CPU_ACCEL_ABI_VERSION=23 "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-oslat \
	>"$baseline_output" 2>&1 || fail "ABI 23 user-oslat run failed"
CPU_ACCEL_ABI_VERSION=24 "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-oslat \
	>"$candidate_output" 2>&1 || fail "ABI 24 user-oslat run failed"

field()
{
	file="$1"
	name="$2"
	value=$(sed -n "s/.*[[:space:]]$name=\([0-9][0-9]*\).*/\1/p" "$file" | tail -n 1)
	[ -n "$value" ] || fail "$name was not reported in $file"
	printf '%s\n' "$value"
}

for output in "$baseline_output" "$candidate_output"; do
	grep -q '^state=3 ' "$output" || \
		fail "user-oslat did not complete in $output"
	grep -q ' mode=0 ' "$output" || \
		fail "user-oslat did not return to Linux mode in $output"
	grep -q ' backend=2 ' "$output" || \
		fail "user-oslat selected the wrong backend in $output"
	grep -q ' workload=7 ' "$output" || \
		fail "user-oslat workload was not selected in $output"
	grep -q ' region_count=1 ' "$output" || \
		fail "user-oslat region count was not reported in $output"
	[ "$(field "$output" samples)" -gt 0 ] || \
		fail "user-oslat produced no samples in $output"
	[ "$(field "$output" hist_total)" -gt 0 ] || \
		fail "user-oslat produced no histogram samples in $output"
	[ "$(field "$output" context_switches)" -eq 0 ] || \
		fail "user-oslat context-switched in $output"
	[ "$(field "$output" migration_detected)" -eq 0 ] || \
		fail "user-oslat migrated in $output"
	[ "$(field "$output" user_escape_count)" -eq 0 ] || \
		fail "user-oslat escaped in $output"
done

for metric in hist_p99_ns hist_p999_ns max_lateness_ns; do
	baseline=$(field "$baseline_output" "$metric")
	candidate=$(field "$candidate_output" "$metric")
	if [ "$candidate" -gt $((baseline * 2)) ]; then
		fail "ABI 24 $metric regressed beyond 2x: $candidate > $baseline"
	fi
done

echo "cpu_accel phase9 freeze gate passed"
