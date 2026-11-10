#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_CPU:-2}
hotplug_cpu=${CPU_ACCEL_HOTPLUG_CPU:-}
hotplug_mode=${CPU_ACCEL_HOTPLUG_MODE:-up}
repeat_count=${CPU_ACCEL_REPEAT_COUNT:-4}
duration_ms=${CPU_ACCEL_DURATION_MS:-3000}
load_cpus=${CPU_ACCEL_LOAD_CPUS:-}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-hotplug-race.XXXXXX)}
summary="$outdir/summary.tsv"
load_pids=

fail()
{
	echo "cpu_accel hotplug-race test: $* (logs: $outdir)" >&2
	exit 1
}

stop_load()
{
	load_pid=
	for load_pid in $load_pids; do
		kill -TERM "$load_pid" 2>/dev/null || true
		wait "$load_pid" 2>/dev/null || true
	done
	load_pids=
}

cleanup()
{
	stop_load
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
case "$hotplug_mode" in
	up|down) ;;
	*) fail "hotplug mode must be 'up' or 'down'" ;;
esac
case "$target_cpu" in
	''|*[!0-9]*) fail "target CPU must be a nonnegative integer" ;;
esac
[ "$target_cpu" -ne 0 ] || fail "target CPU must differ from control CPU 0"
case "$repeat_count" in
	''|*[!0-9]*) fail "repeat count must be a positive integer" ;;
esac
[ "$repeat_count" -ge 1 ] && [ "$repeat_count" -le 32 ] || \
	fail "repeat count must be between 1 and 32"
case "$duration_ms" in
	''|*[!0-9]*) fail "duration must be an integer number of milliseconds" ;;
esac
[ "$duration_ms" -gt 500 ] && [ "$duration_ms" -le 5000 ] || \
	fail "duration must be between 501 and 5000 milliseconds"

target_online="/sys/devices/system/cpu/cpu$target_cpu/online"
[ -e "$target_online" ] || fail "target CPU $target_cpu has no online control"
[ "$(cat "$target_online")" = 1 ] || fail "target CPU $target_cpu is offline"

if [ "$hotplug_mode" = down ]; then
	hotplug_cpu=$target_cpu
	hotplug_online="$target_online"
else
	if [ -z "$hotplug_cpu" ]; then
		for online in /sys/devices/system/cpu/cpu[1-9]*/online; do
			[ -e "$online" ] || continue
			candidate=${online%/online}
			candidate=${candidate##*/cpu}
			if [ "$candidate" != "$target_cpu" ] &&
			   [ "$(cat "$online")" = 1 ]; then
				hotplug_cpu=$candidate
				break
			fi
		done
	fi
	case "$hotplug_cpu" in
		''|*[!0-9]*) fail "no online non-boot hotplug CPU is available" ;;
	esac
	[ "$hotplug_cpu" != "$target_cpu" ] || \
		fail "hotplug CPU must differ from target CPU"
	hotplug_online="/sys/devices/system/cpu/cpu$hotplug_cpu/online"
	[ -e "$hotplug_online" ] || fail "hotplug CPU $hotplug_cpu has no online control"
	[ "$(cat "$hotplug_online")" = 1 ] || fail "hotplug CPU $hotplug_cpu is offline"
fi

if [ -n "$load_cpus" ]; then
	for load_cpu in $(printf '%s' "$load_cpus" | tr ',' ' '); do
		case "$load_cpu" in
			''|*[!0-9]*) fail "load CPU list is invalid: $load_cpus" ;;
		esac
		[ "$load_cpu" -ne 0 ] || fail "load CPU list contains control CPU 0"
		[ "$load_cpu" != "$target_cpu" ] || \
			fail "load CPU list contains the target CPU"
		[ "$load_cpu" != "$hotplug_cpu" ] || \
			fail "load CPU list contains the hotplug CPU"
		load_online="/sys/devices/system/cpu/cpu$load_cpu/online"
		[ "$(cat "$load_online")" = 1 ] || \
			fail "load CPU $load_cpu is offline"
		taskset -c "$load_cpu" true >/dev/null 2>&1 || \
			fail "load CPU $load_cpu is unavailable"
		taskset -c "$load_cpu" sh -c 'while :; do :; done' &
		load_pids="$load_pids $!"
	done
fi

if [ "$hotplug_mode" = down ]; then
	child_script=test-hotplug-down-owner.sh
else
	child_script=test-stop-machine-inactive-owner.sh
fi

mkdir -p "$outdir"
printf 'run\texit\tuser_active_ns\thotplug_wait_ms\n' >"$summary"
iteration=1
while [ "$iteration" -le "$repeat_count" ]; do
	run_log="$outdir/run-$iteration.log"
	echo "== CPU-$hotplug_mode race $iteration/$repeat_count (log: $run_log)"
	set +e
	CPU_ACCEL_TESTCTL="$tool" CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_HOTPLUG_CPU="$hotplug_cpu" \
	CPU_ACCEL_DURATION_MS="$duration_ms" \
		sh "$script_dir/$child_script" \
		>"$run_log" 2>&1
	run_rc=$?
	set -e
	active_ns=$(sed -n 's/.*user_active_ns=\([0-9][0-9]*\).*/\1/p' \
		"$run_log" | tail -n 1)
	[ -n "$active_ns" ] || active_ns=-
	hotplug_wait_ms=$(sed -n 's/.*hotplug_wait_ms=\([0-9][0-9]*\).*/\1/p' \
		"$run_log" | tail -n 1)
	[ -n "$hotplug_wait_ms" ] || hotplug_wait_ms=-
	printf '%s\t%s\t%s\t%s\n' "$iteration" "$run_rc" "$active_ns" \
		"$hotplug_wait_ms" >>"$summary"
	if [ "$run_rc" -ne 0 ]; then
		tail -n 40 "$run_log" || true
		fail "CPU-up race iteration $iteration failed"
	fi
	[ "$(cat "$hotplug_online")" = 1 ] || \
		fail "hotplug CPU $hotplug_cpu remained offline after iteration $iteration"
	iteration=$((iteration + 1))
	sleep 0.1
done

stop_load
max_active_ns=$(awk -F '\t' '
	NR > 1 && $3 != "-" {
		if ($3 > max)
			max = $3
	}
	END { print max + 0 }' "$summary")
cat "$summary"
echo "PASS: $repeat_count CPU-$hotplug_mode races completed with optional concurrent load"
echo "max_user_active_ns=$max_active_ns"
echo "logs: $outdir"
