#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_WRITEBACK_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_WRITEBACK_DURATION_MS:-10000}
file_size_mb=${CPU_ACCEL_WRITEBACK_FILE_MB:-64}
file_root=${CPU_ACCEL_WRITEBACK_ROOT:-/var/tmp}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_writeback_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-writeback.XXXXXX)}
mkdir -p "$outdir"
file=$(mktemp "$file_root/cpu-accel-writeback.XXXXXX")
run_pid=
trace_pid=
dmesg_pid=
run_rc=0
writeback_rc=0

fail()
{
	echo "cpu_accel dirty-writeback owner test: $* (logs: $outdir)" >&2
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
	rm -f "$file"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
for command in awk dd dmesg grep sed stat sync taskset timeout; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"

for event in writeback/writeback_dirty_folio \
	     writeback/writeback_single_inode \
	     cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in writeback/writeback_dirty_folio \
	     writeback/writeback_single_inode \
	     cpu_accel/owner_stop_request cpu_accel/owner_exit_complete; do
	echo 1 >"$instance/events/$event/enable"
done
echo 1 >"$instance/tracing_on"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

rm -f "$outdir/active"
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
if timeout --kill-after=5s 20s taskset -c "$work_cpu" sh -c \
	'dd if=/dev/zero of="$1" bs=1M count="$2" status=none &&
	 stat -c %i "$1" >"$3" &&
	 sync -f "$1"' sh "$file" "$file_size_mb" \
		"$outdir/inode" >"$outdir/writeback.log" 2>&1; then
	writeback_rc=0
else
	writeback_rc=$?
fi
end_ns=$(date +%s%N)
writeback_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

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

[ "$writeback_rc" -eq 0 ] || fail "dirty-writeback operation failed ($writeback_rc)"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
inode=$(cat "$outdir/inode" 2>/dev/null || true)
case "$inode" in
''|*[!0-9]*)
	fail "temporary-file inode was not captured"
	;;
esac

grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "dirty writeback required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during dirty writeback"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during dirty writeback"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during dirty writeback"

dirty_line=$(grep -E "writeback_dirty_folio:.*ino=$inode" \
	"$outdir/trace.log" | head -n 1 || true)
writeback_line=$(grep -E "writeback_single_inode:.*ino=$inode" \
	"$outdir/trace.log" | tail -n 1 || true)
stop_line=$(grep -E "owner_stop_request:.*cpu=$target_cpu " \
	"$outdir/trace.log" | tail -n 1 || true)
exit_line=$(grep -E "owner_exit_complete:.*cpu=$target_cpu " \
	"$outdir/trace.log" | tail -n 1 || true)
[ -n "$dirty_line" ] || fail "dirty-folio trace is missing for the temporary file"
[ -n "$writeback_line" ] || \
	fail "writeback-completion trace is missing for the temporary file"
[ -n "$exit_line" ] || fail "owner-exit trace is missing"

wrote_pages=$(printf '%s\n' "$writeback_line" | \
	sed -n 's/.* wrote=\([0-9][0-9]*\).*/\1/p')
case "$wrote_pages" in
''|*[!0-9]*|0)
	fail "temporary-file writeback did not write pages"
	;;
esac

dirty_timestamp=$(printf '%s\n' "$dirty_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
writeback_timestamp=$(printf '%s\n' "$writeback_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v dirty="$dirty_timestamp" -v writeback="$writeback_timestamp" \
	-v complete="$exit_timestamp" \
	'BEGIN { exit !(dirty > 0 && writeback > dirty && complete > writeback) }' || \
	fail "dirty writeback did not precede owner exit in the trace"

echo "cpu_accel dirty-writeback owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu file_mb=$file_size_mb"
echo "writeback_elapsed_ms=$writeback_elapsed_ms inode=$inode wrote_pages=$wrote_pages"
echo "stop_line=$stop_line"
echo "dirty_line=$dirty_line"
echo "writeback_line=$writeback_line"
echo "exit_line=$exit_line"
