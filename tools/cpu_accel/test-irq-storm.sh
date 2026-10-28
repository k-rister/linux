#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-1}
interface=${CPU_ACCEL_IRQ_INTERFACE:-}
storm_host=${CPU_ACCEL_IRQ_STORM_HOST:-}
duration=${CPU_ACCEL_IRQ_STORM_SECONDS:-3}
packets=${CPU_ACCEL_IRQ_STORM_PACKETS:-1000}
run_pid=0
state_dir=$(mktemp -d)
run_output=$state_dir/run.log
ping_output=$state_dir/ping.log

fail()
{
	echo "cpu_accel: $*" >&2
	exit 1
}

cleanup()
{
	if [ "$run_pid" -ne 0 ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
	fi
	rm -rf "$state_dir"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root to generate a bounded ping flood"
[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -n "$interface" ] || fail "set CPU_ACCEL_IRQ_INTERFACE to the test NIC"
[ -n "$storm_host" ] || fail "set CPU_ACCEL_IRQ_STORM_HOST to a reachable peer"
command -v ping >/dev/null 2>&1 || fail "ping is required"
command -v taskset >/dev/null 2>&1 || fail "taskset is required"

case "$target_cpu" in
	''|*[!0-9]*) fail "CPU_ACCEL_CPU must be a non-negative integer" ;;
esac
case "$duration" in
	''|*[!0-9]*) fail "CPU_ACCEL_IRQ_STORM_SECONDS must be an integer" ;;
esac
case "$packets" in
	''|*[!0-9]*) fail "CPU_ACCEL_IRQ_STORM_PACKETS must be an integer" ;;
esac
[ "$duration" -ge 1 ] && [ "$duration" -le 4 ] || \
	fail "CPU_ACCEL_IRQ_STORM_SECONDS must be between 1 and 4"
[ "$packets" -ge 1 ] && [ "$packets" -le 5000 ] || \
	fail "CPU_ACCEL_IRQ_STORM_PACKETS must be between 1 and 5000"
traffic_cpu=0
[ "$target_cpu" -ne 0 ] || traffic_cpu=1
traffic_online_file=/sys/devices/system/cpu/cpu${traffic_cpu}/online
if [ "$traffic_cpu" -ne 0 ]; then
	[ -r "$traffic_online_file" ] && \
		[ "$(cat "$traffic_online_file")" = 1 ] || \
		fail "traffic CPU $traffic_cpu is not online"
fi
taskset -c "$traffic_cpu" true || \
	fail "traffic CPU $traffic_cpu is not available to taskset"

online_file=/sys/devices/system/cpu/cpu${target_cpu}/online
[ -r "$online_file" ] && [ "$(cat "$online_file")" = 1 ] || \
	fail "target CPU $target_cpu is not online"

device=$(readlink -f "/sys/class/net/$interface/device") || \
	fail "cannot resolve NIC device for $interface"
irq_dir=
candidate=$device
while [ "$candidate" != /sys ] && [ "$candidate" != / ]; do
	if [ -d "$candidate/msi_irqs" ]; then
		irq_dir=$candidate/msi_irqs
		break
	fi
	candidate=$(dirname "$candidate")
done
[ -n "$irq_dir" ] || fail "no MSI IRQ directory found for $interface"

irqs=
irq_count=0
for irq_file in "$irq_dir"/*; do
	[ -e "$irq_file" ] || continue
	irq=${irq_file##*/}
	case "$irq" in
		''|*[!0-9]*) fail "unexpected IRQ entry: $irq_file" ;;
	esac
	[ -r "/proc/irq/$irq/effective_affinity_list" ] || \
		fail "effective affinity is unavailable for IRQ $irq"
	irqs="$irqs $irq"
	irq_count=$((irq_count + 1))
done
[ "$irq_count" -gt 0 ] || fail "no MSI IRQs found for $interface"

cpu_list_contains()
{
	printf '%s\n' "$1" | awk -F, -v cpu="$target_cpu" '
	{
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^[0-9]+-[0-9]+$/) {
				split($i, range, "-")
				if (cpu >= range[1] && cpu <= range[2])
					exit 0
			} else if ($i == cpu) {
				exit 0
			}
		}
		exit 1
	}'
}

irq_cpu_counts()
{
	awk -v irq="$1:" -v wanted_cpu="$target_cpu" '
	NR == 1 {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^CPU[0-9]+$/) {
				cpu_columns++
				cpu = $i
				sub(/^CPU/, "", cpu)
				if (cpu == wanted_cpu)
					target_column = i + 1
			}
		}
		next
	}
	$1 == irq {
		for (i = 2; i <= cpu_columns + 1; i++) {
			count = $i + 0
			total += count
			if (i == target_column)
				target_count = count
		}
		found = 1
		printf "%.0f %.0f\n", total, target_count
		exit
	}
	END {
		if (!found || !target_column)
			exit 1
	}' /proc/interrupts
}

initial_target_irq=0
for irq in $irqs; do
	affinity=$(cat "/proc/irq/$irq/effective_affinity_list")
	if cpu_list_contains "$affinity"; then
		initial_target_irq=1
	fi
done
[ "$initial_target_irq" -eq 1 ] || \
	fail "NIC IRQs were already off CPU $target_cpu; cannot verify quarantine routing"

"$tool" run --cpu "$target_cpu" --duration-ms 5000 --period-us 1000 \
	--workload timestamp --persistent --quarantine-irqs \
	>"$run_output" 2>&1 &
run_pid=$!

ready=0
tries=0
while [ "$tries" -lt 200 ]; do
	if ! kill -0 "$run_pid" 2>/dev/null; then
		cat "$run_output" >&2
		fail "accelerator owner exited before the IRQ stress"
	fi
	ready=1
	for irq in $irqs; do
		affinity=$(cat "/proc/irq/$irq/effective_affinity_list")
		if cpu_list_contains "$affinity"; then
			ready=0
			break
		fi
	done
	[ "$ready" -eq 1 ] && break
	sleep 0.01
	tries=$((tries + 1))
done
[ "$ready" -eq 1 ] || {
	cat "$run_output" >&2
	fail "NIC IRQs were not moved off CPU $target_cpu"
}

for irq in $irqs; do
	counts=$(irq_cpu_counts "$irq") || fail "IRQ $irq is absent from /proc/interrupts"
	before_total=${counts%% *}
	before_target=${counts#* }
	printf '%s %s\n' "$before_total" "$before_target" >"$state_dir/$irq.before"
done

ping_status=0
taskset -c "$traffic_cpu" ping -f -c "$packets" -w "$duration" "$storm_host" \
	>"$ping_output" 2>&1 || ping_status=$?

kill -0 "$run_pid" 2>/dev/null || {
	cat "$run_output" >&2
	cat "$ping_output" >&2
	fail "accelerator owner exited during the IRQ stress"
}

network_irq_delta=0
target_irq_delta=0
for irq in $irqs; do
	counts=$(irq_cpu_counts "$irq") || fail "IRQ $irq disappeared from /proc/interrupts"
	after_total=${counts%% *}
	after_target=${counts#* }
	read before_total before_target <"$state_dir/$irq.before"
	irq_delta=$((after_total - before_total))
	target_delta=$((after_target - before_target))
	[ "$irq_delta" -ge 0 ] || fail "IRQ $irq counter moved backwards"
	[ "$target_delta" -ge 0 ] || fail "CPU $target_cpu IRQ $irq counter moved backwards"
	network_irq_delta=$((network_irq_delta + irq_delta))
	target_irq_delta=$((target_irq_delta + target_delta))
	echo "cpu_accel: nic_irq=$irq delta=$irq_delta cpu${target_cpu}_delta=$target_delta"
done

kill -TERM "$run_pid" 2>/dev/null || true
if ! wait "$run_pid"; then
	run_pid=0
	cat "$run_output" >&2
	cat "$ping_output" >&2
	fail "accelerator owner did not stop cleanly"
fi
run_pid=0

cat "$run_output"
cat "$ping_output"
[ "$ping_status" -eq 0 ] || \
	fail "bounded ping flood failed with status $ping_status"
[ "$network_irq_delta" -gt 0 ] || \
	fail "ping flood caused no MSI IRQ delivery on $interface"
[ "$target_irq_delta" -eq 0 ] || \
	fail "NIC MSI IRQs were delivered to accelerator CPU $target_cpu"
grep -q '^state=4 ' "$run_output" || \
	fail "accelerator workload did not report STOP completion"
grep -q 'mode=0' "$run_output" || \
	fail "accelerator workload did not return to Linux mode"
grep -q 'irq_count=0 ' "$run_output" || \
	fail "an IRQ reached the accelerator CPU during the measured interval"
grep -q 'irq_quarantined=[1-9][0-9]*' "$run_output" || \
	fail "IRQ quarantine did not move any IRQs"
grep -q 'irq_quarantine_blockers=0' "$run_output" || \
	fail "IRQ quarantine reported blockers"

echo "cpu_accel: NIC IRQ stress passed (interface=$interface target_cpu=$target_cpu packets=$packets timeout_s=$duration msi_irqs=$irq_count irq_delta=$network_irq_delta target_irq_delta=$target_irq_delta ping_status=$ping_status)"
