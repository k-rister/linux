#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
target_cpu=${CPU_ACCEL_USER_DEATH_CPU:-3}
duration_ms=${CPU_ACCEL_USER_DEATH_DURATION_MS:-5000}
period_us=${CPU_ACCEL_USER_DEATH_PERIOD_US:-1000}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-user-death.XXXXXX)}
active_marker="$outdir/active"
run_pid=
worker_pid=
dmesg_pid=
owner_started=0
manager_reaped=0

fail()
{
	echo "cpu_accel user-death owner test: $* (logs: $outdir)" >&2
	[ ! -f "$outdir/ctl.log" ] || cat "$outdir/ctl.log" >&2
	[ ! -f "$outdir/status.log" ] || cat "$outdir/status.log" >&2
	[ ! -f "$outdir/kernel-errors.log" ] || cat "$outdir/kernel-errors.log" >&2
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	if [ -n "$worker_pid" ]; then
		kill -KILL "$worker_pid" 2>/dev/null || true
	fi
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ "$owner_started" -eq 1 ]; then
		"$tool" reset >/dev/null 2>&1 || true
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
case "$target_cpu:$duration_ms:$period_us" in
*[!0-9:]*|0:*|*:0:*|*:0) fail "CPU, duration, and period must be positive integers" ;;
esac
[ "$target_cpu" -ne 0 ] || fail "target CPU must differ from the CPU0 control process"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"
for command in awk cat dmesg grep sleep; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done

dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

rm -f "$active_marker"
CPU_ACCEL_ACTIVE_MARKER="$active_marker" "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us "$period_us" --workload user-death \
	>"$outdir/ctl.log" 2>&1 &
run_pid=$!
owner_started=1

ready=0
for attempt in $(seq 1 500); do
	if [ -f "$active_marker" ]; then
		ready=1
		break
	fi
	kill -0 "$run_pid" 2>/dev/null || break
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry marker timed out"

for attempt in $(seq 1 500); do
	worker_children=$(cat "/proc/$run_pid/task/$run_pid/children" 2>/dev/null || true)
	[ -n "$worker_children" ] && break
	kill -0 "$run_pid" 2>/dev/null || break
	sleep 0.01
done
[ -n "$worker_children" ] || fail "could not identify the user-death worker"
set -- $worker_children
[ "$#" -eq 1 ] || fail "expected exactly one user-death worker"
worker_pid=$1
case "$worker_pid" in
''|*[!0-9]*) fail "invalid user-death worker PID: $worker_pid" ;;
esac

kill -KILL "$worker_pid"
for attempt in $(seq 1 1000); do
	if ! kill -0 "$run_pid" 2>/dev/null; then
		if wait "$run_pid"; then
			manager_rc=0
		else
			manager_rc=$?
		fi
		run_pid=
		manager_reaped=1
		break
	fi
	sleep 0.01
done
[ "$manager_reaped" -eq 1 ] || fail "manager did not exit after owner death"

status_line=$(grep -E '^state=5 mode=0 ' "$outdir/ctl.log" | tail -n 1)
[ -n "$status_line" ] || fail "owner death did not reconcile to ERROR/Linux"

grep -q 'recovery_state=5 recovery_error=-3 ' "$outdir/ctl.log" || \
	fail "owner death did not record recovery failure -ESRCH"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "owner death unexpectedly used an escape NMI"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the death test"

kill -TERM "$dmesg_pid" 2>/dev/null || true
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=
grep -Ei 'BUG:|Oops:|soft lockup|blocked for more than|RCU.*stall|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
[ ! -s "$outdir/kernel-errors.log" ] || \
	fail "kernel error appeared during the user-death test"

printf '%s\n' "$status_line"
echo "PASS: killed owner was reconciled without an escape NMI"
echo "logs: $outdir"
