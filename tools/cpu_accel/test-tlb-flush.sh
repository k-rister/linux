#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
probe=${CPU_ACCEL_TLB_FLUSH_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_tlb_flush_probe.ko}
target_cpu=${CPU_ACCEL_CPU:-2}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-3000}
cow_churn_ms=${CPU_ACCEL_COW_CHURN_MS:-0}
stress_cpu=${CPU_ACCEL_STRESS_CPU:-3}
cow_churn_tool=${CPU_ACCEL_TLB_COW_CHURN:-$script_dir/test-tlb-cow-churn}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_tlb_flush_$$"
module_name=
trace_pid=
run_pid=
churn_pid=
module_loaded=0
insmod_rc=0
run_rc=0
churn_rc=0
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-tlb-flush.XXXXXX)}

fail()
{
	echo "cpu_accel TLB flush test: $* (logs: $outdir)" >&2
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$churn_pid" ]; then
		kill -TERM "$churn_pid" 2>/dev/null || true
		wait "$churn_pid" 2>/dev/null || true
		churn_pid=
	fi
	if [ -n "$trace_pid" ]; then
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ -d "$instance" ]; then
		echo 0 >"$instance/tracing_on" 2>/dev/null || true
		rmdir "$instance" 2>/dev/null || true
	fi
	if [ "$module_loaded" -eq 1 ]; then
		rmmod "$module_name" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -f "$probe" ] || fail "flush probe module is missing: $probe"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
case "$target_cpu" in
	''|*[!0-9]*) fail "CPU must be a nonnegative integer" ;;
esac
case "$escape_ms" in
	''|*[!0-9]*) fail "escape delay must be a positive integer" ;;
esac
case "$cow_churn_ms" in
	''|*[!0-9]*) fail "COW churn duration must be a nonnegative integer" ;;
esac
case "$stress_cpu" in
	''|*[!0-9]*) fail "stress CPU must be a nonnegative integer" ;;
esac
[ "$escape_ms" -gt 0 ] || fail "escape delay must be positive"
[ "$escape_ms" -lt 5000 ] || fail "escape delay must be shorter than the workload"
[ "$cow_churn_ms" -le 30000 ] || fail "COW churn duration must be at most 30000 ms"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"
if [ "$cow_churn_ms" -gt 0 ]; then
	[ "$stress_cpu" -ne "$target_cpu" ] || \
		fail "stress CPU must differ from owner CPU $target_cpu"
	[ "$(cat "/sys/devices/system/cpu/cpu$stress_cpu/online")" = 1 ] || \
		fail "stress CPU $stress_cpu is offline"
	[ -x "$cow_churn_tool" ] || \
		fail "COW churn helper is not executable: $cow_churn_tool"
fi

module_name=$(modinfo -F name "$probe")
case "$(modinfo -F vermagic "$probe")" in
	"$(uname -r) "*) ;;
	*) fail "probe vermagic does not match running kernel $(uname -r)" ;;
esac
if grep -q "^${module_name} " /proc/modules; then
	fail "probe module is already loaded: $module_name"
fi

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in tlb_flush_wait owner_stop_request owner_exit_complete; do
	[ -e "$instance/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done

# These core tracepoints identify IPI targets and callback delivery when
# diagnosing a stalled synchronous flush. They are optional across configs.
for event in \
	ipi/ipi_send_cpu ipi/ipi_send_cpumask \
	csd/csd_queue_cpu csd/csd_function_entry csd/csd_function_exit \
	irq_vectors/call_function_entry irq_vectors/call_function_exit \
	irq_vectors/call_function_single_entry \
	irq_vectors/call_function_single_exit; do
	if [ -e "$instance/events/$event/enable" ]; then
		echo 1 >"$instance/events/$event/enable"
	fi
done

cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
echo 1 >"$instance/tracing_on"

if [ "$cow_churn_ms" -gt 0 ]; then
	"$cow_churn_tool" "$stress_cpu" "$cow_churn_ms" \
		>"$outdir/cow-churn.log" 2>&1 &
	churn_pid=$!
	sleep 0.1
fi

"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload user-hang --escape-after-ms "$escape_ms" --escape-retries 1 \
	>"$outdir/ctl.log" 2>&1 &
run_pid=$!
sleep 0.5

if insmod "$probe" >"$outdir/insmod.log" 2>&1; then
	module_loaded=1
else
	insmod_rc=$?
fi
if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
if [ -n "$churn_pid" ]; then
	if wait "$churn_pid"; then
		churn_rc=0
	else
		churn_rc=$?
	fi
	churn_pid=
fi

echo 0 >"$instance/tracing_on"
kill -TERM "$trace_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
if [ "$module_loaded" -eq 1 ]; then
	rmmod "$module_name"
	module_loaded=0
fi
dmesg | grep -E 'soft lockup|RCU.*stall|BUG:|Oops:' >"$outdir/kernel-errors.log" || true

[ "$insmod_rc" -eq 0 ] || fail "probe module load failed"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$churn_rc" -eq 0 ] || fail "bounded fork/COW churn failed"
if [ "$cow_churn_ms" -gt 0 ]; then
	grep -q '^PASS: forks=[1-9][0-9]* cow_write_faults=[1-9][0-9]* ' \
		"$outdir/cow-churn.log" || fail "fork/COW churn did not complete"
fi
grep -q 'state=7 ' "$outdir/ctl.log" || fail "owner did not report ESCAPED"
grep -q 'recovery_state=2' "$outdir/ctl.log" || \
	fail "owner escape recovery did not complete"
grep -q 'user_escape_count=1' "$outdir/ctl.log" || \
	fail "owner escape count was not one"
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
/tlb_flush_wait:/ {
	id = number($0, "id")
	if (index($0, "phase=begin")) {
		if (id == "" || ++begin_count[id] != 1)
			bad_pair = 1
		begin_at[id] = NR
		if (number($0, "owners") == 1) {
			begin_id = id
			begin_line = NR
		}
	}
	if (index($0, "phase=complete")) {
		if (id == "" || begin_count[id] != 1 ||
		    ++complete_count[id] != 1)
			bad_pair = 1
		complete_at[id] = NR
		complete_id = id
		complete_line = NR
	}
}
/owner_stop_request:/ {
	if (number($0, "cpu") == target &&
	    index($0, "stop_cb=1") && index($0, "sent=1")) {
		stop_owner = number($0, "owner")
		stop_line = NR
	}
}
/owner_exit_complete:/ {
	if (number($0, "cpu") == target &&
	    number($0, "owner") == stop_owner &&
	    index($0, "stop_requested=1"))
		exit_line = NR
}
END {
	for (id in begin_count)
		if (complete_count[id] != 1 || complete_at[id] <= begin_at[id])
			bad_pair = 1
	for (id in complete_count)
		if (begin_count[id] != 1)
			bad_pair = 1
	if (!bad_pair && begin_id != "" && begin_id == complete_id &&
	    stop_owner != "" && stop_line > begin_line &&
	    exit_line > stop_line && complete_line > exit_line)
		exit 0
	exit 1
}' "$outdir/trace.log"; then
	cat "$outdir/trace.log" >&2
	fail "trace did not pair every flush or show a completed stop/exit/flush sequence"
fi
if grep -Eq 'LOST [0-9]+ EVENTS' "$outdir/trace.log"; then
	fail "trace buffer reported lost events"
fi
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

echo "PASS: owner stop, exit, and global flush completion traced"
if [ "$cow_churn_ms" -gt 0 ]; then
	cat "$outdir/cow-churn.log"
fi
cat "$outdir/ctl.log"
cat "$outdir/trace.log"
echo "logs: $outdir"
