#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
churn_tool=${CPU_ACCEL_SECCOMP_CHURN:-$script_dir/test-seccomp-jit-churn}
target_cpu=${CPU_ACCEL_CPU:-2}
work_cpus=${CPU_ACCEL_SECCOMP_CPUS:-3-4}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_call_function_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-call-function.XXXXXX)}
trace_pid=
dmesg_pid=
run_pid=
helper_rc=0
run_rc=0

fail()
{
	echo "cpu_accel call-function stop test: $* (logs: $outdir)" >&2
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
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -x "$churn_tool" ] || fail "seccomp churn helper is not executable: $churn_tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw bpf_arch_ibpb "$trace_root/available_filter_functions" || \
	fail "bpf_arch_ibpb is not traceable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

jit_enabled=$(cat /proc/sys/net/core/bpf_jit_enable)
case "$jit_enabled" in
	1|2) ;;
	*) fail "BPF JIT is disabled (bpf_jit_enable=$jit_enabled)" ;;
esac

# Leave a dirty classic-BPF pack so the active-owner allocation executes IBPB
# under pack_mutex without first allocating executable pages through CPA.
taskset -c "${work_cpus%%-*}" "$churn_tool" 1 100 \
	>"$outdir/warm.log" 2>&1 || fail "could not prewarm the BPF pack"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done
printf 'x86_cpu_accel_user_enter bpf_arch_ibpb' >"$instance/set_ftrace_filter"
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
if timeout --kill-after=2s 8s taskset -c "$work_cpus" \
	"$churn_tool" 2 500 >"$outdir/churn.log" 2>&1; then
	helper_rc=0
else
	helper_rc=$?
fi
end_ns=$(date +%s%N)
helper_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

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

[ "$helper_rc" -eq 0 ] || fail "seccomp/JIT churn failed or timed out"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
grep -q 'state=7 ' "$outdir/ctl.log" || fail "owner did not report ESCAPED"
grep -q 'recovery_state=2' "$outdir/ctl.log" || \
	fail "owner escape recovery did not complete"
grep -Eq 'arch_call_function_deferred=[1-9][0-9]*' "$outdir/ctl.log" || \
	fail "the active owner did not defer a call-function IPI"
[ "$helper_elapsed_ms" -lt 2000 ] || \
	fail "seccomp/JIT caller waited for the timed owner escape (${helper_elapsed_ms} ms)"
# The x86 hook tail-calls the stop helper, so the trace names this generic
# synchronous call-function path as the caller.
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
	    index($0, "sent=1") && index($0, "caller=smp_call_function_many_cond")) {
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
	fail "trace did not pair call-function stop with the active owner exit"
fi
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: deferred call-function IPI stopped the registered owner before timed escape"
echo "helper_elapsed_ms=$helper_elapsed_ms"
cat "$outdir/ctl.log" "$outdir/churn.log" "$outdir/trace.log"
echo "logs: $outdir"
