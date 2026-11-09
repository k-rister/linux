#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
duration_ms=${CPU_ACCEL_RESCTRL_DURATION_MS:-5000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
resctrl_root=${CPU_ACCEL_RESCTRL_FS:-/sys/fs/resctrl}
instance="$trace_root/instances/cpu_accel_resctrl_$$"
group="$resctrl_root/cpu-accel-resctrl-$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-resctrl.XXXXXX)}
resctrl_mounted_by_test=0
run_pid=
trace_pid=
dmesg_pid=
run_rc=0
resctrl_rc=0

fail()
{
	echo "cpu_accel resctrl owner-stop test: $* (logs: $outdir)" >&2
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
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
	if [ -d "$group" ]; then
		rmdir "$group" 2>/dev/null || true
	fi
	if [ "$resctrl_mounted_by_test" -eq 1 ] && mountpoint -q "$resctrl_root"; then
		umount "$resctrl_root" 2>/dev/null || true
		resctrl_mounted_by_test=0
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

if ! mountpoint -q "$resctrl_root"; then
	mount -t resctrl cpu-accel "$resctrl_root" || fail "could not mount resctrl"
	resctrl_mounted_by_test=1
fi
[ -r "$resctrl_root/schemata" ] || fail "resctrl schemata is unavailable"

mkdir "$group"
cp "$resctrl_root/schemata" "$group/schemata" || \
	fail "could not initialize the resctrl group schema"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
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
if timeout --kill-after=2s 10s sh -c 'echo "$1" >"$2"' \
	sh "$target_cpu" "$group/cpus_list" >"$outdir/resctrl.log" 2>&1; then
	resctrl_rc=0
else
	resctrl_rc=$?
fi
end_ns=$(date +%s%N)
resctrl_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

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

[ "$resctrl_rc" -eq 0 ] || fail "resctrl CPU assignment failed"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$resctrl_elapsed_ms" -lt 2000 ] || \
	fail "resctrl assignment waited too long (${resctrl_elapsed_ms} ms)"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "resctrl stop required an escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the resctrl operation"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the resctrl operation"
grep -Eq 'arch_call_function_deferred=[1-9][0-9]*' "$outdir/ctl.log" || \
	fail "owner did not defer the resctrl call-function IPI"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the resctrl operation"

stop_line=$(grep -E "owner_stop_request: cpu=$target_cpu .*caller=smp_call_function_many_cond" \
	"$outdir/trace.log" | tail -n 1 || true)
exit_line=$(grep -E "owner_exit_complete: cpu=$target_cpu .*stop_requested=1" \
	"$outdir/trace.log" | tail -n 1 || true)
[ -n "$stop_line" ] || fail "resctrl owner-stop trace is missing"
[ -n "$exit_line" ] || fail "resctrl owner-exit trace is missing"

stop_timestamp=$(printf '%s\n' "$stop_line" | awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v stop="$stop_timestamp" -v complete="$exit_timestamp" \
	'BEGIN { exit !(stop > 0 && complete > stop) }' || \
	fail "owner exit did not follow the resctrl stop request"

echo "cpu_accel resctrl owner-stop test: PASS"
echo "resctrl_elapsed_ms=$resctrl_elapsed_ms"
echo "stop_line=$stop_line"
echo "exit_line=$exit_line"
