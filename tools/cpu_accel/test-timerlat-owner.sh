#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
trace_root=${TRACEFS:-/sys/kernel/tracing}
target_cpu=${CPU_ACCEL_TIMERLAT_CPU:-2}
period_us=${CPU_ACCEL_TIMERLAT_PERIOD_US:-1000}
escape_ms=${CPU_ACCEL_TIMERLAT_ESCAPE_AFTER_MS:-250}
baseline_ms=${CPU_ACCEL_TIMERLAT_BASELINE_MS:-500}
instance="$trace_root/instances/cpu_accel_timerlat_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-timerlat.XXXXXX)}
active_marker="$outdir/owner-active"
baseline_trace="$outdir/timerlat-baseline.trace"
owner_trace="$outdir/timerlat-owner.trace"
run_pid=
dmesg_pid=
osnoise_config_saved=0
saved_osnoise_cpus=
saved_timerlat_period_us=
saved_stop_tracing_us=
saved_stop_tracing_total_us=

fail()
{
	echo "cpu_accel timerlat owner test: $* (logs: $outdir)" >&2
	if [ -f "$outdir/kernel-errors.log" ]; then
		cat "$outdir/kernel-errors.log" >&2
	fi
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -d "$instance" ]; then
		echo 0 >"$instance/tracing_on" 2>/dev/null || true
		echo nop >"$instance/current_tracer" 2>/dev/null || true
		if [ "$osnoise_config_saved" -eq 1 ]; then
			echo "$saved_osnoise_cpus" >"$trace_root/osnoise/cpus" 2>/dev/null || true
			echo "$saved_timerlat_period_us" >"$trace_root/osnoise/timerlat_period_us" 2>/dev/null || true
			echo "$saved_stop_tracing_us" >"$trace_root/osnoise/stop_tracing_us" 2>/dev/null || true
			echo "$saved_stop_tracing_total_us" >"$trace_root/osnoise/stop_tracing_total_us" 2>/dev/null || true
			osnoise_config_saved=0
		fi
		rmdir "$instance" 2>/dev/null || true
	fi
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
[ -d "$trace_root/osnoise" ] || fail "timerlat/osnoise tracing is unavailable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"
grep -qw timerlat "$trace_root/available_tracers" || \
	fail "timerlat tracer is unavailable"
case "$period_us:$escape_ms:$baseline_ms" in
*[!0-9:]*|0:*|*:0:*|*:*:0) \
	fail "period, escape time, and baseline time must be positive integers" ;;
esac
[ "$escape_ms" -ge 100 ] || fail "escape time must be at least 100 ms"
[ "$baseline_ms" -ge 100 ] || fail "baseline time must be at least 100 ms"

mkdir "$instance" || fail "could not create tracefs instance"
for path in "$trace_root/osnoise/cpus" \
	"$trace_root/osnoise/timerlat_period_us" \
	"$trace_root/osnoise/stop_tracing_us" \
	"$trace_root/osnoise/stop_tracing_total_us" \
	"$instance/trace_clock"; do
	[ -w "$path" ] || fail "timerlat control is unavailable: $path"
done

saved_osnoise_cpus=$(cat "$trace_root/osnoise/cpus")
saved_timerlat_period_us=$(cat "$trace_root/osnoise/timerlat_period_us")
saved_stop_tracing_us=$(cat "$trace_root/osnoise/stop_tracing_us")
saved_stop_tracing_total_us=$(cat "$trace_root/osnoise/stop_tracing_total_us")
osnoise_config_saved=1
echo "$target_cpu" >"$trace_root/osnoise/cpus"
echo "$period_us" >"$trace_root/osnoise/timerlat_period_us"
echo 0 >"$trace_root/osnoise/stop_tracing_us"
echo 0 >"$trace_root/osnoise/stop_tracing_total_us"
echo mono >"$instance/trace_clock"
echo timerlat >"$instance/current_tracer"
echo 1 >"$instance/tracing_on"
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!
sleep 0.05
echo 0 >"$instance/tracing_on"
: >"$instance/trace"
echo 1 >"$instance/tracing_on"
baseline_seconds=$(awk -v ms="$baseline_ms" 'BEGIN { printf "%.3f", ms / 1000 }')
sleep "$baseline_seconds"
echo 0 >"$instance/tracing_on"
cat "$instance/trace" >"$baseline_trace"
: >"$instance/trace"
echo 1 >"$instance/tracing_on"
sleep 0.02

rm -f "$active_marker"
CPU_ACCEL_ACTIVE_MARKER="$active_marker" timeout --kill-after=5s 10s \
	"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload user-hang --escape-after-ms "$escape_ms" --escape-retries 1 \
	>"$outdir/ctl.log" 2>&1 &
run_pid=$!
ready=0
for n in $(seq 1 300); do
	if [ -s "$active_marker" ] &&
	   [ "$(cat "$active_marker")" = active ]; then
		ready=1
		break
	fi
	if ! kill -0 "$run_pid" 2>/dev/null; then
		wait "$run_pid" || true
		run_pid=
		cat "$outdir/ctl.log" >&2
		fail "owner exited before publishing the active marker"
	fi
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner did not publish the active marker"

# Keep the timerlat tracer running across owner entry and correlate its
# monotonic timestamps with the owner interval reported by cpu-accelctl.
wait "$run_pid" || fail "cpu-accelctl run failed"
run_pid=
sleep 0.05
echo 0 >"$instance/tracing_on"
cat "$instance/trace" >"$owner_trace"
kill -TERM "$dmesg_pid" 2>/dev/null || true
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

grep -q 'state=7 ' "$outdir/ctl.log" || \
	fail "owner did not report the escaped terminal state"
grep -q 'recovery_state=2' "$outdir/ctl.log" || \
	fail "owner recovery did not complete"
active_start_ns=$(sed -n 's/.*user_active_start_ns=\([0-9][0-9]*\).*/\1/p' \
	"$outdir/ctl.log")
active_end_ns=$(sed -n 's/.*user_active_end_ns=\([0-9][0-9]*\).*/\1/p' \
	"$outdir/ctl.log")
[ -n "$active_start_ns" ] && [ -n "$active_end_ns" ] || \
	fail "owner active interval is missing from the result"

cpu_label=$(printf '[%03d]' "$target_cpu")
latency_summary()
{
	kind=$1
	awk -v cpu="$cpu_label" -v kind="$kind" \
		'$2 == cpu && $0 ~ "context +" kind " +timer_latency" { print $(NF - 1) }' \
		"$baseline_trace" | sort -n | awk -v kind="$kind" '
		{ sample[NR] = $1 }
		END {
			if (!NR)
				exit 1
			p50 = int(NR * 0.50 + 0.999999)
			p95 = int(NR * 0.95 + 0.999999)
			p99 = int(NR * 0.99 + 0.999999)
			printf "baseline_%s_samples=%d baseline_%s_p50_ns=%d " \
				"baseline_%s_p95_ns=%d baseline_%s_p99_ns=%d " \
				"baseline_%s_max_ns=%d\n", \
				kind, NR, kind, sample[p50], kind, sample[p95], \
				kind, sample[p99], kind, sample[NR]
		}'
}

baseline_irq_summary=$(latency_summary irq) || \
	fail "baseline timerlat IRQ samples are missing"
baseline_thread_summary=$(latency_summary thread) || \
	fail "baseline timerlat thread samples are missing"

if ! awk -v cpu="$cpu_label" -v start="$active_start_ns" \
	-v end="$active_end_ns" '
function observe(kind, latency, timestamp)
{
	latency = $(NF - 1) + 0
	timestamp = $4
	sub(/:$/, "", timestamp)
	timestamp *= 1000000000
	kind = /context +irq +timer_latency/ ? "irq" : "thread"
	total[kind]++
	if (latency > max_total[kind])
		max_total[kind] = latency
	if (timestamp < start)
		before[kind]++
	else if (timestamp <= end)
		active[kind]++
	else
		after[kind]++
	if (timestamp >= start && timestamp <= end && latency > max_active[kind])
		max_active[kind] = latency
	if (kind == "irq") {
		if (previous_irq && timestamp - previous_irq > max_irq_gap)
			max_irq_gap = timestamp - previous_irq
		previous_irq = timestamp
	}
}
$2 == cpu && /context +(irq|thread) +timer_latency/ { observe() }
END {
	if (!total["irq"] || !before["irq"] || !after["irq"] ||
	    !total["thread"] || !before["thread"] || !after["thread"])
		exit 1
	printf "owner_timerlat_cpu=%s ", cpu
	for (i = 1; i <= 2; i++) {
		kind = i == 1 ? "irq" : "thread"
		printf "%s_before=%d %s_active=%d %s_after=%d ", \
			kind, before[kind], kind, active[kind], kind, after[kind]
		printf "%s_max_active_ns=%d %s_max_total_ns=%d ", \
			kind, max_active[kind], kind, max_total[kind]
	}
	printf "irq_max_gap_ns=%.0f\n", max_irq_gap
}' "$owner_trace" >"$outdir/timerlat-summary.log"; then
	fail "timerlat samples do not bracket the active owner interval"
fi

cat "$outdir/timerlat-summary.log"
echo "$baseline_irq_summary"
echo "$baseline_thread_summary"
grep -Ei 'BUG:|Oops:|soft lockup|blocked for more than|RCU.*stall' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
[ ! -s "$outdir/kernel-errors.log" ] || \
	fail "kernel warning/error appeared during the timerlat run"

grep -Eo 'user_active_ns=[0-9]+|user_escape_count=[0-9]+|timer_softirq_count=[0-9]+|hrtimer_softirq_count=[0-9]+' \
	"$outdir/ctl.log"
echo "PASS: timerlat baseline and owner interval were captured on CPU $target_cpu"
echo "NOTE: owner-run timerlat values are observations; this test enforces no latency bound."
echo "logs: $outdir"
