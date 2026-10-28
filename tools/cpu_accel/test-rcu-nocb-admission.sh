#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
offloaded_cpu=${CPU_ACCEL_RCU_OFFLOADED_CPU:-2}
other_cpu=${CPU_ACCEL_RCU_OTHER_CPU:-1}
other_offloaded=${CPU_ACCEL_RCU_OTHER_OFFLOADED:-0}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-rcu-nocb.XXXXXX)}

fail()
{
	echo "cpu_accel RCU nocb admission test: $* (logs: $outdir)" >&2
	[ ! -f "$outdir/offloaded.log" ] || cat "$outdir/offloaded.log" >&2
	[ ! -f "$outdir/other.log" ] || cat "$outdir/other.log" >&2
	exit 1
}

cpulist_contains()
{
	printf '%s\n' "$1" | awk -F, -v cpu="$2" '
		{
			for (i = 1; i <= NF; i++) {
				split($i, range, "-")
				first = range[1] + 0
				last = (range[2] == "" ? first : range[2] + 0)
				if (cpu >= first && cpu <= last)
					found = 1
			}
		}
		END { exit !found }
	'
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
case "$offloaded_cpu:$other_cpu" in
*[!0-9:]*|0:*|*:0) fail "CPU IDs must be positive integers" ;;
esac
[ "$offloaded_cpu" -ne "$other_cpu" ] || \
	fail "offloaded and comparison CPU IDs must differ"
case "$other_offloaded" in
0|1) ;;
*) fail "CPU_ACCEL_RCU_OTHER_OFFLOADED must be 0 or 1" ;;
esac

for cpu in "$offloaded_cpu" "$other_cpu"; do
	[ -r "/sys/devices/system/cpu/cpu$cpu/online" ] || \
		fail "CPU $cpu is not present or has no online state"
	[ "$(cat "/sys/devices/system/cpu/cpu$cpu/online")" = 1 ] || \
		fail "CPU $cpu is offline"
done

nohz_full=$(cat /sys/devices/system/cpu/nohz_full)
cpulist_contains "$nohz_full" "$offloaded_cpu" || \
	fail "target CPU $offloaded_cpu is not isolated from RCU housekeeping"

if [ "$other_offloaded" -eq 1 ]; then
	if cpulist_contains "$nohz_full" "$other_cpu"; then
		"$tool" run --cpu "$other_cpu" --duration-ms 100 \
			--period-us 1000 --workload user-oslat >"$outdir/other.log" 2>&1 || \
			fail "offloaded and isolated comparison CPU was rejected"
		grep -q 'state=3 mode=0 ' "$outdir/other.log" || \
			fail "offloaded comparison CPU completed with an invalid state"
		grep -Eq 'samples=[1-9][0-9]* ' "$outdir/other.log" || \
			fail "offloaded comparison CPU completed without workload samples"
		other_result="admitted (offloaded and RCU-isolated)"
	else
		if "$tool" run --cpu "$other_cpu" --duration-ms 100 \
			--period-us 1000 --workload user-oslat >"$outdir/other.log" 2>&1; then
			fail "offloaded housekeeping comparison CPU was admitted"
		fi
		grep -q 'state=5 mode=4 ' "$outdir/other.log" || \
			fail "comparison CPU did not report a rejected owner state"
		grep -q 'recovery_error=95 ' "$outdir/other.log" || \
			fail "comparison CPU failed for a reason other than missing RCU isolation"
		other_result="rejected (still an RCU housekeeping CPU)"
	fi
else
	if "$tool" run --cpu "$other_cpu" --duration-ms 100 \
		--period-us 1000 --workload user-oslat >"$outdir/other.log" 2>&1; then
		fail "non-offloaded comparison CPU was admitted"
	fi
	grep -q 'state=5 mode=4 ' "$outdir/other.log" || \
		fail "comparison CPU did not report a rejected owner state"
	grep -q 'recovery_error=95 ' "$outdir/other.log" || \
		fail "comparison CPU failed for a reason other than missing RCU disposition"
	other_result="rejected (target lacks RCU callback offload)"
fi

"$tool" run --cpu "$offloaded_cpu" --duration-ms 100 \
	--period-us 1000 --workload user-oslat >"$outdir/offloaded.log" 2>&1 || \
	fail "offloaded CPU admission or workload failed"
grep -q 'state=3 mode=0 ' "$outdir/offloaded.log" || \
	fail "offloaded CPU did not complete in Linux mode"
grep -Eq 'samples=[1-9][0-9]* ' "$outdir/offloaded.log" || \
	fail "offloaded CPU completed without workload samples"

echo "comparison_cpu=$other_cpu result=$other_result"
cat "$outdir/offloaded.log"
echo "PASS: ring-3 admission requires RCU callback offload"
echo "logs: $outdir"
