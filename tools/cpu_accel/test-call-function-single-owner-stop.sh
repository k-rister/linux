#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
probe=${CPU_ACCEL_SINGLE_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_call_function_single_probe.ko}
target_cpu=${CPU_ACCEL_CPU:-2}
work_cpu=${CPU_ACCEL_SINGLE_WORK_CPU:-3}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_call_function_single_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-call-function-single.XXXXXX)}
module_name=
module_loaded=0
trace_pid=
dmesg_pid=
run_pid=
probe_rc=0
run_rc=0

fail()
{
	echo "cpu_accel single-call stop test: $* (logs: $outdir)" >&2
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
	if [ "$module_loaded" -eq 1 ]; then
		rmmod "$module_name" 2>/dev/null || true
		module_loaded=0
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -f "$probe" ] || fail "single-call probe is missing: $probe"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw x86_cpu_accel_user_enter "$trace_root/available_filter_functions" || \
	fail "x86_cpu_accel_user_enter is not traceable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"
[ "$(cat "/sys/devices/system/cpu/cpu$work_cpu/online")" = 1 ] || \
	fail "work CPU $work_cpu is offline"
[ "$target_cpu" -ne "$work_cpu" ] || fail "work CPU must differ from target CPU"

module_name=$(modinfo -F name "$probe") || fail "cannot inspect probe module"
module_vermagic=$(modinfo -F vermagic "$probe") || \
	fail "cannot read probe vermagic"
case "$module_vermagic" in
	"$(uname -r) "*) ;;
	*) fail "probe vermagic does not match running kernel $(uname -r)" ;;
esac
if grep -q "^${module_name} " /proc/modules; then
	fail "probe module is already loaded: $module_name"
fi
insmod "$probe" "target_cpu=$target_cpu" || fail "could not load probe module"
module_loaded=1
fire_path="/sys/module/$module_name/parameters/fire"
[ -w "$fire_path" ] || fail "probe fire parameter is unavailable"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done
printf 'x86_cpu_accel_user_enter' >"$instance/set_ftrace_filter"
echo function_graph >"$instance/current_tracer"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
echo 1 >"$instance/tracing_on"
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload user-hang --escape-after-ms "$escape_ms" --escape-retries 1 \
	>"$outdir/ctl.log" 2>&1 &
run_pid=$!
ready=0
for n in $(seq 1 300); do
	if grep -Fq 'x86_cpu_accel_user_enter();' "$outdir/trace.log"; then
		ready=1
		break
	fi
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry trace timed out"

start_ns=$(date +%s%N)
if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
	sh -c 'set -e; echo 1 >"$1"; echo callback_count=1' sh "$fire_path" \
	>"$outdir/probe.log" 2>&1; then
	probe_rc=0
else
	probe_rc=$?
fi
end_ns=$(date +%s%N)
probe_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

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

[ "$probe_rc" -eq 0 ] || fail "synchronous single-call probe failed or timed out"
[ "$probe_elapsed_ms" -lt 2000 ] || \
	fail "single-call waiter waited for timed owner escape (${probe_elapsed_ms} ms)"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
grep -q 'state=7 ' "$outdir/ctl.log" || fail "owner did not report ESCAPED"
grep -q 'recovery_state=2' "$outdir/ctl.log" || \
	fail "owner escape recovery did not complete"
grep -q 'user_escape_count=1' "$outdir/ctl.log" || \
	fail "owner escape count was not one"
grep -q 'callback_count=1' "$outdir/probe.log" || \
	fail "single-call callback did not complete"
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
/owner_stop_request:/ {
	if (number($0, "cpu") == target && index($0, "stop_cb=1") &&
	    index($0, "sent=1")) {
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
}' "$outdir/trace.log"; then
	cat "$outdir/trace.log" >&2
	fail "trace did not pair single-call stop with the active owner exit"
fi
if grep -Eq 'LOST [0-9]+ EVENTS' "$outdir/trace.log"; then
	fail "trace buffer reported lost events"
fi
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: synchronous single-call waiter stopped the registered owner"
echo "probe_elapsed_ms=$probe_elapsed_ms"
cat "$outdir/ctl.log" "$outdir/probe.log" "$outdir/trace.log"
echo "logs: $outdir"
