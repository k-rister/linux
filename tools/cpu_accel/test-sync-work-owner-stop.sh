#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
prime_tool=${CPU_ACCEL_SYNC_WAIT_PRIME:-$script_dir/test-sync-wait-prime}
target_cpu=${CPU_ACCEL_CPU:-2}
work_cpu=${CPU_ACCEL_WORK_CPU:-3}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-4900}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_sync_wait_$$"
resize_instance="$trace_root/instances/cpu_accel_resize_$$"
slab_shrink=${CPU_ACCEL_SLAB_SHRINK:-/sys/kernel/slab/kmalloc-64/shrink}
cpuset_root=${CPU_ACCEL_CGROUP_ROOT:-/sys/fs/cgroup}
timer_cpu=${CPU_ACCEL_TIMER_CPU:-1}
netdev_a="ca$$_a"
netdev_b="ca$$_b"
netdev_ip_a=198.18.0.1
netdev_ip_b=198.18.0.2
rcu_sheaf_probe=${CPU_ACCEL_RCU_SHEAF_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_slub_rcu_sheaf_probe.ko}
rcu_sheaf_module=cpu_accel_slub_rcu_sheaf_probe
vmalloc_purge_probe=${CPU_ACCEL_VMALLOC_PURGE_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_vmalloc_purge_probe.ko}
vmalloc_purge_module=cpu_accel_vmalloc_purge_probe
vmalloc_purge_held_mb=${CPU_ACCEL_VMALLOC_PURGE_HELD_MB:-384}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-sync-wait.XXXXXX)}
selected_case=${CPU_ACCEL_TEST_CASE:-all}
trace_pid=
dmesg_pid=
prime_pid=
run_pid=
ping_pid=
netdev_created=0
rcu_sheaf_loaded=0
vmalloc_purge_loaded=0
timer_cgroup="$cpuset_root/cpu-accel-timer-$$"
timer_partition="$timer_cgroup/cpuset.cpus.partition"
timer_cgroup_created=0
cpuset_enabled_by_test=0
thp_enabled_path=/sys/kernel/mm/transparent_hugepage/enabled
thp_enabled_saved=
thp_enabled_changed=0

should_run_case()
{
	[ "$selected_case" = all ] || [ "$selected_case" = "$1" ]
}

fail()
{
	echo "cpu_accel synchronous-work owner-stop test: $* (logs: $outdir)" >&2
	if [ -f "$outdir/trace.log" ]; then
		grep -E 'owner_stop_request:|owner_exit_complete:' "$outdir/trace.log" \
			| tail -n 20 >&2 || true
	fi
	if [ -f "$outdir/kernel-errors.log" ]; then
		cat "$outdir/kernel-errors.log" >&2
	fi
	exit 1
}

cleanup()
{
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$ping_pid" ]; then
		kill -TERM "$ping_pid" 2>/dev/null || true
		wait "$ping_pid" 2>/dev/null || true
		ping_pid=
	fi
	if [ "$netdev_created" -eq 1 ]; then
		ip link del "$netdev_a" 2>/dev/null || true
		netdev_created=0
	fi
	if [ "$timer_cgroup_created" -eq 1 ]; then
		timeout --kill-after=2s 8s sh -c \
			"echo member > '$timer_partition'" 2>/dev/null || true
		timeout --kill-after=2s 8s rmdir "$timer_cgroup" 2>/dev/null || true
		timer_cgroup_created=0
	fi
	if [ "$cpuset_enabled_by_test" -eq 1 ]; then
		echo -cpuset >"$cpuset_root/cgroup.subtree_control" 2>/dev/null || true
		cpuset_enabled_by_test=0
	fi
	if [ "$rcu_sheaf_loaded" -eq 1 ]; then
		timeout --kill-after=2s 8s rmmod "$rcu_sheaf_module" 2>/dev/null || true
		rcu_sheaf_loaded=0
	fi
	if [ "$vmalloc_purge_loaded" -eq 1 ]; then
		timeout --kill-after=2s 8s rmmod "$vmalloc_purge_module" 2>/dev/null || true
		vmalloc_purge_loaded=0
	fi
	if [ -n "$prime_pid" ]; then
		kill -TERM "$prime_pid" 2>/dev/null || true
		wait "$prime_pid" 2>/dev/null || true
		prime_pid=
	fi
	if [ -n "$trace_pid" ]; then
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -d "$instance" ]; then
		echo 0 >"$instance/tracing_on" 2>/dev/null || true
		rmdir "$instance" 2>/dev/null || true
	fi
	if [ -d "$resize_instance" ]; then
		echo 0 >"$resize_instance/tracing_on" 2>/dev/null || true
		rmdir "$resize_instance" 2>/dev/null || true
	fi
	if [ "$thp_enabled_changed" -eq 1 ]; then
		timeout --kill-after=2s 8s sh -c \
			"echo '$thp_enabled_saved' > '$thp_enabled_path'" \
			2>/dev/null || true
		thp_enabled_changed=0
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
case "$selected_case" in
all|vmstat|lru|slub_shrink|ring_resize|timer_migration|net_backlog|rcu_sheaf|vmalloc_purge) ;;
*) fail "unknown test case: $selected_case" ;;
esac
[ -x "$tool" ] || fail "cpu-accelctl is not executable: $tool"
[ -x "$prime_tool" ] || fail "page-prime helper is not executable: $prime_tool"
[ -c /dev/cpu_accel ] || modprobe cpu_accel || \
	fail "could not load the cpu_accel module"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -w /proc/sys/vm/stat_refresh ] || fail "vm.stat_refresh is unavailable"
[ -w /proc/sys/vm/drop_caches ] || fail "drop_caches is unavailable"
[ -w "$slab_shrink" ] || fail "SLUB shrink control is unavailable: $slab_shrink"
if should_run_case rcu_sheaf; then
	[ -r "$rcu_sheaf_probe" ] || \
		fail "SLUB RCU sheaf probe is unavailable: $rcu_sheaf_probe"
fi
if should_run_case vmalloc_purge; then
	[ -r "$vmalloc_purge_probe" ] || \
		fail "vmalloc purge probe is unavailable: $vmalloc_purge_probe"
	for path in "$trace_root/events/workqueue/workqueue_queue_work/enable" \
		"$trace_root/events/workqueue/workqueue_execute_start/enable" \
		"$trace_root/events/vmalloc/free_vmap_area_noflush/enable" \
		"$trace_root/events/vmalloc/purge_vmap_area_lazy/enable"; do
		[ -e "$path" ] || fail "required vmalloc trace event is unavailable: $path"
	done
fi
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
grep -qw function_graph "$trace_root/available_tracers" || \
	fail "function_graph tracer is unavailable"
grep -qw x86_cpu_accel_user_enter "$trace_root/available_filter_functions" || \
	fail "owner entry function is not traceable"
[ "$(cat "/sys/devices/system/cpu/cpu$target_cpu/online")" = 1 ] || \
	fail "target CPU $target_cpu is offline"

for event in owner_stop_request owner_exit_complete; do
	[ -e "$trace_root/events/cpu_accel/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

for function in cpuset_partition_write wait_attach_done_lock update_prstate \
	cpuset_update_sd_hk_unlock hk_sd_workfn housekeeping_update \
	pci_probe_flush_workqueue mem_cgroup_flush_workqueue \
	vmstat_flush_workqueue workqueue_unbound_housekeeping_update \
	tmigr_isolated_exclude_cpumask tmigr_cpu_isolate tmigr_cpu_unisolate; do
	grep -qw "$function" "$trace_root/available_filter_functions" || \
		fail "timer migration trace function is unavailable: $function"
done

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
mkdir "$resize_instance"
resize_path="$resize_instance/per_cpu/cpu$target_cpu/buffer_size_kb"
[ -w "$resize_path" ] || fail "target CPU ring buffer size is unavailable"
case "$(cat "$resize_path")" in
128) resize_size=256 ;;
*) resize_size=128 ;;
esac
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$instance/events/cpu_accel/$event/enable"
done
if should_run_case vmalloc_purge; then
	echo 1 >"$instance/events/workqueue/workqueue_queue_work/enable"
	echo 1 >"$instance/events/workqueue/workqueue_execute_start/enable"
	echo 1 >"$instance/events/vmalloc/free_vmap_area_noflush/enable"
	echo 1 >"$instance/events/vmalloc/purge_vmap_area_lazy/enable"
fi
printf '%s\n' x86_cpu_accel_user_enter cpuset_partition_write \
	wait_attach_done_lock update_prstate cpuset_update_sd_hk_unlock hk_sd_workfn \
	housekeeping_update pci_probe_flush_workqueue mem_cgroup_flush_workqueue \
	vmstat_flush_workqueue workqueue_unbound_housekeeping_update \
	tmigr_isolated_exclude_cpumask tmigr_cpu_isolate tmigr_cpu_unisolate \
	>"$instance/set_ftrace_filter"
echo function_graph >"$instance/current_tracer"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
echo 1 >"$instance/tracing_on"
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

# khugepaged begins each scan with lru_add_drain_all(), which can stop the
# timer-test owner before timer migration reaches its own isolation hook.
# Disable THP while this test runs; cleanup restores the selected mode.
if should_run_case timer_migration && [ -r "$thp_enabled_path" ] && \
	[ -w "$thp_enabled_path" ]; then
	thp_enabled_state=$(cat "$thp_enabled_path")
	thp_enabled_saved=$(printf '%s\n' "$thp_enabled_state" |
		sed -n 's/.*\[\([^]]*\)\].*/\1/p')
	[ -n "$thp_enabled_saved" ] || \
		fail "could not read the selected transparent hugepage mode"
	if [ "$thp_enabled_saved" != never ]; then
		thp_enabled_changed=1
		echo never >"$thp_enabled_path" || \
			fail "could not disable transparent hugepages for the owner-stop test"
	fi
	case "$(cat "$thp_enabled_path")" in
	*'[never]'*) ;;
	*) fail "transparent hugepages did not enter never mode" ;;
	esac
	echo "INFO: transparent hugepages temporarily disabled (previous mode: $thp_enabled_saved)"
fi

run_case()
{
	name=$1
	trigger_target=$2
	expected_caller=$3
	write_value=${4:-1}
	trigger_type=${5:-sysfs}
	case_cpu=${6:-$target_cpu}
	owner_cgroup=${7:-}
	entry_count=$(grep -Fc 'x86_cpu_accel_user_enter();' "$outdir/trace.log" || true)

	if [ -n "$owner_cgroup" ]; then
		taskset -c "$work_cpu" "$tool" run --cpu "$case_cpu" \
			--duration-ms 5000 --period-us 1000 --workload user-hang \
			--escape-after-ms "$escape_ms" --escape-retries 1 \
			--worker-cgroup "$owner_cgroup" \
			>"$outdir/$name-ctl.log" 2>&1 &
	else
		"$tool" run --cpu "$case_cpu" \
			--duration-ms 5000 --period-us 1000 \
			--workload user-hang --escape-after-ms "$escape_ms" \
			--escape-retries 1 >"$outdir/$name-ctl.log" 2>&1 &
	fi
	run_pid=$!
	ready=0
	for n in $(seq 1 300); do
		count=$(grep -Fc 'x86_cpu_accel_user_enter();' "$outdir/trace.log" || true)
		if [ "$count" -gt "$entry_count" ]; then
			ready=1
			break
		fi
		sleep 0.01
	done
	[ "$ready" -eq 1 ] || fail "$name owner-entry trace timed out"

	start_ns=$(date +%s%N)
	if [ "$trigger_type" = rmmod ]; then
		if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
			rmmod "$trigger_target" >"$outdir/$name-trigger.log" 2>&1; then
			helper_rc=0
			rcu_sheaf_loaded=0
		else
			helper_rc=$?
		fi
	elif [ "$trigger_type" = netdev ]; then
		taskset -c "$work_cpu" ping -n -q -f -w 8 -I "$netdev_b" \
			"$netdev_ip_a" >"$outdir/$name-ping.log" 2>&1 &
		ping_pid=$!
		sleep 0.1
		if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
			ip link del "$trigger_target" >"$outdir/$name-trigger.log" 2>&1; then
			helper_rc=0
			netdev_created=0
		else
			helper_rc=$?
		fi
		kill -TERM "$ping_pid" 2>/dev/null || true
		wait "$ping_pid" 2>/dev/null || true
		ping_pid=
	else
		if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
			sh -c "echo '$write_value' > '$trigger_target'" >"$outdir/$name-trigger.log" 2>&1; then
			helper_rc=0
		else
			helper_rc=$?
		fi
	fi
	if [ "$name" = timer_migration ] && [ "$helper_rc" -eq 0 ]; then
		partition_state=$(cat "$trigger_target")
		[ "$partition_state" = isolated ] || \
			fail "timer migration partition state is '$partition_state'"
	fi
	end_ns=$(date +%s%N)
	helper_elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
	if wait "$run_pid"; then
		run_rc=0
	else
		run_rc=$?
	fi
	run_pid=

	[ "$helper_rc" -eq 0 ] || fail "$name trigger failed or timed out"
	[ "$run_rc" -eq 0 ] || fail "$name owner run failed"
	grep -q 'state=7 ' "$outdir/$name-ctl.log" || \
		fail "$name owner did not report ESCAPED"
	grep -q 'recovery_state=2' "$outdir/$name-ctl.log" || \
		fail "$name owner escape recovery did not complete"
	max_helper_elapsed_ms=2000
	if [ "$name" = timer_migration ]; then
		[ "$escape_ms" -gt "$max_helper_elapsed_ms" ] || \
			fail "timer escape timeout must exceed 2000 ms"
	fi
	[ "$helper_elapsed_ms" -lt "$max_helper_elapsed_ms" ] || \
		fail "$name trigger waited ${helper_elapsed_ms} ms (limit ${max_helper_elapsed_ms} ms) for timed owner escape"

	if ! awk -v target="$case_cpu" -v caller="$expected_caller" '
function number(line, name, rest, pos)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	rest = substr(line, pos + length(name) + 1)
	sub(/[^0-9].*$/, "", rest)
	return rest
}
/owner_stop_request:/ {
	if (number($0, "cpu") == target && index($0, "caller=" caller) &&
	    index($0, "stop_cb=1") && index($0, "sent=1")) {
		owner = number($0, "owner")
		stop_line = NR
	}
}
/owner_exit_complete:/ {
	if (number($0, "cpu") == target && number($0, "owner") == owner &&
	    index($0, "stop_requested=1"))
		exit_line = NR
}
END {
	exit !(owner != "" && stop_line && exit_line > stop_line)
}' "$outdir/trace.log"; then
		fail "$name trace did not pair owner stop with owner exit"
	fi
	if [ "$name" = vmalloc_purge ]; then
		grep -Eq 'workqueue_queue_work:.*function=purge_vmap_node .*req_cpu=' \
			"$outdir/trace.log" || \
			fail "vmalloc purge did not queue a purge_vmap_node worker"
		grep -Eq 'workqueue_execute_start:.*function purge_vmap_node' \
			"$outdir/trace.log" || \
			fail "vmalloc purge did not execute a purge_vmap_node worker"
	fi
	echo "PASS: $name stopped CPU $case_cpu owner in ${helper_elapsed_ms} ms"
}

if should_run_case vmstat; then
	run_case vmstat /proc/sys/vm/stat_refresh schedule_on_each_cpu
fi
if should_run_case lru; then
	# Prime immediately before drop_caches so an earlier case cannot drain it.
	"$prime_tool" "$target_cpu" 8 >"$outdir/prime.log" 2>&1 &
	prime_pid=$!
	ready=0
	for n in $(seq 1 300); do
		if grep -q '^ready ' "$outdir/prime.log"; then
			ready=1
			break
		fi
		sleep 0.01
	done
	[ "$ready" -eq 1 ] || fail "target CPU LRU pages were not primed"
	run_case lru /proc/sys/vm/drop_caches __lru_add_drain_all
fi
if should_run_case slub_shrink; then
	run_case slub_shrink "$slab_shrink" flush_all_cpus_locked
fi
if should_run_case ring_resize; then
	run_case ring_resize "$resize_path" ring_buffer_resize "$resize_size"
fi

if should_run_case timer_migration; then
	[ -r "$cpuset_root/cgroup.controllers" ] || \
		fail "cgroup v2 is required for timer migration test"
	grep -qw cpuset "$cpuset_root/cgroup.controllers" || \
		fail "cpuset controller is unavailable for timer migration test"
	[ -d "/sys/devices/system/cpu/cpu$timer_cpu" ] || \
		fail "timer test CPU $timer_cpu does not exist"
	[ "$(cat "/sys/devices/system/cpu/cpu$timer_cpu/online")" = 1 ] || \
		fail "timer test CPU $timer_cpu is offline"
	[ "$timer_cpu" -ne "$target_cpu" ] && [ "$timer_cpu" -ne "$work_cpu" ] || \
		fail "timer test CPU must differ from the owner and work CPUs"
	case " $(cat "$cpuset_root/cgroup.subtree_control") " in
	*" cpuset "*) ;;
	*)
		echo +cpuset >"$cpuset_root/cgroup.subtree_control" || \
			fail "could not enable cpuset for timer migration test"
		cpuset_enabled_by_test=1
		;;
	esac
	mkdir "$timer_cgroup" || fail "could not create timer migration test cgroup"
	timer_cgroup_created=1
	echo "$timer_cpu" >"$timer_cgroup/cpuset.cpus"
	timer_mems=$(cat "$cpuset_root/cpuset.mems.effective")
	echo "$timer_mems" >"$timer_cgroup/cpuset.mems"
	timer_stop_trigger="$instance/events/cpu_accel/owner_stop_request/trigger"
	if [ "${CPU_ACCEL_TIMER_TRACE_STACK:-0}" -eq 1 ]; then
		echo stacktrace:1 >"$timer_stop_trigger"
	fi
	run_case timer_migration "$timer_partition" \
		housekeeping_update isolated sysfs "$timer_cpu" "$timer_cgroup"
	if [ "${CPU_ACCEL_TIMER_TRACE_STACK:-0}" -eq 1 ]; then
		echo '!stacktrace:1' >"$timer_stop_trigger"
	fi
	echo member >"$timer_partition"
	rmdir "$timer_cgroup"
	timer_cgroup_created=0
	if [ "$cpuset_enabled_by_test" -eq 1 ]; then
		echo -cpuset >"$cpuset_root/cgroup.subtree_control"
		cpuset_enabled_by_test=0
	fi
fi

if should_run_case net_backlog; then
	[ "$target_cpu" -lt 31 ] || fail "RPS test requires target CPU below 31"
	command -v ip >/dev/null 2>&1 || fail "iproute2 is required for backlog test"
	command -v ping >/dev/null 2>&1 || fail "iputils ping is required for backlog test"
	modprobe veth || fail "veth module is unavailable for backlog test"
	ip link add "$netdev_a" type veth peer name "$netdev_b" || \
		fail "could not create veth pair for backlog test"
	netdev_created=1
	ip addr add "$netdev_ip_a/30" dev "$netdev_a"
	ip addr add "$netdev_ip_b/30" dev "$netdev_b"
	ip link set "$netdev_a" up
	ip link set "$netdev_b" up
	netdev_mac_a=$(cat "/sys/class/net/$netdev_a/address")
	ip neigh replace "$netdev_ip_a" lladdr "$netdev_mac_a" nud permanent dev "$netdev_b"
	rps_path="/sys/class/net/$netdev_a/queues/rx-0/rps_cpus"
	set_rps_cpu()
	{
		candidate_cpu=$1
		rps_mask=$(printf '%x' "$((1 << candidate_cpu))")
		if echo "$rps_mask" >"$rps_path" 2>/dev/null; then
			return 0
		fi
		return 1
	}

	net_cpu=${CPU_ACCEL_NET_CPU:-$target_cpu}
	if [ "$net_cpu" -eq "$work_cpu" ] || ! set_rps_cpu "$net_cpu"; then
		net_cpu=
		for online_path in /sys/devices/system/cpu/cpu[1-9]*/online \
			/sys/devices/system/cpu/cpu0/online; do
			[ -r "$online_path" ] || continue
			[ "$(cat "$online_path")" = 1 ] || continue
			candidate_cpu=${online_path#/sys/devices/system/cpu/cpu}
			candidate_cpu=${candidate_cpu%/online}
			[ "$candidate_cpu" -eq "$work_cpu" ] && continue
			if set_rps_cpu "$candidate_cpu"; then
				net_cpu=$candidate_cpu
				break
			fi
		done
	fi
	[ -n "$net_cpu" ] || fail "no online housekeeping CPU is available for RPS backlog test"
	run_case net_backlog "$netdev_a" flush_all_backlogs 1 netdev "$net_cpu"
fi

if should_run_case rcu_sheaf; then
	[ ! -e "/sys/module/$rcu_sheaf_module" ] || \
		fail "SLUB RCU sheaf probe is already loaded"
	insmod "$rcu_sheaf_probe" target_cpu="$target_cpu" \
		>"$outdir/rcu-sheaf-insmod.log" 2>&1 || \
		fail "SLUB RCU sheaf probe load failed"
	rcu_sheaf_loaded=1
	run_case rcu_sheaf "$rcu_sheaf_module" flush_rcu_sheaves_on_cache 1 rmmod
fi

if should_run_case vmalloc_purge; then
	[ ! -e "/sys/module/$vmalloc_purge_module" ] || \
		fail "vmalloc purge probe is already loaded"
	insmod "$vmalloc_purge_probe" target_cpu="$target_cpu" work_cpu="$work_cpu" \
		held_mb="$vmalloc_purge_held_mb" \
		>"$outdir/vmalloc-purge-insmod.log" 2>&1 || \
		fail "vmalloc purge probe load failed"
	vmalloc_purge_loaded=1
	run_case vmalloc_purge "$vmalloc_purge_module" \
		kernel_tlb_flush_all 1 rmmod "$target_cpu"
	vmalloc_purge_loaded=0
fi

echo 0 >"$instance/tracing_on"
kill -TERM "$trace_pid" "$dmesg_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

grep -Ei 'soft lockup|task .* blocked for more than|RCU.*stall|(^|[[:space:]])BUG:|Oops:|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

if [ "$selected_case" = all ]; then
	echo "PASS: synchronous work, LRU drain, SLUB/RCU flushes, network backlog, vmalloc purge, and ring resize complete with active-owner stops"
else
	echo "PASS: $selected_case stopped the active owner"
fi
echo "logs: $outdir"
