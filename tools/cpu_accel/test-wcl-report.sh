#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Worst-case latency matrix: run one bounded stress class at a time,
# capture the lateness distribution and counter correlation for each,
# and emit a per-class log plus a summary TSV.
#
# The default matrix is the common portable set.  Select the device
# lifecycle, power-policy, resctrl, perf, ftrace, TLB, hotplug, stop-machine,
# fault-injection, exception, RCU, migration, net_backlog, timer_migration, or
# irq_storm classes individually when the required topology or test NIC is
# available; selecting all attempts every class.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-wcl.XXXXXX)}
target_cpu=${CPU_ACCEL_WCL_CPU:-2}
duration_ms=${CPU_ACCEL_WCL_DURATION_MS:-5000}
load_cpus=${CPU_ACCEL_WCL_LOAD_CPUS:-3,4,5,6,7}
work_cpu=${CPU_ACCEL_WCL_WORK_CPU:-0}
selected=${CPU_ACCEL_WCL_CLASSES:-common}
summary="$outdir/summary.tsv"
thp_enabled_path=/sys/kernel/mm/transparent_hugepage/enabled
thp_enabled_saved=
thp_enabled_changed=0
wcl_failed=0

fail()
{
	echo "cpu_accel WCL report: $*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accel-testctl not found or not executable: $tool"
mkdir -p "$outdir"
	printf 'class\texit\tstate\tsamples\tmin_ns\tp50_ns\tp90_ns\tp99_ns\t' \
	       'p999_ns\tmax_ns\thist_total\tirq_count\tsoftirq_count\t' \
	       'workqueue_queued\tarch_ipi_count\tarch_tlb_count\t' \
	       'arch_nmi_count\tarch_mce_exception_count\t' \
	       'arch_mce_poll_count\tcontext_switches\n' \
	>"$summary"

# khugepaged's lru_add_drain_all() stops every owned CPU on its scan
# period and would end a run early; disable THP for the whole matrix.
if [ -r "$thp_enabled_path" ] && [ -w "$thp_enabled_path" ]; then
	thp_enabled_state=$(cat "$thp_enabled_path")
	thp_enabled_saved=$(printf '%s\n' "$thp_enabled_state" |
		sed -n 's/.*\[\([^]]*\)\].*/\1/p')
	[ -n "$thp_enabled_saved" ] || \
		fail "could not read the selected transparent hugepage mode"
	if [ "$thp_enabled_saved" != never ]; then
		thp_enabled_changed=1
		echo never >"$thp_enabled_path" || \
			fail "could not disable transparent hugepages for the WCL matrix"
	fi
	case "$(cat "$thp_enabled_path")" in
	*'[never]'*) ;;
	*) fail "transparent hugepages did not enter never mode" ;;
	esac
	echo "INFO: transparent hugepages temporarily disabled (previous mode: $thp_enabled_saved)"
fi

cleanup()
{
	if [ "$thp_enabled_changed" -eq 1 ]; then
		timeout --kill-after=2s 8s sh -c \
			"echo '$thp_enabled_saved' > '$thp_enabled_path'" \
			2>/dev/null || true
		thp_enabled_changed=0
	fi
}
trap cleanup EXIT

should_run_class()
{
	case "$selected" in
	all)
		return 0
		;;
	common)
		case "$1" in
		device_reload|device_teardown|device_rebind|device_sriov|\
		amd_pstate|thermal_cooling|cpuidle_state|dirty_writeback|\
		resctrl_owner|resctrl_task|resctrl_monitor|\
		perf_event|ftrace_maintenance|tlb_flush|\
		hotplug_up|hotplug_down|stop_machine|stop_machine_fault|\
		user_escape_fault|\
		user_death|rcu_callback_routing|\
		migration|net_backlog|timer_migration|irq_storm)
			return 1
			;;
		esac
		return 0
		;;
	*)
		[ "$selected" = "$1" ]
		;;
	esac
}

busy_loop_pids=
memory_pressure_pid=
memory_pressure_file=

start_load()
{
	wcl_cpu=
	wcl_pid=
	for wcl_cpu in $(printf '%s' "$load_cpus" | tr ',' ' '); do
		taskset -c "$wcl_cpu" sh -c 'while :; do :; done' &
		wcl_pid=$!
		busy_loop_pids="$busy_loop_pids $wcl_pid"
	done
}

stop_load()
{
	wcl_pid=
	for wcl_pid in $busy_loop_pids; do
		kill "$wcl_pid" 2>/dev/null || true
		wait "$wcl_pid" 2>/dev/null || true
	done
	busy_loop_pids=
}

trap 'stop_load; stop_memory_pressure; cleanup' EXIT

start_memory_pressure()
{
	memory_pressure_file="/dev/shm/cpu-accel-wcl-memory-$$"
	taskset -c "$work_cpu" sh -c '
		while :; do
			dd if=/dev/zero of="$1" bs=1M count=128 status=none
			dd if="$1" of=/dev/null bs=1M status=none
		done' \
		sh "$memory_pressure_file" &
	memory_pressure_pid=$!
}

stop_memory_pressure()
{
	if [ -n "$memory_pressure_pid" ]; then
		kill -TERM "$memory_pressure_pid" 2>/dev/null || true
		wait "$memory_pressure_pid" 2>/dev/null || true
		memory_pressure_pid=
	fi
	if [ -n "$memory_pressure_file" ]; then
		rm -f "$memory_pressure_file"
		memory_pressure_file=
	fi
}

# Extract one field from a status line by exact token name.
field()
{
	printf '%s\n' "$2" | awk -v name="$1=" '
		{
			for (i = 1; i <= NF; i++)
				if (index($i, name) == 1) {
					print substr($i, length(name) + 1)
					exit
				}
		}'
}

# Append one summary row using the final status line found in the class
# outdir ctl logs first, then the class stdout log.
record_class()
{
	wcl_class=$1
	wcl_log=$2
	wcl_exit=$3
	status_line=
	for ctl_log in "$outdir/$1"/*ctl*.log "$outdir/$1/ctl.log"; do
		[ -f "$ctl_log" ] || continue
		wcl_candidate=$(grep -E '^state=[3-7] ' "$ctl_log" | tail -n 1 || true)
		[ -n "$wcl_candidate" ] && status_line=$wcl_candidate
	done
	if [ -z "$status_line" ]; then
		status_line=$(grep -E '^state=[3-7] ' "$wcl_log" | tail -n 1 || true)
	fi
	if [ -z "$status_line" ]; then
		printf '%s\t%s\tmissing\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\n' \
			"$wcl_class" "$wcl_exit" >>"$summary"
		return
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
		"$wcl_class" "$wcl_exit" \
		"$(field state "$status_line")" \
		"$(field samples "$status_line")" \
		"$(field min_lateness_ns "$status_line")" \
		"$(field hist_p50_ns "$status_line")" \
		"$(field hist_p90_ns "$status_line")" \
		"$(field hist_p99_ns "$status_line")" \
		"$(field hist_p999_ns "$status_line")" \
		"$(field max_lateness_ns "$status_line")" \
		"$(field hist_total "$status_line")" \
		"$(field irq_count "$status_line")" \
		"$(field softirq_count "$status_line")" \
		"$(field workqueue_queued "$status_line")" \
		"$(field arch_ipi_count "$status_line")" \
		"$(field arch_tlb_count "$status_line")" \
		"$(field arch_nmi_count "$status_line")" \
		"$(field arch_mce_exception_count "$status_line")" \
		"$(field arch_mce_poll_count "$status_line")" \
		"$(field context_switches "$status_line")" >>"$summary"
}

run_class()
{
	wcl_class=$1
	wcl_log="$outdir/$1.log"
	shift
	echo "== WCL class: $wcl_class (log: $wcl_log)"
	mkdir -p "$outdir/$wcl_class"
	set +e
	CPU_ACCEL_TRACE_DIR="$outdir/$wcl_class" "$@" >"$wcl_log" 2>&1
	wcl_rc=$?
	set -e
	if [ $wcl_rc -ne 0 ]; then
		echo "WCL class $wcl_class exited $wcl_rc; tail:"
		tail -n 15 "$wcl_log" || true
		wcl_failed=1
	fi
	record_class "$wcl_class" "$wcl_log" "$wcl_rc"
}

baseline_run()
{
	"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
		--period-us 1000 --workload user-cyclictest --quarantine-irqs
}

load_run()
{
	start_load
	baseline_run
	wcl_rc=$?
	stop_load
	return $wcl_rc
}

memory_pressure_run()
{
	start_memory_pressure
	baseline_run
	wcl_rc=$?
	stop_memory_pressure
	return $wcl_rc
}

sync_work_case()
{
	CPU_ACCEL_CPU="$target_cpu" CPU_ACCEL_WORK_CPU="$work_cpu" \
	CPU_ACCEL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TEST_CASE="$1" CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-sync-work-owner-stop.sh"
}

single_probe_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-call-function-single-owner-stop.sh" "$@"
}

netdevsim_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_NETDEVSIM_WORK_CPU="$work_cpu" \
	CPU_ACCEL_NETDEVSIM_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-netdevsim-$1-owner.sh"
}

amd_pstate_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_AMD_PSTATE_WORK_CPU="$work_cpu" \
	CPU_ACCEL_AMD_PSTATE_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-amd-pstate-shmem-owner.sh"
}

thermal_cooling_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_THERMAL_WORK_CPU="$work_cpu" \
	CPU_ACCEL_THERMAL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-thermal-cooling-owner.sh"
}

cpuidle_state_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_CPUIDLE_WORK_CPU="$work_cpu" \
	CPU_ACCEL_CPUIDLE_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-cpuidle-state-owner.sh"
}

dirty_writeback_case()
{
	writeback_duration_ms=$duration_ms
	[ "$writeback_duration_ms" -ge 10000 ] || writeback_duration_ms=10000
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_WRITEBACK_WORK_CPU="$work_cpu" \
	CPU_ACCEL_WRITEBACK_DURATION_MS="$writeback_duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-dirty-writeback-owner.sh"
}

resctrl_owner_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_RESCTRL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-resctrl-owner-stop.sh"
}

resctrl_task_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_RESCTRL_TASK_WORK_CPU="$work_cpu" \
	CPU_ACCEL_RESCTRL_TASK_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-resctrl-task-owner-stop.sh"
}

resctrl_monitor_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_RESCTRL_MONITOR_WORK_CPU="$work_cpu" \
	CPU_ACCEL_RESCTRL_MONITOR_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-resctrl-monitor-owner-stop.sh"
}

perf_event_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_ESCAPE_AFTER_MS=3000 \
	CPU_ACCEL_TESTCTL="$tool" \
	CPU_ACCEL_PERF_EVENT_HELPER="$script_dir/test-perf-event-open" \
		sh "$script_dir/test-perf-owner-stop.sh"
}

ftrace_maintenance_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_ESCAPE_AFTER_MS=3000 \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-ftrace-maintenance.sh"
}

tlb_flush_case()
{
	tlb_stress_cpu=${CPU_ACCEL_WCL_TLB_STRESS_CPU:-$work_cpu}
	if [ "$tlb_stress_cpu" -eq "$target_cpu" ]; then
		fail "TLB stress CPU must differ from the owner CPU"
	fi
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_ESCAPE_AFTER_MS=3000 \
	CPU_ACCEL_TESTCTL="$tool" \
	CPU_ACCEL_TLB_FLUSH_PROBE="$script_dir/tlb-flush-probe/cpu_accel_tlb_flush_probe.ko" \
	CPU_ACCEL_TLB_COW_CHURN="$script_dir/test-tlb-cow-churn" \
	CPU_ACCEL_TLB_COW_CHURN_MS=3000 \
	CPU_ACCEL_STRESS_CPU="$tlb_stress_cpu" \
		sh "$script_dir/test-tlb-flush.sh"
}

hotplug_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
	CPU_ACCEL_HOTPLUG_MODE="$1" \
		sh "$script_dir/test-hotplug-race.sh"
}

stop_machine_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-stop-machine-inactive-owner.sh"
}

stop_machine_fault_case()
{
	CPU_ACCEL_CPU="$target_cpu" \
	CPU_ACCEL_DURATION_MS="$duration_ms" \
	CPU_ACCEL_TESTCTL="$tool" \
	CPU_ACCEL_STOP_MACHINE_PROBE=\
		"$script_dir/stop-machine-probe/cpu_accel_stop_machine_probe.ko" \
		sh "$script_dir/test-stop-machine-owner-stop.sh"
}

user_escape_fault_case()
{
	CPU_ACCEL_ESCAPE_CPU="$target_cpu" \
	CPU_ACCEL_TESTCTL="$tool" \
	CPU_ACCEL_ESCAPE_RESCUE="$script_dir/test-user-escape-rescue" \
		sh "$script_dir/test-user-escape-fault.sh"
}

user_death_case()
{
	CPU_ACCEL_USER_DEATH_CPU="$target_cpu" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-user-death-owner.sh"
}

rcu_callback_routing_case()
{
	CPU_ACCEL_RCU_CPU="$target_cpu" \
	CPU_ACCEL_RCU_READER_CPU="$work_cpu" \
	CPU_ACCEL_RCU_PROBE="$script_dir/rcu-probe/cpu_accel_rcu_callback_probe.ko" \
	CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-rcu-callback-routing.sh"
}

if should_run_class baseline; then
	run_class baseline baseline_run
fi

if should_run_class load; then
	run_class load load_run
fi

if should_run_class memory_pressure; then
	run_class memory_pressure memory_pressure_run
fi

if should_run_class vmstat; then
	run_class vmstat sync_work_case vmstat
fi

if should_run_class lru; then
	run_class lru sync_work_case lru
fi

if should_run_class slub_shrink; then
	run_class slub_shrink sync_work_case slub_shrink
fi

if should_run_class ring_resize; then
	run_class ring_resize sync_work_case ring_resize
fi

if should_run_class call_function; then
	run_class call_function single_probe_case
fi

if should_run_class irq_work; then
	run_class irq_work single_probe_case irq-work
fi

if should_run_class timerlat; then
	run_class timerlat env CPU_ACCEL_TIMERLAT_CPU="$target_cpu" \
		CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-timerlat-owner.sh"
fi

if should_run_class device_reload; then
	run_class device_reload netdevsim_case reload
fi

if should_run_class device_teardown; then
	run_class device_teardown netdevsim_case teardown
fi

if should_run_class device_rebind; then
	run_class device_rebind netdevsim_case rebind
fi

if should_run_class device_sriov; then
	run_class device_sriov netdevsim_case sriov
fi

if should_run_class amd_pstate; then
	run_class amd_pstate amd_pstate_case
fi

if should_run_class thermal_cooling; then
	run_class thermal_cooling thermal_cooling_case
fi

if should_run_class cpuidle_state; then
	run_class cpuidle_state cpuidle_state_case
fi

if should_run_class dirty_writeback; then
	run_class dirty_writeback dirty_writeback_case
fi

if should_run_class resctrl_owner; then
	run_class resctrl_owner resctrl_owner_case
fi

if should_run_class resctrl_task; then
	run_class resctrl_task resctrl_task_case
fi

if should_run_class resctrl_monitor; then
	run_class resctrl_monitor resctrl_monitor_case
fi

if should_run_class perf_event; then
	run_class perf_event perf_event_case
fi

if should_run_class ftrace_maintenance; then
	run_class ftrace_maintenance ftrace_maintenance_case
fi

if should_run_class tlb_flush; then
	run_class tlb_flush tlb_flush_case
fi

if should_run_class hotplug_up; then
	run_class hotplug_up hotplug_case up
fi

if should_run_class hotplug_down; then
	run_class hotplug_down hotplug_case down
fi

if should_run_class stop_machine; then
	run_class stop_machine stop_machine_case
fi

if should_run_class stop_machine_fault; then
	run_class stop_machine_fault stop_machine_fault_case
fi

if should_run_class user_escape_fault; then
	run_class user_escape_fault user_escape_fault_case
fi

if should_run_class user_death; then
	run_class user_death user_death_case
fi

if should_run_class rcu_callback_routing; then
	run_class rcu_callback_routing rcu_callback_routing_case
fi

if should_run_class migration; then
	[ "$duration_ms" -ge 3000 ] || \
		fail "migration class requires at least 3000 ms duration"
	migration_reader_cpu=${CPU_ACCEL_WCL_MIGRATION_READER_CPU:-1}
	if [ "$migration_reader_cpu" -eq "$target_cpu" ] ||
	   [ "$migration_reader_cpu" -eq "$work_cpu" ]; then
		fail "migration reader CPU must differ from owner and work CPUs"
	fi
	run_class migration env CPU_ACCEL_TESTCTL="$tool" \
		CPU_ACCEL_CPU="$target_cpu" \
		CPU_ACCEL_MIGRATION_WORK_CPU="$work_cpu" \
		CPU_ACCEL_MIGRATION_READER_CPU="$migration_reader_cpu" \
		CPU_ACCEL_MIGRATION_OWNER_DURATION_MS="$duration_ms" \
		CPU_ACCEL_MIGRATION_DURATION_MS="$((duration_ms - 2000))" \
		sh "$script_dir/test-migration-owner.sh"
fi

if should_run_class net_backlog; then
	run_class net_backlog env CPU_ACCEL_CPU="$target_cpu" \
		CPU_ACCEL_WORK_CPU="$work_cpu" \
		CPU_ACCEL_DURATION_MS="$duration_ms" \
		CPU_ACCEL_TEST_CASE=net_backlog \
		CPU_ACCEL_TESTCTL="$tool" \
		sh "$script_dir/test-sync-work-owner-stop.sh"
fi

if should_run_class timer_migration; then
	run_class timer_migration sync_work_case timer_migration
fi

if should_run_class irq_storm; then
	run_class irq_storm env CPU_ACCEL_TESTCTL="$tool" \
		CPU_ACCEL_CPU="$target_cpu" \
		sh "$script_dir/test-irq-storm.sh"
fi

echo "== WCL summary: $summary"
column -t -s "$(printf '\t')" "$summary" 2>/dev/null || cat "$summary"
exit "$wcl_failed"
