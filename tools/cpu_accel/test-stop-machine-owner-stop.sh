#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
probe_dir=$script_dir/stop-machine-probe
probe=${CPU_ACCEL_STOP_MACHINE_PROBE:-$probe_dir/cpu_accel_stop_machine_probe.ko}
target_cpu=${CPU_ACCEL_CPU:-2}
duration_ms=${CPU_ACCEL_DURATION_MS:-5000}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-stop-machine-fault.XXXXXX)}
active_marker=$outdir/active-marker
fire_param=/sys/module/cpu_accel_stop_machine_probe/parameters/fire
inject_error_param=/sys/module/cpu_accel_stop_machine_probe/parameters/inject_error
escape_after_ms=$((duration_ms - 500))
max_active_ns=$((duration_ms * 500000))
run_pid=
module_loaded=0
trace_dir=
trace_probe_group=

fail()
{
	echo "cpu_accel stop-machine fault test: $*" >&2
	exit 1
}

cleanup()
{
	status=$?
	trap - EXIT
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ "$module_loaded" -eq 1 ]; then
		rmmod cpu_accel_stop_machine_probe 2>/dev/null || true
		module_loaded=0
	fi
	if [ -n "$trace_dir" ] && [ -d "$trace_dir" ]; then
		echo 0 >"$trace_dir/tracing_on" 2>/dev/null || true
		echo nop >"$trace_dir/current_tracer" 2>/dev/null || true
		for event in sm_entry sm_return maint_begin maint_return \
			    cpu_lock cpu_unlock maint_end; do
			echo 0 >"$trace_dir/events/$trace_probe_group/$event/enable" \
				2>/dev/null || true
		done
		for event in owner_stop_request owner_exit_complete; do
			echo 0 >"$trace_dir/events/cpu_accel/$event/enable" \
				2>/dev/null || true
		done
		rmdir "$trace_dir" 2>/dev/null || true
	fi
	if [ -n "$trace_probe_group" ]; then
		for event in sm_entry sm_return maint_begin maint_return \
			    cpu_lock cpu_unlock maint_end; do
			printf '%s\n' "-:$trace_probe_group/$event" \
				>>"$trace_root/kprobe_events" 2>/dev/null || true
		done
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -f "$probe" ] || fail "stop-machine probe module is unavailable: $probe"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
command -v dmesg >/dev/null 2>&1 || fail "dmesg is required"
command -v insmod >/dev/null 2>&1 || fail "insmod is required"
command -v rmmod >/dev/null 2>&1 || fail "rmmod is required"
case "$target_cpu" in
	''|*[!0-9]*) fail "target CPU must be a nonnegative integer" ;;
esac
[ "$target_cpu" -ne 0 ] || fail "target CPU must differ from control CPU 0"
case "$duration_ms" in
	''|*[!0-9]*) fail "duration must be an integer number of milliseconds" ;;
esac
[ "$duration_ms" -gt 500 ] && [ "$duration_ms" -le 10000 ] || \
	fail "duration must be between 501 and 10000 milliseconds"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

trace_root=/sys/kernel/tracing
[ -d "$trace_root" ] || trace_root=/sys/kernel/debug/tracing
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
[ -w "$trace_root/instances" ] || fail "tracefs instances are not writable"
[ -w "$trace_root/kprobe_events" ] || fail "tracefs kprobe events are unavailable"
trace_dir=$trace_root/instances/cpu-accel-stop-machine-fault-$$
mkdir "$trace_dir" || fail "could not create a private trace instance"
echo 0 >"$trace_dir/tracing_on"
trace_probe_group=cpu_accel_stop_machine_fault_$$
printf 'p:%s/sm_entry stop_machine\n' "$trace_probe_group" \
	>>"$trace_root/kprobe_events" || \
	fail "could not register stop-machine entry probe"
printf 'r:%s/sm_return stop_machine retval=$retval\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register stop-machine return probe"
printf 'p:%s/maint_begin x86_cpu_accel_maintenance_begin\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register maintenance-begin entry probe"
printf 'r:%s/maint_return x86_cpu_accel_maintenance_begin\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register maintenance-begin return probe"
printf 'p:%s/cpu_lock cpus_read_lock\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register CPU-hotplug lock probe"
printf 'p:%s/cpu_unlock cpus_read_unlock\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register CPU-hotplug unlock probe"
printf 'p:%s/maint_end x86_cpu_accel_maintenance_end\n' \
	"$trace_probe_group" >>"$trace_root/kprobe_events" || \
	fail "could not register maintenance-end probe"
for event in sm_entry sm_return maint_begin maint_return \
	    cpu_lock cpu_unlock maint_end; do
	event_path=$trace_dir/events/$trace_probe_group/$event
	[ -e "$event_path/enable" ] || \
		fail "stop-machine $event event is unavailable in trace instance"
	echo "common_pid == $$" >"$event_path/filter" || \
		fail "could not filter stop-machine $event event"
	echo 1 >"$event_path/enable"
done
for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_dir/events/cpu_accel/$event/enable" ] || \
		fail "CPU accelerator $event tracepoint is unavailable"
	echo 1 >"$trace_dir/events/cpu_accel/$event/enable"
done

insmod "$probe" target_cpu="$target_cpu" inject_error=0
module_loaded=1
[ -w "$fire_param" ] || fail "stop-machine fire parameter is not writable"
[ -w "$inject_error_param" ] || \
	fail "stop-machine inject-error parameter is not writable"

wait_for_active()
{
	active_wait=0
	while [ ! -s "$active_marker" ]; do
		kill -0 "$run_pid" 2>/dev/null || {
			cat "$1"
			fail "owner run ended before entering ring 3"
		}
		[ "$active_wait" -lt 300 ] || {
			cat "$1"
			fail "owner did not enter ring 3 within 3 seconds"
		}
		sleep 0.01
		active_wait=$((active_wait + 1))
	done
	[ "$(cat "$active_marker")" = active ] || \
		fail "owner active marker has unexpected contents"
}

run_case()
{
	case_name=$1
	expected_error=$2
	run_output=$outdir/run-$case_name.log
	fire_output=$outdir/fire-$case_name.log
	trace_output=$outdir/trace-$case_name.log
	dmesg_output=$outdir/dmesg-$case_name.log
	expected_ret=0

	if [ "$expected_error" -eq 1 ]; then
		expected_ret=-5
	fi
	echo "$expected_error" >"$inject_error_param"

	echo >"$trace_dir/trace"
	echo 1 >"$trace_dir/tracing_on"
	rm -f "$active_marker"
	CPU_ACCEL_ACTIVE_MARKER="$active_marker" \
		"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
		--period-us 1000 --workload user-cyclictest \
		--escape-after-ms "$escape_after_ms" >"$run_output" 2>&1 &
	run_pid=$!
	wait_for_active "$run_output"

	set +e
	echo 1 >"$fire_param" 2>"$fire_output"
	fire_rc=$?
	set -e
	if ! wait "$run_pid"; then
		run_pid=
		cat "$run_output" "$fire_output"
		fail "$case_name owner run failed"
	fi
	run_pid=
	echo 0 >"$trace_dir/tracing_on"
	cat "$trace_dir/trace" >"$trace_output"
	dmesg | grep 'cpu_accel_stop_machine_probe:' | tail -n 1 \
		>"$dmesg_output"

	if [ "$expected_error" -eq 0 ] && [ "$fire_rc" -ne 0 ]; then
		cat "$run_output" "$fire_output" "$dmesg_output"
		fail "$case_name stop-machine trigger failed"
	fi
	if [ "$fire_rc" -ne 0 ]; then
		cat "$run_output" "$fire_output" "$dmesg_output"
		fail "$case_name stop-machine trigger failed"
	fi

	grep -q '^state=7 ' "$run_output" || \
		fail "$case_name stop-machine did not escape the active ring-3 owner"
	grep -q 'mode=0' "$run_output" || \
		fail "$case_name stopped owner did not return to Linux mode"
	grep -q 'recovery_state=2 recovery_error=0' "$run_output" || \
		fail "$case_name owner did not recover cleanly after the NMI escape"
	grep -q 'user_escape_count=1' "$run_output" || \
		fail "$case_name owner did not record exactly one NMI escape"
	active_ns=$(sed -n 's/.*user_active_ns=\([0-9][0-9]*\).*/\1/p' \
		"$run_output" | tail -n 1)
	[ -n "$active_ns" ] || \
		fail "$case_name owner run did not report ring-3 active time"
	[ "$active_ns" -lt "$max_active_ns" ] || \
		fail "$case_name owner exceeded the stop-machine deadline"

	probe_line=$(cat "$dmesg_output")
	[ -n "$probe_line" ] || fail "$case_name probe result is missing from dmesg"
	probe_ret=$(printf '%s\n' "$probe_line" |
		sed -n 's/.*ret=\([^ ]*\).*/\1/p')
	callback_count=$(printf '%s\n' "$probe_line" |
		sed -n 's/.*callback_count=\([^ ]*\).*/\1/p')
	online_cpus=$(printf '%s\n' "$probe_line" |
		sed -n 's/.*online_cpus=\([^ ]*\).*/\1/p')
	[ "$probe_ret" = "$expected_ret" ] || \
		fail "$case_name stop-machine returned $probe_ret, expected $expected_ret"
	[ -n "$callback_count" ] && [ -n "$online_cpus" ] || \
		fail "$case_name probe result is missing callback or online CPU counts"
	[ "$callback_count" = "$online_cpus" ] || \
		fail "$case_name stop-machine callback did not run on every online CPU"
	grep -q "target_owner_before=1 " "$dmesg_output" || \
		fail "$case_name stop-machine began without the target owner active"
	grep -q "target_owner_after=0 " "$dmesg_output" || \
		fail "$case_name stop-machine finished with the target owner still active"
	grep -q "any_owner_after=0" "$dmesg_output" || \
		fail "$case_name stop-machine finished with an accelerator owner still active"
	grep -q "target_cpu=$target_cpu " "$dmesg_output" || \
		fail "$case_name probe result reports the wrong target CPU"

	awk '
	/sm_entry:/ && !entry { entry = NR }
	/maint_begin:/ && !maint_begin { maint_begin = NR }
	/owner_stop_request:/ && !stop_request { stop_request = NR }
	/owner_exit_complete:/ && !owner_exit { owner_exit = NR }
	/maint_return:/ && !maint_return { maint_return = NR }
	/cpu_lock:/ && !cpu_lock { cpu_lock = NR }
	/sm_return:/ && !machine_return { machine_return = NR }
	/cpu_unlock:/ && !cpu_unlock { cpu_unlock = NR }
	/maint_end:/ && !maint_end { maint_end = NR }
	END {
		if (!(maint_begin && stop_request && owner_exit && maint_return &&
		      cpu_lock && entry && machine_return && cpu_unlock && maint_end &&
		      entry < maint_begin && maint_begin < stop_request &&
		      stop_request < owner_exit &&
		      owner_exit < maint_return && maint_return < cpu_lock &&
		      cpu_unlock < maint_end && maint_end < machine_return))
			exit 1
	}' "$trace_output" || {
		cat "$trace_output"
		fail "$case_name trace did not prove lock-order and owner exit"
	}

	cat "$run_output"
	cat "$dmesg_output"
	echo "cpu_accel: $case_name stop-machine fault case passed"
}

run_case success 0
run_case fault 1

echo "PASS: stop-machine owner drain and fault propagation passed"
echo "logs: $outdir"
