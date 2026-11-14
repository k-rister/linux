#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
prime_tool=${CPU_ACCEL_SYNC_WAIT_PRIME:-$script_dir/test-sync-wait-prime}
target_cpu=${CPU_ACCEL_CPU:-2}
work_cpu=${CPU_ACCEL_WORK_CPU:-3}
duration_ms=${CPU_ACCEL_DURATION_MS:-5000}
escape_ms=${CPU_ACCEL_ESCAPE_AFTER_MS:-$((duration_ms - 100))}
trace_root=${TRACEFS:-/sys/kernel/tracing}
event_instance="$trace_root/instances/cpu_accel_sync_wait_events_$$"
graph_instance="$trace_root/instances/cpu_accel_sync_wait_graph_$$"
resize_instance="$trace_root/instances/cpu_accel_resize_$$"
slab_shrink=${CPU_ACCEL_SLAB_SHRINK:-/sys/kernel/slab/kmalloc-64/shrink}
cpuset_root=${CPU_ACCEL_CGROUP_ROOT:-/sys/fs/cgroup}
timer_cpu=${CPU_ACCEL_TIMER_CPU:-1}
rearm_cpu=${CPU_ACCEL_REARM_CPU:-5}
vmalloc_trigger_cpu=${CPU_ACCEL_VMALLOC_TRIGGER_CPU:-4}
netdev_a="ca$$_a"
netdev_b="ca$$_b"
netns="ca$$_ns"
netdev_ip_a=198.18.0.1
netdev_ip_b=198.18.0.2
netdev_port=19000
net_recv_tool=${CPU_ACCEL_NET_RECV_TOOL:-$script_dir/net-backlog-rfs-receiver.pl}
net_send_tool=${CPU_ACCEL_NET_SEND_TOOL:-$script_dir/net-backlog-rfs-sender.pl}
rcu_sheaf_probe=${CPU_ACCEL_RCU_SHEAF_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_slub_rcu_sheaf_probe.ko}
rcu_sheaf_module=cpu_accel_slub_rcu_sheaf_probe
srcu_cleanup_probe=${CPU_ACCEL_SRCU_CLEANUP_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_srcu_cleanup_probe.ko}
srcu_cleanup_module=cpu_accel_srcu_cleanup_probe
vmalloc_purge_probe=${CPU_ACCEL_VMALLOC_PURGE_PROBE:-$script_dir/tlb-flush-probe/cpu_accel_vmalloc_purge_probe.ko}
vmalloc_purge_module=cpu_accel_vmalloc_purge_probe
vmalloc_purge_held_mb=${CPU_ACCEL_VMALLOC_PURGE_HELD_MB:-384}
sync_work_flush_probe=${CPU_ACCEL_SYNC_WORK_FLUSH_PROBE:-}
[ -n "$sync_work_flush_probe" ] || \
	sync_work_flush_probe="$script_dir/tlb-flush-probe/cpu_accel_sync_work_flush_probe.ko"
sync_work_flush_module=cpu_accel_sync_work_flush_probe
sync_work_flush_loaded=0
debugfs_root=${DEBUGFS:-/sys/kernel/debug}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-sync-wait.XXXXXX)}
selected_case=${CPU_ACCEL_TEST_CASE:-all}
event_trace_pid=
graph_trace_pid=
dmesg_pid=
prime_pid=
run_pid=
rearm_pid=
net_recv_pid=
net_send_pid=
rps_sock_flow_entries_saved=
net_kprobe_suffix=$$
net_kprobes_created=0
netdev_created=0
netns_created=0
net_firewalld_added=0
rcu_sheaf_loaded=0
srcu_cleanup_loaded=0
vmalloc_purge_loaded=0
timer_cgroup="$cpuset_root/cpu-accel-timer-$$"
timer_partition="$timer_cgroup/cpuset.cpus.partition"
timer_cgroup_created=0
cpuset_enabled_by_test=0
event_instance_created=0
graph_instance_created=0
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
	if [ -f "$outdir/owner-trace.log" ]; then
		grep -E 'owner_stop_request:|owner_exit_complete:' "$outdir/owner-trace.log" \
			| tail -n 20 >&2 || true
	fi
	if [ -f "$outdir/kernel-errors.log" ]; then
		cat "$outdir/kernel-errors.log" >&2
	fi
	exit 1
}

cleanup()
{
	if [ -n "$rearm_pid" ]; then
		kill -TERM "$rearm_pid" 2>/dev/null || true
		wait "$rearm_pid" 2>/dev/null || true
		rearm_pid=
	fi
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
	fi
	if [ -n "$net_send_pid" ]; then
		kill -CONT "$net_send_pid" 2>/dev/null || true
		kill -TERM "$net_send_pid" 2>/dev/null || true
		wait "$net_send_pid" 2>/dev/null || true
		net_send_pid=
	fi
	if [ -n "$net_recv_pid" ]; then
		kill -TERM "$net_recv_pid" 2>/dev/null || true
		wait "$net_recv_pid" 2>/dev/null || true
		net_recv_pid=
	fi
	if [ -n "$rps_sock_flow_entries_saved" ]; then
		echo "$rps_sock_flow_entries_saved" \
			>/proc/sys/net/core/rps_sock_flow_entries 2>/dev/null || true
		rps_sock_flow_entries_saved=
	fi
	if [ "$net_firewalld_added" -eq 1 ]; then
		firewall-cmd --zone=trusted --remove-interface="$netdev_a" \
			>/dev/null 2>&1 || true
		net_firewalld_added=0
	fi
	if [ "$netns_created" -eq 1 ]; then
		ip netns del "$netns" 2>/dev/null || true
		netns_created=0
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
	if [ "$srcu_cleanup_loaded" -eq 1 ]; then
		timeout --kill-after=2s 8s rmmod "$srcu_cleanup_module" 2>/dev/null || true
		srcu_cleanup_loaded=0
	fi
	if [ "$vmalloc_purge_loaded" -eq 1 ]; then
		timeout --kill-after=2s 8s rmmod "$vmalloc_purge_module" 2>/dev/null || true
		vmalloc_purge_loaded=0
	fi
	if [ "$sync_work_flush_loaded" -eq 1 ]; then
		timeout --kill-after=2s 8s rmmod "$sync_work_flush_module" 2>/dev/null || true
		sync_work_flush_loaded=0
	fi
	if [ -n "$prime_pid" ]; then
		kill -TERM "$prime_pid" 2>/dev/null || true
		wait "$prime_pid" 2>/dev/null || true
		prime_pid=
	fi
	if [ -n "$graph_trace_pid" ]; then
		echo 0 >"$graph_instance/tracing_on" 2>/dev/null || true
		kill -TERM "$graph_trace_pid" 2>/dev/null || true
		wait "$graph_trace_pid" 2>/dev/null || true
		graph_trace_pid=
		echo >"$graph_instance/set_ftrace_filter" 2>/dev/null || true
		echo nop >"$graph_instance/current_tracer" 2>/dev/null || true
	fi
	if [ -n "$event_trace_pid" ]; then
		echo 0 >"$event_instance/tracing_on" 2>/dev/null || true
		kill -TERM "$event_trace_pid" 2>/dev/null || true
		wait "$event_trace_pid" 2>/dev/null || true
		event_trace_pid=
	fi
	"$tool" stop >"$outdir/cleanup-stop.log" 2>&1 || true
	"$tool" exit >"$outdir/cleanup-exit.log" 2>&1 || true
	"$tool" reset >"$outdir/cleanup-reset.log" 2>&1 || true
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ "$graph_instance_created" -eq 1 ]; then
		rmdir "$graph_instance" 2>/dev/null || true
		graph_instance_created=0
	fi
	if [ "$event_instance_created" -eq 1 ]; then
		rmdir "$event_instance" 2>/dev/null || true
		event_instance_created=0
	fi
	if [ "$net_kprobes_created" -eq 1 ]; then
		for event in "net_enqueue_$net_kprobe_suffix" \
			"net_get_rps_cpu_$net_kprobe_suffix" \
			"net_queue_work_$net_kprobe_suffix" \
			"net_flush_backlog_$net_kprobe_suffix"; do
			echo "-:$event" >>"$trace_root/kprobe_events" 2>/dev/null || true
		done
		net_kprobes_created=0
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
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
case "$selected_case" in
all|vmstat|lru|slub_shrink|ring_resize|timer_migration|net_backlog|\
rcu_sheaf|srcu_cleanup|vmalloc_purge|vmalloc_purge_wait|sync_work_flush|\
sync_workqueue_flush) ;;
*) fail "unknown test case: $selected_case" ;;
esac
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
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
if should_run_case srcu_cleanup; then
	[ -r "$srcu_cleanup_probe" ] || \
		fail "SRCU cleanup probe is unavailable: $srcu_cleanup_probe"
	for path in "$trace_root/events/workqueue/workqueue_queue_work/enable" \
		"$trace_root/events/workqueue/workqueue_execute_start/enable"; do
		[ -e "$path" ] || fail "required SRCU cleanup trace event is unavailable: $path"
	done
	grep -qw cleanup_srcu_struct "$trace_root/available_filter_functions" || \
		fail "SRCU cleanup function is not traceable"
fi
if should_run_case vmalloc_purge || should_run_case vmalloc_purge_wait; then
	[ -r "$vmalloc_purge_probe" ] || \
		fail "vmalloc purge probe is unavailable: $vmalloc_purge_probe"
	for path in "$trace_root/events/workqueue/workqueue_queue_work/enable" \
		"$trace_root/events/workqueue/workqueue_execute_start/enable" \
		"$trace_root/events/vmalloc/free_vmap_area_noflush/enable" \
		"$trace_root/events/vmalloc/purge_vmap_area_lazy/enable"; do
		[ -e "$path" ] || fail "required vmalloc trace event is unavailable: $path"
	done
fi
if should_run_case sync_work_flush || should_run_case sync_workqueue_flush; then
	[ -r "$sync_work_flush_probe" ] || \
		fail "sync-work flush probe is unavailable: $sync_work_flush_probe"
	[ -d "$debugfs_root" ] || fail "debugfs is unavailable: $debugfs_root"
	for path in "$trace_root/events/workqueue/workqueue_queue_work/enable" \
		"$trace_root/events/workqueue/workqueue_execute_start/enable"; do
		[ -e "$path" ] || \
			fail "required sync-work flush trace event is unavailable: $path"
	done
fi
if should_run_case vmalloc_purge_wait; then
	[ -d "/sys/devices/system/cpu/cpu$rearm_cpu" ] && \
		[ "$(cat "/sys/devices/system/cpu/cpu$rearm_cpu/online")" = 1 ] || \
		fail "rearm CPU $rearm_cpu is offline or unavailable"
	[ -d "/sys/devices/system/cpu/cpu$vmalloc_trigger_cpu" ] && \
		[ "$(cat "/sys/devices/system/cpu/cpu$vmalloc_trigger_cpu/online")" = 1 ] || \
		fail "vmalloc trigger CPU $vmalloc_trigger_cpu is offline or unavailable"
	[ "$rearm_cpu" -ne "$target_cpu" ] && \
		[ "$rearm_cpu" -ne "$work_cpu" ] && \
		[ "$rearm_cpu" -ne "$vmalloc_trigger_cpu" ] || \
		fail "rearm CPU must differ from the owner and work CPUs"
	[ "$vmalloc_trigger_cpu" -ne "$target_cpu" ] && \
		[ "$vmalloc_trigger_cpu" -ne "$work_cpu" ] || \
		fail "vmalloc trigger CPU must differ from the owner and work CPUs"
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
	tmigr_isolated_exclude_cpumask tmigr_cpu_isolate tmigr_cpu_unisolate \
	arch_smp_sync_wait_begin arch_smp_sync_wait_stop arch_smp_sync_wait_end \
	x86_cpu_accel_request_stop_owner_gen; do
	grep -qw "$function" "$trace_root/available_filter_functions" || \
		fail "timer migration trace function is unavailable: $function"
done

mkdir "$event_instance"
event_instance_created=1
echo 4096 >"$event_instance/buffer_size_kb"
echo nop >"$event_instance/current_tracer"
mkdir "$graph_instance"
graph_instance_created=1
echo 4096 >"$graph_instance/buffer_size_kb"

mkdir "$resize_instance"
resize_path="$resize_instance/per_cpu/cpu$target_cpu/buffer_size_kb"
[ -w "$resize_path" ] || fail "target CPU ring buffer size is unavailable"
case "$(cat "$resize_path")" in
128) resize_size=256 ;;
*) resize_size=128 ;;
esac
for event in owner_stop_request owner_exit_complete; do
	echo 1 >"$event_instance/events/cpu_accel/$event/enable"
done
if should_run_case net_backlog; then
	for function in enqueue_to_backlog get_rps_cpu queue_work_on flush_backlog; do
		grep -qw "$function" "$trace_root/available_filter_functions" || \
			fail "net_backlog trace function is unavailable: $function"
	done
	echo "p:net_enqueue_$net_kprobe_suffix enqueue_to_backlog cpu=\$arg2" \
		>>"$trace_root/kprobe_events"
	net_kprobes_created=1
	echo "r:net_get_rps_cpu_$net_kprobe_suffix get_rps_cpu cpu=\$retval" \
		>>"$trace_root/kprobe_events"
	echo "p:net_queue_work_$net_kprobe_suffix queue_work_on cpu=\$arg1 work=\$arg3" \
		>>"$trace_root/kprobe_events"
	echo "p:net_flush_backlog_$net_kprobe_suffix flush_backlog" \
		>>"$trace_root/kprobe_events"
	for event in "net_enqueue_$net_kprobe_suffix" \
		"net_get_rps_cpu_$net_kprobe_suffix" \
		"net_queue_work_$net_kprobe_suffix" \
		"net_flush_backlog_$net_kprobe_suffix"; do
		echo 1 >"$event_instance/events/kprobes/$event/enable"
	done
fi
owner_stop_trigger="$event_instance/events/cpu_accel/owner_stop_request/trigger"
[ -w "$owner_stop_trigger" ] || \
	fail "owner-stop trace trigger is unavailable"
echo stacktrace:1 >"$owner_stop_trigger"
if should_run_case srcu_cleanup || should_run_case vmalloc_purge || \
	should_run_case vmalloc_purge_wait || should_run_case sync_work_flush || \
	should_run_case sync_workqueue_flush; then
	echo 1 >"$event_instance/events/workqueue/workqueue_queue_work/enable"
	echo 1 >"$event_instance/events/workqueue/workqueue_execute_start/enable"
fi
if should_run_case vmalloc_purge || should_run_case vmalloc_purge_wait; then
	echo 1 >"$event_instance/events/vmalloc/free_vmap_area_noflush/enable"
	echo 1 >"$event_instance/events/vmalloc/purge_vmap_area_lazy/enable"
fi
if should_run_case sync_work_flush || should_run_case sync_workqueue_flush; then
	insmod "$sync_work_flush_probe" target_cpu="$target_cpu" || \
		fail "could not load the sync-work flush probe"
	sync_work_flush_loaded=1
	sync_work_flush_path="$debugfs_root/cpu_accel_sync_work_flush/run"
	sync_workqueue_flush_path="$debugfs_root/cpu_accel_sync_work_flush/run_workqueue"
	[ -w "$sync_work_flush_path" ] || \
		fail "sync-work flush trigger is unavailable: $sync_work_flush_path"
	if should_run_case sync_workqueue_flush; then
		[ -w "$sync_workqueue_flush_path" ] || \
			fail "sync-workqueue flush trigger unavailable: $sync_workqueue_flush_path"
		grep -qw cpu_accel_flush_workqueue \
			"$trace_root/available_filter_functions" || \
			fail "cpu_accel_flush_workqueue is not traceable"
	fi
	if should_run_case sync_work_flush; then
		grep -qw flush_work \
			"$trace_root/available_filter_functions" || \
			fail "flush_work is not traceable"
	fi
	grep -qw cpu_accel_sync_work_flush_probe_fn \
		"$trace_root/available_filter_functions" || \
		fail "sync-work flush probe callback is not traceable"
fi
printf '%s\n' x86_cpu_accel_user_enter cpuset_partition_write \
	wait_attach_done_lock update_prstate cpuset_update_sd_hk_unlock hk_sd_workfn \
	cleanup_srcu_struct \
	housekeeping_update pci_probe_flush_workqueue mem_cgroup_flush_workqueue \
	vmstat_flush_workqueue workqueue_unbound_housekeeping_update \
	tmigr_isolated_exclude_cpumask tmigr_cpu_isolate tmigr_cpu_unisolate \
	arch_smp_sync_wait_begin arch_smp_sync_wait_stop arch_smp_sync_wait_end \
	x86_cpu_accel_request_stop_owner_gen \
	>"$graph_instance/set_ftrace_filter"
if should_run_case lru; then
	grep -qw lru_add_drain_all \
		"$trace_root/available_filter_functions" || \
		fail "LRU drain caller is not traceable"
	printf '%s\n' lru_add_drain_all >>"$graph_instance/set_ftrace_filter"
fi
if should_run_case vmalloc_purge_wait; then
	printf '%s\n' __purge_vmap_area_lazy purge_vmap_node flush_work \
		>>"$graph_instance/set_ftrace_filter"
fi
if should_run_case sync_work_flush; then
	printf '%s\n' flush_work \
		>>"$graph_instance/set_ftrace_filter"
fi
if should_run_case sync_workqueue_flush; then
	printf '%s\n' cpu_accel_flush_workqueue \
		>>"$graph_instance/set_ftrace_filter"
fi
if should_run_case sync_work_flush || should_run_case sync_workqueue_flush; then
	printf '%s\n' cpu_accel_sync_work_flush_probe_fn \
		>>"$graph_instance/set_ftrace_filter"
fi
if should_run_case net_backlog; then
	grep -qw flush_all_backlogs \
		"$trace_root/available_filter_functions" || \
		fail "flush_all_backlogs is not traceable"
	printf '%s\n' flush_all_backlogs >>"$graph_instance/set_ftrace_filter"
fi

echo function_graph >"$graph_instance/current_tracer"
echo 0 >"$event_instance/tracing_on"
echo 0 >"$graph_instance/tracing_on"
cat "$event_instance/trace_pipe" >"$outdir/owner-trace.log" &
event_trace_pid=$!
cat "$graph_instance/trace_pipe" >"$outdir/function-graph.log" &
graph_trace_pid=$!
echo 1 >"$event_instance/tracing_on"
echo 1 >"$graph_instance/tracing_on"
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
	case_escape_ms=$escape_ms
	active_marker="$outdir/$name-active"
	if [ "$name" = vmalloc_purge_wait ]; then
		case_escape_ms=100
	fi
	rm -f "$active_marker"

	if [ "$name" = timer_migration ]; then
		CPU_ACCEL_ACTIVE_MARKER="$active_marker" "$tool" run \
			--cpu "$case_cpu" --duration-ms "$duration_ms" --period-us 1000 \
			--workload user-hang --escape-after-ms "$case_escape_ms" \
			--escape-retries 1 --worker-cgroup "$owner_cgroup" \
			>"$outdir/$name-ctl.log" 2>&1 &
	elif [ -n "$owner_cgroup" ]; then
		CPU_ACCEL_ACTIVE_MARKER="$active_marker" \
			taskset -c "$work_cpu" "$tool" run --cpu "$case_cpu" \
			--duration-ms "$duration_ms" --period-us 1000 --workload user-hang \
			--escape-after-ms "$case_escape_ms" --escape-retries 1 \
			--worker-cgroup "$owner_cgroup" \
			>"$outdir/$name-ctl.log" 2>&1 &
	else
		CPU_ACCEL_ACTIVE_MARKER="$active_marker" "$tool" run \
			--cpu "$case_cpu" \
			--duration-ms "$duration_ms" --period-us 1000 \
			--workload user-hang --escape-after-ms "$case_escape_ms" \
			--escape-retries 1 >"$outdir/$name-ctl.log" 2>&1 &
	fi
	run_pid=$!
	ready=0
	for n in $(seq 1 300); do
		if [ -s "$active_marker" ] &&
		   [ "$(cat "$active_marker")" = active ]; then
			ready=1
		fi
		if [ "$ready" -eq 1 ]; then
			ready=1
			break
		fi
		sleep 0.01
	done
	[ "$ready" -eq 1 ] || fail "$name owner activation timed out"
	if [ "$name" = vmalloc_purge_wait ]; then
		(
			for attempt in $(seq 1 400); do
				if taskset -c "$rearm_cpu" "$tool" run --cpu "$case_cpu" \
					--duration-ms 5000 --period-us 1000 \
					--workload user-hang --escape-after-ms 500 \
					--escape-retries 1 \
					>"$outdir/$name-rearm-ctl.log" 2>&1; then
					exit 0
				fi
				grep -q 'open /dev/cpu_accel: Device or resource busy' \
					"$outdir/$name-rearm-ctl.log" || exit 1
				sleep 0.005
			done
			echo "rearm device open stayed busy after $attempt attempts" \
				>>"$outdir/$name-rearm.log"
			exit 1
		) &
		rearm_pid=$!
	fi

	start_ns=$(date +%s%N)
	if [ "$trigger_type" = rmmod ]; then
		trigger_cpu=$work_cpu
		[ "$name" = vmalloc_purge_wait ] && \
			trigger_cpu=$vmalloc_trigger_cpu
		if timeout --kill-after=2s 8s taskset -c "$trigger_cpu" \
			rmmod "$trigger_target" >"$outdir/$name-trigger.log" 2>&1; then
			helper_rc=0
			case "$trigger_target" in
				cpu_accel_slub_rcu_sheaf_probe) rcu_sheaf_loaded=0 ;;
				cpu_accel_srcu_cleanup_probe) srcu_cleanup_loaded=0 ;;
				cpu_accel_vmalloc_purge_probe) vmalloc_purge_loaded=0 ;;
			esac
		else
			helper_rc=$?
		fi
	elif [ "$trigger_type" = netdev ]; then
		kill -USR1 "$net_send_pid" || \
			fail "$name could not uncork the network sender"
		sleep 0.1
		if timeout --kill-after=2s 8s taskset -c "$work_cpu" \
			ip link del "$trigger_target" >"$outdir/$name-trigger.log" 2>&1; then
			helper_rc=0
			netdev_created=0
		else
			helper_rc=$?
		fi
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
	if [ "$name" = vmalloc_purge_wait ]; then
		if wait "$rearm_pid"; then
			rearm_rc=0
		else
			rearm_rc=$?
		fi
		rearm_pid=
		[ "$rearm_rc" -eq 0 ] || \
			fail "vmalloc rearm owner failed (see $outdir/$name-rearm-ctl.log)"
		grep -q 'state=7 ' "$outdir/$name-rearm-ctl.log" || \
			fail "vmalloc rearm owner did not report ESCAPED"
		grep -q 'recovery_state=2' "$outdir/$name-rearm-ctl.log" || \
			fail "vmalloc rearm owner recovery did not complete"
	fi

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

	# Keep trace events in an instance using the nop tracer. Mixing them with
	# function_graph rewrites the event caller as return_to_handler.
	if ! awk -v target="$case_cpu" -v caller="$expected_caller" \
		'
function field(line, name, value, pos)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	value = substr(line, pos + length(name) + 1)
	sub(/[^[:alnum:]_].*$/, "", value)
	return value
}
function caller_matches(line, expected, actual, parts, count, i)
{
	actual = field(line, "caller")
	count = split(expected, parts, "|")
	for (i = 1; i <= count; i++) {
		if (actual == parts[i])
			return 1
	}
	return 0
}
function stack_matches(line, expected, parts, count, i)
{
	count = split(expected, parts, "|")
	for (i = 1; i <= count; i++) {
		if (index(line, "=> " parts[i]))
			return 1
	}
	return 0
}
/owner_stop_request:/ {
	pending = 0
	stack_active = 0
	if (field($0, "cpu") == target) {
		owner = ""
		stop_line = 0
		if (field($0, "stop_cb") == "1" && field($0, "sent") == "1") {
			this_owner = field($0, "owner")
			if (caller_matches($0, caller)) {
				owner = this_owner
				stop_line = NR
			} else if (field($0, "caller") == "return_to_handler") {
				pending = 1
				pending_owner = this_owner
				pending_line = NR
			}
		}
	} else {
		owner = ""
		stop_line = 0
	}
}
/<stack trace>/ {
	if (pending)
		stack_active = 1
	next
}
{
	if (pending && stack_active) {
		if (stack_matches($0, caller)) {
			owner = pending_owner
			stop_line = pending_line
			pending = 0
			stack_active = 0
		} else if ($0 !~ /=>/) {
			pending = 0
			stack_active = 0
		}
	}
}
/owner_exit_complete:/ {
	if (field($0, "cpu") == target && field($0, "owner") == owner &&
	    field($0, "stop_requested") == "1")
		exit_line = NR
}
END {
	exit !(owner != "" && stop_line && exit_line > stop_line)
}' "$outdir/owner-trace.log"; then
		fail "$name trace did not pair owner stop with owner exit"
	fi
	if [ "$name" = net_backlog ]; then
		target_hex=$(printf '%x' "$case_cpu")
		flush_cpu="[$(printf '%03d' "$case_cpu")]"
		sender_task="perl-$net_send_pid"
		grep -q '^received 1 bytes$' "$outdir/net_backlog-recv.log" || \
			fail "net_backlog receiver did not establish the TCP flow"
		grep -q '^uncorked$' "$outdir/net_backlog-send.log" || \
			fail "net_backlog sender did not uncork the held packet"
		grep -Eq "$sender_task.*net_get_rps_cpu_$net_kprobe_suffix:.*cpu=0x$target_hex$" \
			"$outdir/owner-trace.log" || \
			fail "net_backlog RFS did not select CPU $case_cpu"
		grep -Eq "$sender_task.*net_enqueue_$net_kprobe_suffix:.*cpu=0x$target_hex$" \
			"$outdir/owner-trace.log" || \
			fail "net_backlog did not enqueue the held packet on CPU $case_cpu"
		grep -Eq "net_queue_work_$net_kprobe_suffix:.*cpu=0x$target_hex " \
			"$outdir/owner-trace.log" || \
			fail "net_backlog did not queue a flush worker on CPU $case_cpu"
		grep "net_flush_backlog_$net_kprobe_suffix:" "$outdir/owner-trace.log" |
			grep -F "$flush_cpu" >/dev/null || \
			fail "net_backlog did not execute a flush worker on CPU $case_cpu"
	fi
	if [ "$name" = vmalloc_purge ]; then
		grep -Eq 'workqueue_queue_work:.*function=purge_vmap_node .*req_cpu=' \
			"$outdir/owner-trace.log" || \
			fail "vmalloc purge did not queue a purge_vmap_node worker"
		grep -Eq 'workqueue_execute_start:.*function purge_vmap_node' \
			"$outdir/owner-trace.log" || \
			fail "vmalloc purge did not execute a purge_vmap_node worker"
	fi
	if [ "$name" = vmalloc_purge_wait ]; then
		grep -q 'reentry_seen=1 reentry_timed_out=0' \
			"$outdir/dmesg-new.log" || \
			fail "vmalloc TLB probe did not observe owner re-entry"
		# The compiler can tail-call the arch hook, leaving the vmalloc
		# callsite in the stop trace's caller field.
		if ! awk -v target="$case_cpu" '
function field(line, name, pos, value)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	value = substr(line, pos + length(name) + 1)
	sub(/[^[:alnum:]_].*$/, "", value)
	return value
}
/owner_stop_request:/ && field($0, "cpu") == target &&
	(field($0, "caller") == "arch_smp_sync_wait_stop" ||
	 field($0, "caller") == "__purge_vmap_area_lazy") &&
	field($0, "stop_cb") == "1" && field($0, "sent") == "1" {
	owner = field($0, "owner")
	stop_line = NR
}
/owner_exit_complete:/ && field($0, "cpu") == target &&
	field($0, "owner") == owner && field($0, "stop_requested") == "1" {
	exit_line = NR
}
END { exit !(owner != "" && stop_line && exit_line > stop_line) }
' "$outdir/owner-trace.log"; then
			fail "vmalloc wait did not stop and exit the rearmed owner"
		fi
		if ! awk -v target="$case_cpu" '
function field(line, name, pos, value)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	value = substr(line, pos + length(name) + 1)
	sub(/[^[:alnum:]_].*$/, "", value)
	return value
}
/workqueue_queue_work:/ && index($0, "function=purge_vmap_node") &&
	field($0, "req_cpu") == target {
	work = $0
	sub(/^.*work struct=/, "", work)
	sub(/[[:space:]].*$/, "", work)
}
/workqueue_execute_start:/ && work != "" &&
	index($0, "work struct " work ":") &&
	index($0, "function purge_vmap_node") { executed = 1 }
END { exit !(work != "" && executed) }
' "$outdir/owner-trace.log"; then
			fail "vmalloc wait did not execute the target CPU purge worker"
		fi
	fi
	if [ "$name" = srcu_cleanup ]; then
		if ! awk -v target="$case_cpu" '
function field(line, name, pos, value)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	value = substr(line, pos + length(name) + 1)
	sub(/[^0-9].*$/, "", value)
	return value
}
/workqueue_queue_work:/ && index($0, "function=srcu_invoke_callbacks") {
	if (field($0, "req_cpu") == target) {
		work = $0
		sub(/^.*work struct=/, "", work)
		sub(/[[:space:]].*$/, "", work)
	}
}
/workqueue_execute_start:/ && work != "" &&
	index($0, "work struct " work ":") &&
	index($0, "function srcu_invoke_callbacks") {
	executed = 1
}
END {
	exit !(work != "" && executed)
}' "$outdir/owner-trace.log"; then
			fail "SRCU cleanup did not execute its queued per-CPU callback worker"
		fi
	fi
	if [ "$name" = sync_work_flush ] || \
		[ "$name" = sync_workqueue_flush ]; then
		if ! awk -v target="$case_cpu" '
function field(line, name, pos, value)
{
	pos = index(line, name "=")
	if (!pos)
		return ""
	value = substr(line, pos + length(name) + 1)
	sub(/[^0-9].*$/, "", value)
	return value
}
/workqueue_queue_work:/ && index($0, "function=cpu_accel_sync_work_flush_probe_fn") {
	if (field($0, "req_cpu") == target) {
		work = $0
		sub(/^.*work struct=/, "", work)
		sub(/[[:space:]].*$/, "", work)
	}
}
/workqueue_execute_start:/ && work != "" &&
	index($0, "work struct " work ":") &&
	index($0, "function cpu_accel_sync_work_flush_probe_fn") {
	executed = 1
}
END { exit !(work != "" && executed) }
' "$outdir/owner-trace.log"; then
			fail "sync-work flush probe did not execute on CPU $case_cpu"
		fi
	fi
	echo "PASS: $name stopped CPU $case_cpu owner in ${helper_elapsed_ms} ms"
}

if should_run_case sync_work_flush; then
	run_case sync_work_flush "$sync_work_flush_path" \
		arch_smp_sync_wait_stop\|__flush_work\|flush_work
fi
if should_run_case sync_workqueue_flush; then
	run_case sync_workqueue_flush "$sync_workqueue_flush_path" \
		arch_smp_sync_wait_stop\|cpu_accel_flush_workqueue\|workqueue_sync_wait_begin
fi
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
	run_case timer_migration "$timer_partition" \
		housekeeping_update isolated sysfs "$timer_cpu" "$timer_cgroup"
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
	command -v perl >/dev/null 2>&1 || fail "perl is required for backlog test"
	[ -r "$net_recv_tool" ] || \
		fail "net_backlog receiver helper is unavailable: $net_recv_tool"
	[ -r "$net_send_tool" ] || \
		fail "net_backlog sender helper is unavailable: $net_send_tool"
	modprobe veth || fail "veth module is unavailable for backlog test"
	ip netns add "$netns" || \
		fail "could not create network namespace for backlog test"
	netns_created=1
	ip link add "$netdev_a" type veth peer name "$netdev_b" || \
		fail "could not create veth pair for backlog test"
	netdev_created=1
	netdev_mac_b=$(cat "/sys/class/net/$netdev_b/address")
	ip link set "$netdev_b" netns "$netns" || \
		fail "could not move sender veth to its network namespace"
	ip addr add "$netdev_ip_a/30" dev "$netdev_a"
	ip link set "$netdev_a" up
	ip netns exec "$netns" ip addr add "$netdev_ip_b/30" dev "$netdev_b"
	ip netns exec "$netns" ip link set "$netdev_b" up
	netdev_mac_a=$(cat "/sys/class/net/$netdev_a/address")
	ip netns exec "$netns" ip neigh replace "$netdev_ip_a" \
		lladdr "$netdev_mac_a" nud permanent dev "$netdev_b"
	ip neigh replace "$netdev_ip_b" lladdr "$netdev_mac_b" \
		nud permanent dev "$netdev_a"
	if command -v firewall-cmd >/dev/null 2>&1 &&
	   firewall-cmd --state >/dev/null 2>&1; then
		firewall-cmd --zone=trusted --add-interface="$netdev_a" >/dev/null || \
			fail "could not trust temporary net_backlog veth"
		net_firewalld_added=1
	fi
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

	net_cpu=${CPU_ACCEL_NET_CPU:-$work_cpu}
	set_rps_cpu "$net_cpu" || \
		fail "net_backlog fallback CPU $net_cpu is not RPS-eligible"
	rps_sock_flow_entries_saved=$(cat /proc/sys/net/core/rps_sock_flow_entries)
	echo 4096 >/proc/sys/net/core/rps_sock_flow_entries
	echo 4096 >"/sys/class/net/$netdev_a/queues/rx-0/rps_flow_cnt"
	taskset -c "$target_cpu" perl "$net_recv_tool" \
		"$netdev_ip_a" "$netdev_port" \
		>/dev/null 2>"$outdir/net_backlog-recv.log" &
	net_recv_pid=$!
	sleep 0.1
	ip netns exec "$netns" taskset -c "$target_cpu" perl "$net_send_tool" \
		"$netdev_ip_a" "$netdev_port" "$netdev_ip_b" "$netdev_b" \
		>"$outdir/net_backlog-send.log" 2>&1 &
	net_send_pid=$!
	sleep 0.2
	taskset -pc "$work_cpu" "$net_send_pid" \
		>"$outdir/net_backlog-send-affinity.log" 2>&1
	kill -0 "$net_recv_pid" 2>/dev/null || \
		fail "net_backlog receiver exited before the owner run"
	kill -0 "$net_send_pid" 2>/dev/null || \
		fail "net_backlog sender exited before the owner run"
	run_case net_backlog "$netdev_a" flush_all_backlogs 1 netdev "$target_cpu"
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

if should_run_case srcu_cleanup; then
	[ ! -e "/sys/module/$srcu_cleanup_module" ] || \
		fail "SRCU cleanup probe is already loaded"
	insmod "$srcu_cleanup_probe" target_cpu="$target_cpu" \
		>"$outdir/srcu-cleanup-insmod.log" 2>&1 || \
		fail "SRCU cleanup probe load failed"
	srcu_cleanup_loaded=1
	run_case srcu_cleanup "$srcu_cleanup_module" cleanup_srcu_struct \
		1 rmmod "$target_cpu"
	srcu_cleanup_loaded=0
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

if should_run_case vmalloc_purge_wait; then
	[ ! -e "/sys/module/$vmalloc_purge_module" ] || \
		fail "vmalloc purge probe is already loaded"
	insmod "$vmalloc_purge_probe" target_cpu="$target_cpu" \
		work_cpu="$work_cpu" held_mb="$vmalloc_purge_held_mb" \
		wait_for_reentry=1 reentry_timeout_ms=400 \
		>"$outdir/vmalloc-purge-wait-insmod.log" 2>&1 || \
		fail "vmalloc wait probe load failed"
	vmalloc_purge_loaded=1
	run_case vmalloc_purge_wait "$vmalloc_purge_module" \
		kernel_tlb_flush_all 1 rmmod "$target_cpu"
	vmalloc_purge_loaded=0
fi

echo 0 >"$event_instance/tracing_on"
echo 0 >"$graph_instance/tracing_on"
echo '!stacktrace:1' >"$owner_stop_trigger"
kill -TERM "$event_trace_pid" "$graph_trace_pid" "$dmesg_pid" \
	2>/dev/null || true
wait "$event_trace_pid" 2>/dev/null || true
event_trace_pid=
wait "$graph_trace_pid" 2>/dev/null || true
graph_trace_pid=
echo >"$graph_instance/set_ftrace_filter"
echo nop >"$graph_instance/current_tracer"
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

grep -Ei 'soft lockup|task .* blocked for more than|RCU.*stall|(^|[[:space:]])BUG:|Oops:|Kernel panic' \
	"$outdir/dmesg-new.log" >"$outdir/kernel-errors.log" || true
if [ -s "$outdir/kernel-errors.log" ]; then
	cat "$outdir/kernel-errors.log" >&2
	fail "kernel log contains a lockup or error record"
fi

if [ "$selected_case" = all ]; then
	echo "PASS: synchronous work, LRU drain, SLUB/RCU/SRCU flushes, network backlog, vmalloc purge and re-entry, and ring resize complete with active-owner stops"
else
	echo "PASS: $selected_case stopped the active owner"
fi
echo "logs: $outdir"
