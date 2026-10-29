#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Worst-case latency matrix: run one bounded stress class at a time,
# capture the lateness distribution and counter correlation for each,
# and emit a per-class log plus a summary TSV.
#
# Deferred classes (not run here): net_backlog and timer_migration need
# their owner CPU in rcu_nocbs (admission is -EOPNOTSUPP otherwise), and
# irq_storm needs a test NIC via CPU_ACCEL_IRQ_INTERFACE.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCELCTL:-$script_dir/cpu-accelctl}
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-wcl.XXXXXX)}
target_cpu=${CPU_ACCEL_WCL_CPU:-2}
duration_ms=${CPU_ACCEL_WCL_DURATION_MS:-5000}
load_cpus=${CPU_ACCEL_WCL_LOAD_CPUS:-3,4,5,6,7}
selected=${CPU_ACCEL_WCL_CLASSES:-all}
summary="$outdir/summary.tsv"
thp_enabled_path=/sys/kernel/mm/transparent_hugepage/enabled
thp_enabled_saved=
thp_enabled_changed=0

fail()
{
	echo "cpu_accel WCL report: $*" >&2
	exit 1
}

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -x "$tool" ] || fail "cpu-accelctl not found or not executable: $tool"
mkdir -p "$outdir"
printf 'class\texit\tstate\tsamples\tmin_ns\tp50_ns\tp90_ns\tp99_ns\tp999_ns\tmax_ns\thist_total\tirq_count\tsoftirq_count\tworkqueue_queued\tarch_ipi_count\tarch_tlb_count\tcontext_switches\n' \
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
	[ "$selected" = all ] || [ "$selected" = "$1" ]
}

busy_loop_pids=

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

trap 'stop_load; cleanup' EXIT

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
		printf '%s\t%s\tmissing\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\t-\n' \
			"$wcl_class" "$wcl_exit" >>"$summary"
		return
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
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
	fi
	record_class "$wcl_class" "$wcl_log" "$wcl_rc"
}

baseline_run()
{
	"$tool" run --cpu "$target_cpu" --duration-ms "$duration_ms" \
		--period-us 1000 --workload user-oslat --quarantine-irqs
}

load_run()
{
	start_load
	baseline_run
	wcl_rc=$?
	stop_load
	return $wcl_rc
}

sync_work_case()
{
	CPU_ACCEL_TEST_CASE="$1" CPU_ACCELCTL="$tool" \
		sh "$script_dir/test-sync-work-owner-stop.sh"
}

single_probe_case()
{
	CPU_ACCELCTL="$tool" \
		sh "$script_dir/test-call-function-single-owner-stop.sh" "$@"
}

if should_run_class baseline; then
	run_class baseline baseline_run
fi

if should_run_class load; then
	run_class load load_run
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
	run_class timerlat env CPU_ACCELCTL="$tool" \
		sh "$script_dir/test-timerlat-owner.sh"
fi

echo "== WCL summary: $summary"
column -t -s "$(printf '\t')" "$summary" 2>/dev/null || cat "$summary"
