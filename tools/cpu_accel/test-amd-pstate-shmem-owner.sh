#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_AMD_PSTATE_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_AMD_PSTATE_DURATION_MS:-5000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
cpufreq_dir="/sys/devices/system/cpu/cpu$target_cpu/cpufreq"
governor_file="$cpufreq_dir/scaling_governor"
driver_file="$cpufreq_dir/scaling_driver"
available_governors_file="$cpufreq_dir/scaling_available_governors"
instance="$trace_root/instances/cpu_accel_amd_pstate_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-amd-pstate.XXXXXX)}
mkdir -p "$outdir"
run_pid=
trace_pid=
dmesg_pid=
run_rc=0
pstate_rc=0
original_governor=
governor_changed=0

fail()
{
	echo "cpu_accel amd-pstate shared-memory owner test: $* (logs: $outdir)" >&2
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
	if [ -n "$trace_pid" ]; then
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -d "$instance" ]; then
		echo 0 >"$instance/tracing_on" 2>/dev/null || true
		rmdir "$instance" 2>/dev/null || true
	fi
	if [ "$governor_changed" -eq 1 ] && [ -n "$original_governor" ]; then
		printf '%s\n' "$original_governor" >"$governor_file" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/events/amd_cpu" ] || fail "amd-pstate tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
[ -r "$governor_file" ] || fail "target CPU frequency governor is unavailable"
[ "$(cat "$driver_file")" = "amd-pstate-epp" ] || \
	fail "target CPU does not use amd-pstate-epp"
grep -qw performance "$available_governors_file" || \
	fail "performance governor is unavailable on the target CPU"
grep -qw powersave "$available_governors_file" || \
	fail "powersave governor is unavailable on the target CPU"
original_governor=$(cat "$governor_file")
[ "$original_governor" = "performance" ] || \
	fail "target CPU must initially use the performance governor"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"

for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete \
	     amd_cpu/amd_pstate_epp_perf; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete \
	     amd_cpu/amd_pstate_epp_perf; do
	echo 1 >"$instance/events/$event/enable"
done
echo 1 >"$instance/tracing_on"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

CPU_ACCEL_ACTIVE_MARKER="$outdir/active" "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--workload user-oslat >"$outdir/ctl.log" 2>&1 &
run_pid=$!

ready=0
for attempt in $(seq 1 500); do
	if [ -f "$outdir/active" ]; then
		ready=1
		break
	fi
	if ! kill -0 "$run_pid" 2>/dev/null; then
		break
	fi
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry marker timed out"

start_ns=$(date +%s%N)
if timeout --kill-after=2s 10s taskset -c "$work_cpu" sh -c \
	'printf "%s\n" "$1" >"$2"' sh powersave "$governor_file" \
	>"$outdir/amd-pstate.log" 2>&1; then
	pstate_rc=0
else
	pstate_rc=$?
fi
end_ns=$(date +%s%N)
pstate_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
printf '%s\n' "$pstate_elapsed_ms" >"$outdir/pstate-elapsed-ms"
if [ "$pstate_rc" -eq 0 ]; then
	governor_changed=1
fi

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
sleep 0.1
echo 0 >"$instance/tracing_on"
kill -TERM "$trace_pid" "$dmesg_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

kernel_error_pattern='soft lockup|task .* blocked for more than|RCU.*stall'
kernel_error_pattern="$kernel_error_pattern|(^|[[:space:]])BUG:|Oops:|Kernel panic"
grep -Ei "$kernel_error_pattern" "$outdir/dmesg-new.log" \
	>"$outdir/kernel-errors.log" || true

[ "$pstate_rc" -eq 0 ] || fail "amd-pstate governor switch failed ($pstate_rc)"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$(cat "$governor_file")" = "powersave" ] || \
	fail "amd-pstate governor did not switch to powersave"
[ "$pstate_elapsed_ms" -lt 100 ] || \
	fail "amd-pstate shared-memory switch was not immediate (${pstate_elapsed_ms} ms)"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "amd-pstate update required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the amd-pstate update"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the amd-pstate update"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the amd-pstate update"

amd_pstate_line=$(grep -E "amd_pstate_epp_perf:.*cpu$target_cpu:.*changed=1" \
	"$outdir/trace.log" | tail -n 1 || true)
stop_line=$(grep -E "owner_stop_request: cpu=$target_cpu " \
	"$outdir/trace.log" | tail -n 1 || true)
exit_line=$(grep -E "owner_exit_complete: cpu=$target_cpu .*stop_requested=0" \
	"$outdir/trace.log" | tail -n 1 || true)
[ -n "$amd_pstate_line" ] || \
	fail "amd-pstate EPP trace is missing for the target CPU"
[ -z "$stop_line" ] || \
	fail "amd-pstate shared-memory update unexpectedly requested owner stop"
[ -n "$exit_line" ] || fail "amd-pstate owner-exit trace is missing"

amd_timestamp=$(printf '%s\n' "$amd_pstate_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v amd="$amd_timestamp" \
	-v complete="$exit_timestamp" \
	'BEGIN { exit !(amd > 0 && complete > amd) }' || \
	fail "amd-pstate update did not precede owner exit in the trace"

printf '%s\n' "$original_governor" >"$governor_file"
governor_changed=0
[ "$(cat "$governor_file")" = "$original_governor" ] || \
	fail "failed to restore the original amd-pstate governor"

echo "cpu_accel amd-pstate shared-memory owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu governor=$original_governor->powersave"
echo "pstate_elapsed_ms=$pstate_elapsed_ms"
echo "amd_pstate_line=$amd_pstate_line"
echo "exit_line=$exit_line"
