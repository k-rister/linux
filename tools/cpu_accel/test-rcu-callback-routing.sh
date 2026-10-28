#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
probe_module=${CPU_ACCEL_RCU_PROBE:-$script_dir/rcu-probe/cpu_accel_rcu_callback_probe.ko}
target_cpu=${CPU_ACCEL_RCU_CPU:-2}
reader_cpu=${CPU_ACCEL_RCU_READER_CPU:-0}
callback_count=${CPU_ACCEL_RCU_CALLBACKS:-64}
duration_ms=5000
probe_dir=/sys/kernel/debug/cpu_accel_rcu_callback_probe
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-rcu-routing.XXXXXX)}
probe_loaded=0
ctl_pid=

fail()
{
	echo "cpu_accel RCU callback routing test: $* (logs: $outdir)" >&2
	[ ! -f "$outdir/ctl.log" ] || cat "$outdir/ctl.log" >&2
	[ ! -f "$outdir/probe.log" ] || cat "$outdir/probe.log" >&2
	[ ! -f "$outdir/taskset.log" ] || cat "$outdir/taskset.log" >&2
	exit 1
}

cpu_in_mask()
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

check_nocb_affinity()
{
	cb_found=0
	gp_found=0
	for comm in /proc/[0-9]*/comm; do
		IFS= read -r name <"$comm" || continue
		case "$name" in
		rcuo[ps]/*|rcuog/*) ;;
		*) continue ;;
		esac
		pid=${comm#/proc/}
		pid=${pid%/comm}
		allowed=$(awk '/Cpus_allowed_list/ {print $2}' "/proc/$pid/status")
		[ -n "$allowed" ] || fail "could not read CPU mask for $name"
		cpu_in_mask "$allowed" "$target_cpu" && \
			fail "$name may still run on isolated owner CPU $target_cpu (mask $allowed)"
		case "$name" in
		rcuo[ps]/"$target_cpu") cb_found=1 ;;
		rcuog/*) gp_found=1 ;;
		esac
		if taskset -pc "$target_cpu" "$pid" >"$outdir/taskset.log" 2>&1; then
			taskset -pc "$allowed" "$pid" >/dev/null 2>&1 || true
			fail "$name accepted a userspace affinity change to the owner CPU"
		fi
	done
	[ "$cb_found" -eq 1 ] || fail "no callback kthread found for CPU $target_cpu"
	[ "$gp_found" -eq 1 ] || fail "no nocb grace-period kthread was found"
}

cleanup()
{
	status=
	attempt=0

	if [ -n "$ctl_pid" ] && kill -0 "$ctl_pid" 2>/dev/null; then
		wait "$ctl_pid" || true
	fi
	if [ "$probe_loaded" -eq 1 ]; then
		if [ -w "$probe_dir/release" ]; then
			echo 1 >"$probe_dir/release" 2>/dev/null || true
		fi
		while [ "$attempt" -lt 100 ]; do
			status=$(cat "$probe_dir/status" 2>/dev/null || true)
			queued=$(field "$status" queued)
			completed=$(field "$status" completed)
			[ "$queued" = "$completed" ] && break
			sleep 0.1
			attempt=$((attempt + 1))
		done
		[ -z "$status" ] || printf '%s\n' "$status" >"$outdir/probe.log"
		rmmod cpu_accel_rcu_callback_probe 2>/dev/null || true
	fi
}
trap cleanup EXIT HUP INT TERM

field()
{
	printf '%s\n' "$1" | awk -v key="$2" '{
		for (i = 1; i <= NF; i++) {
			if (index($i, key "=") == 1) {
				sub("^" key "=", "", $i)
				print $i
				exit
			}
		}
	}'
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -f "$probe_module" ] || fail "RCU probe module is missing: $probe_module"
[ -d /sys/kernel/debug ] || fail "debugfs is unavailable"
[ -r /sys/devices/system/cpu/nohz_full ] || fail "nohz_full CPU mask is unavailable"
[ -n "$(command -v taskset 2>/dev/null || true)" ] || fail "taskset is required"
[ ! -e "$probe_dir" ] || fail "RCU probe is already loaded"
[ ! -e /sys/module/cpu_accel_rcu_callback_probe ] || \
	fail "RCU probe module is already loaded"
case "$target_cpu:$reader_cpu:$callback_count" in
*[!0-9:]*|0:*|*::*) fail "CPU IDs and callback count must be integers" ;;
esac
[ "$callback_count" -gt 0 ] || fail "callback count must be positive"
[ "$target_cpu" -ne "$reader_cpu" ] || \
	fail "target and RCU reader CPUs must differ"
[ "$callback_count" -le 256 ] || fail "callback count exceeds probe limit"

for cpu in "$target_cpu" "$reader_cpu"; do
	if [ "$cpu" -eq 0 ]; then
		[ -d /sys/devices/system/cpu/cpu0 ] || \
			fail "CPU 0 is not present"
		continue
	fi
	[ -r "/sys/devices/system/cpu/cpu$cpu/online" ] || \
		fail "CPU $cpu is not present or has no online state"
	[ "$(cat "/sys/devices/system/cpu/cpu$cpu/online")" = 1 ] || \
		fail "CPU $cpu is offline"
done

nohz_full=$(cat /sys/devices/system/cpu/nohz_full)
cpu_in_mask "$nohz_full" "$target_cpu" || \
	fail "target CPU $target_cpu is not isolated from RCU housekeeping"
check_nocb_affinity

insmod "$probe_module" target_cpu="$target_cpu" reader_cpu="$reader_cpu" || \
	fail "could not load RCU callback probe"
probe_loaded=1
[ -d "$probe_dir" ] || fail "RCU callback probe did not create its debugfs interface"

# Module init text is reclaimed asynchronously after an RCU grace period.
# Let that work finish before holding another read-side section; its execmem
# cleanup performs a kernel-wide TLB flush that correctly stops an active owner.
# One second was not sufficient on the test VM: the first grace period could
# still be pending when the probe reader was acquired, so the later TLB flush
# interrupted the owner run. Give init-text reclamation three seconds to drain.
sleep 3

echo 1 >"$probe_dir/hold" || fail "could not hold an RCU read-side section"
grep -q 'reader_active=1' "$probe_dir/status" || \
	fail "RCU reader did not enter its read-side section"
echo "$callback_count" >"$probe_dir/queue" || \
	fail "could not queue callbacks on CPU $target_cpu"

"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us 1000 --workload user-oslat >"$outdir/ctl.log" 2>&1 &
ctl_pid=$!
sleep 1
kill -0 "$ctl_pid" 2>/dev/null || fail "owner run ended before callback release"
echo 1 >"$probe_dir/release" || fail "could not release the RCU reader"
if ! wait "$ctl_pid"; then
	ctl_pid=
	fail "ring-3 owner run failed"
fi
ctl_pid=

grep -q 'state=3 mode=0 ' "$outdir/ctl.log" || \
	fail "owner did not complete in Linux mode"
grep -Eq 'samples=[1-9][0-9]* ' "$outdir/ctl.log" || \
	fail "owner completed without workload samples"
grep -q 'recovery_state=0 ' "$outdir/ctl.log" || \
	fail "unexpected owner recovery state"

attempt=0
while [ "$attempt" -lt 100 ]; do
	status=$(cat "$probe_dir/status")
	completed=$(field "$status" completed)
	[ "$completed" = "$callback_count" ] && break
	sleep 0.1
	attempt=$((attempt + 1))
done
[ "$completed" = "$callback_count" ] || \
	fail "timed out waiting for all callbacks to complete"
printf '%s\n' "$status" >"$outdir/probe.log"
queued=$(field "$status" queued)
completed=$(field "$status" completed)
target_callbacks=$(field "$status" target_callbacks)
callback_first=$(field "$status" callback_first_ns)
target_first=$(field "$status" target_first_ns)
callback_cpus=$(field "$status" callback_cpus)
active_start=$(field "$(cat "$outdir/ctl.log")" user_active_start_ns)
active_end=$(field "$(cat "$outdir/ctl.log")" user_active_end_ns)

[ "$queued" = "$callback_count" ] || \
	fail "queued callback count was $queued, expected $callback_count"
[ "$completed" = "$callback_count" ] || \
	fail "only $completed of $callback_count callbacks completed"
[ -n "$callback_cpus" ] || fail "callback execution CPU mask is empty"
[ "$active_start" -gt 0 ] && [ "$active_end" -gt "$active_start" ] || \
	fail "owner interval timestamps are invalid"
[ "$callback_first" -ge "$active_start" ] || \
	fail "callbacks completed before the recorded owner interval"

if [ "$target_callbacks" -gt 0 ]; then
	[ "$target_first" -ge "$active_end" ] || \
		fail "$target_callbacks callbacks ran on owner CPU $target_cpu during ownership"
fi

if [ "$callback_first" -lt "$active_end" ]; then
	route=ran-off-owner-during-ownership
else
	route=no-callback-completed-during-ownership
fi

echo "route=$route queued=$queued completed=$completed target_callbacks=$target_callbacks callback_cpus=$callback_cpus"
echo "owner_start_ns=$active_start owner_end_ns=$active_end callback_first_ns=$callback_first target_first_ns=$target_first"
cat "$outdir/ctl.log"
echo "PASS: no RCU callback ran on the owned CPU during ring-3 ownership"
echo "logs: $outdir"
