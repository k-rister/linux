#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

tool=${CPU_ACCELCTL:-./cpu-accelctl}
target_cpu=${CPU_ACCEL_CPU:-2}
watchdog_file=/proc/sys/kernel/watchdog
watchdog_mask_file=/proc/sys/kernel/watchdog_cpumask
online_file=/sys/devices/system/cpu/cpu${target_cpu}/online
old_mask=
mask_changed=0

fail()
{
	echo "cpu_accel: $*" >&2
	exit 1
}

restore_watchdog_mask()
{
	if [ "$mask_changed" -eq 1 ]; then
		printf '%s\n' "$old_mask" >"$watchdog_mask_file" || \
			echo "cpu_accel: failed to restore watchdog_cpumask" >&2
	fi
}

trap restore_watchdog_mask EXIT

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "control tool is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -w "$watchdog_mask_file" ] || fail "watchdog_cpumask is not writable"
[ -e "$online_file" ] || fail "CPU $target_cpu does not exist"
[ "$(cat "$online_file")" = 1 ] || fail "CPU $target_cpu is offline"

if [ "$(cat "$watchdog_file")" != 1 ]; then
	echo "SKIP: no lockup detector is enabled"
	exit 4
fi

old_mask=$(cat "$watchdog_mask_file")
mask_changed=1
printf '%s\n' "$target_cpu" >"$watchdog_mask_file"
[ "$(cat "$watchdog_mask_file")" = "$target_cpu" ] || \
	fail "watchdog_cpumask did not select CPU $target_cpu"

if output=$("$tool" run --cpu "$target_cpu" --duration-ms 20 \
	--period-us 1000 2>&1); then
	fail "accelerator entry succeeded while the lockup detector covered CPU $target_cpu"
fi

echo "$output"
case "$output" in
	*"Device or resource busy"*) ;;
	*) fail "expected -EBUSY for a watchdog-covered CPU" ;;
esac

echo "PASS: watchdog-covered CPU admission was rejected"
