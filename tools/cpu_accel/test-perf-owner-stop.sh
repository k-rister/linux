#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
event_helper=${CPU_ACCEL_PERF_EVENT_HELPER:-$script_dir/test-perf-event-open}
target_cpu=${CPU_ACCEL_CPU:-2}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_perf_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-perf.XXXXXX)}
run_output=$(mktemp)
active_marker=$(mktemp)
trace_pid=
dmesg_pid=
run_pid=
perf_rc=0
run_rc=0

fail()
{
	echo "cpu_accel perf owner-stop test: $* (logs: $outdir)" >&2
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
	rm -f "$run_output" "$active_marker"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -x "$event_helper" ] || fail "perf event helper is not executable: $event_helper"
command -v timeout >/dev/null 2>&1 || fail "timeout is required"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
case "$target_cpu" in
	''|*[!0-9]*) fail "target CPU must be a nonnegative integer" ;;
esac
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done
cat "$instance/trace_pipe" >"$outdir/owner-trace.log" &
trace_pid=$!
echo 1 >"$instance/tracing_on"
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

# The control utility publishes this marker only after shared state confirms
# ring-3 RUNNING state, accelerator mode, and an active-start timestamp.
rm -f "$active_marker"
CPU_ACCEL_ACTIVE_MARKER="$active_marker" \
	"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload user-hang --escape-after-ms "$escape_ms" --escape-retries 1 \
	>"$run_output" 2>&1 &
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

# A CPU-scoped PMU event is installed through perf's synchronous CPU callback.
# The generic synchronous-call hook must stop the owner before that callback
# can complete on the target CPU.
start_ns=$(date +%s%N)
if timeout --kill-after=2s 8s "$event_helper" "$target_cpu" 250 \
	>"$outdir/perf.log" 2>&1; then
	perf_rc=0
else
	perf_rc=$?
fi
end_ns=$(date +%s%N)
perf_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

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
grep -Ei 'soft lockup|task .* blocked for more than|RCU.*stall|(^|[[:space:]])BUG:|Oops:|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true

[ "$perf_rc" -eq 0 ] || fail "perf event open/run/close failed or timed out"
grep -Eq "^cpu=$target_cpu cycles=[0-9]+$" "$outdir/perf.log" || \
	fail "perf event helper returned malformed output"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$perf_elapsed_ms" -lt 2000 ] || \
	fail "perf waited for the timed owner escape (${perf_elapsed_ms} ms)"
grep -q 'state=7 ' "$run_output" || fail "owner did not report ESCAPED"
grep -q 'recovery_state=2' "$run_output" || \
	fail "owner escape recovery did not complete"
grep -Eq 'arch_call_function_deferred=[1-9][0-9]*' "$run_output" || \
	fail "the active owner did not defer a call-function IPI"
if ! awk -v target="$target_cpu" '
function number(line, name, rest, pos)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	rest = substr(line, pos + length(name) + 1)
	sub(/[^0-9].*$/, "", rest)
	return rest
}
function expected_perf_caller(line)
{
	return index(line, "caller=__smp_call_function_single") ||
	       index(line, "caller=smp_call_function_many_cond")
}
/owner_stop_request:/ {
	if (number($0, "cpu") == target && index($0, "stop_cb=1") &&
	    index($0, "sent=1") && expected_perf_caller($0)) {
		stop_owner = number($0, "owner")
		stop_line = NR
	}
}
/owner_exit_complete:/ {
	if (number($0, "cpu") == target && number($0, "owner") == stop_owner &&
	    index($0, "stop_requested=1"))
		exit_line = NR
}
END {
	exit !(stop_owner != "" && stop_line && exit_line > stop_line)
}' "$outdir/owner-trace.log"; then
	cat "$outdir/owner-trace.log" >&2
	fail "trace did not pair the perf CPU callback with the active owner exit"
fi
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: per-CPU perf event stopped the active owner before timed escape"
echo "perf_elapsed_ms=$perf_elapsed_ms"
cat "$run_output" "$outdir/perf.log" "$outdir/owner-trace.log"
echo "logs: $outdir"
