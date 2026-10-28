#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
rescue=${CPU_ACCEL_ESCAPE_RESCUE:-$script_dir/test-user-escape-rescue}
target_cpu=${CPU_ACCEL_ESCAPE_CPU:-2}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-250}
duration_ms=5000
drop_parameter=/sys/module/cpu_accel/parameters/user_escape_drop_count
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-escape-fault.XXXXXX)}
saved_drop_count=
drop_count_saved=0
dmesg_pid=
rescue_pid=
owner_started=0
owner_finished=0

fail()
{
	echo "cpu_accel user escape fault test: $* (logs: $outdir)" >&2
	[ ! -f "$outdir/ctl.log" ] || cat "$outdir/ctl.log" >&2
	[ ! -f "$outdir/kernel-errors.log" ] || cat "$outdir/kernel-errors.log" >&2
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -n "$rescue_pid" ]; then
		if [ "$owner_started" -eq 1 ] && [ "$owner_finished" -eq 0 ]; then
			wait "$rescue_pid" 2>/dev/null || true
		else
			kill -TERM "$rescue_pid" 2>/dev/null || true
			wait "$rescue_pid" 2>/dev/null || true
		fi
		rescue_pid=
	fi
	if [ "$drop_count_saved" -eq 1 ]; then
		if ! printf '%s\n' "$saved_drop_count" >"$drop_parameter"; then
			echo "cpu_accel user escape fault test: failed to restore $drop_parameter" >&2
			status=1
		fi
		drop_count_saved=0
	fi
	exit "$status"
}

trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -x "$rescue" ] || fail "NMI rescue helper is not executable: $rescue"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -r "$drop_parameter" ] && [ -w "$drop_parameter" ] || \
	fail "user_escape_drop_count fault-injection parameter is unavailable"
case "$target_cpu:$escape_ms" in
*[!0-9:]*|0:*|*:0) fail "CPU and escape delay must be positive integers" ;;
esac
[ "$target_cpu" -ne 0 ] || fail "target CPU must differ from the CPU0 escape controller"
[ "$escape_ms" -lt 3000 ] || \
	fail "escape delay must leave time for the independent rescue helper"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

saved_drop_count=$(cat "$drop_parameter")
case "$saved_drop_count" in
''|*[!0-9]*) fail "unexpected drop parameter value: $saved_drop_count" ;;
esac
drop_count_saved=1
echo 1 >"$drop_parameter"

dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!
"$rescue" 5000 >"$outdir/rescue.log" 2>&1 &
rescue_pid=$!
sleep 0.05

# The independent rescue helper issues a clean NMI after five seconds if the
# retry path fails. The injected drop should make the second attempt win first.
owner_started=1
if ! "$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
	--period-us 1000 --workload user-hang --escape-after-ms "$escape_ms" \
	--escape-retries 2 >"$outdir/ctl.log" 2>&1; then
	owner_finished=1
	fail "bounded retry run failed"
fi
owner_finished=1

kill -TERM "$dmesg_pid" 2>/dev/null || true
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

grep -q 'state=7 mode=0 ' "$outdir/ctl.log" || \
	fail "fault-injected escape did not reach ESCAPED/Linux state"
grep -q 'recovery_state=2 recovery_error=0 recovery_attempts=2 ' \
	"$outdir/ctl.log" || fail "escape retry result was not reported exactly"
grep -q 'user_escape_count=1 ' "$outdir/ctl.log" || \
	fail "successful escape NMI was not counted exactly once"
grep -Eq 'user_active_ns=[1-9][0-9]* ' "$outdir/ctl.log" || \
	fail "owner active interval was not reported"
[ "$(cat "$drop_parameter")" = 0 ] || \
	fail "the injected NMI drop was not consumed exactly once"
nmi_count=$(grep -F 'Uhhuh. NMI received for unknown reason' \
	"$outdir/dmesg-new.log" | grep -F -c "on CPU $target_cpu." || true)
[ "$nmi_count" = 1 ] || \
	fail "expected one unclaimed injected NMI diagnostic on CPU $target_cpu"
[ "$(grep -F -c 'Dazed and confused, but trying to continue' \
	"$outdir/dmesg-new.log" || true)" = 1 ] || \
	fail "expected the matching unknown-NMI continuation diagnostic"

grep -Ei 'BUG:|Oops:|soft lockup|blocked for more than|RCU.*stall|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
[ ! -s "$outdir/kernel-errors.log" ] || \
	fail "kernel error appeared during the NMI retry test"

cat "$outdir/ctl.log"
echo "PASS: one dropped escape NMI was retried and one successful escape was counted"
echo "logs: $outdir"
