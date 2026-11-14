#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
helper=${CPU_ACCEL_MIGRATION_HELPER:-$script_dir/test-migration-owner-helper}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_MIGRATION_WORK_CPU:-0}
reader_cpu=${CPU_ACCEL_MIGRATION_READER_CPU:-1}
owner_duration_ms=${CPU_ACCEL_MIGRATION_OWNER_DURATION_MS:-15000}
migration_duration_ms=${CPU_ACCEL_MIGRATION_DURATION_MS:-10000}
trace_root=${TRACEFS:-/sys/kernel/tracing}
instance="$trace_root/instances/cpu_accel_migration_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-migration.XXXXXX)}
run_pid=
helper_pid=
trace_pid=
dmesg_pid=
run_rc=0
helper_rc=0

fail()
{
	echo "cpu_accel migration owner test: $* (logs: $outdir)" >&2
	exit 1
}

cpu_is_online()
{
	[ "$1" -eq 0 ] && return 0
	[ "$(cat "/sys/devices/system/cpu/cpu$1/online")" = 1 ]
}

cleanup()
{
	if [ -n "$helper_pid" ]; then
		kill -TERM "$helper_pid" 2>/dev/null || true
		wait "$helper_pid" 2>/dev/null || true
		helper_pid=
	fi
	if [ -n "$run_pid" ]; then
		kill -TERM "$run_pid" 2>/dev/null || true
		wait "$run_pid" 2>/dev/null || true
		run_pid=
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
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
case "$target_cpu" in
''|*[!0-9]*) fail "target CPU must be a nonnegative integer" ;;
esac
case "$work_cpu" in
''|*[!0-9]*) fail "work CPU must be a nonnegative integer" ;;
esac
case "$reader_cpu" in
''|*[!0-9]*) fail "reader CPU must be a nonnegative integer" ;;
esac
case "$owner_duration_ms" in
''|*[!0-9]*) fail "owner duration must be a positive integer" ;;
esac
case "$migration_duration_ms" in
''|*[!0-9]*) fail "migration duration must be a positive integer" ;;
esac
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ "$target_cpu" -ne "$reader_cpu" ] || \
	fail "reader CPU must differ from owner CPU $target_cpu"
[ "$work_cpu" -ne "$reader_cpu" ] || \
	fail "reader CPU must differ from work CPU $work_cpu"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -x "$helper" ] || fail "migration helper is not executable: $helper"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/events/migrate" ] || fail "migration tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
for command in awk cat dmesg find grep sed sort timeout; do
	command -v "$command" >/dev/null 2>&1 || \
		fail "required command is unavailable: $command"
done
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
cpu_is_online "$reader_cpu" || fail "reader CPU $reader_cpu is offline"

numa_nodes=
for path in /sys/devices/system/node/node*; do
	[ -f "$path/meminfo" ] || continue
	node=${path##*/}
	node=${node#node}
	case "$node" in
	''|*[!0-9]*) continue ;;
	esac
	numa_nodes="$numa_nodes $node"
done
node1=$(printf '%s\n' $numa_nodes | sort -n | sed -n '1p')
node2=$(printf '%s\n' $numa_nodes | sort -n | sed -n '2p')
[ -n "$node1" ] && [ -n "$node2" ] && [ "$node1" != "$node2" ] || \
	fail "two NUMA memory nodes are required"

for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete \
	     migrate/mm_migrate_pages_start migrate/mm_migrate_pages \
	     migrate/set_migration_pte migrate/remove_migration_pte; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done

helper_timeout_seconds=$(( (migration_duration_ms + 999) / 1000 + 2 ))
[ "$migration_duration_ms" -gt 0 ] || fail "migration duration must be positive"
[ "$migration_duration_ms" -le 60000 ] || \
	fail "migration duration must be at most 60000 ms"
[ "$owner_duration_ms" -gt 0 ] || fail "owner duration must be positive"
[ "$owner_duration_ms" -le 60000 ] || \
	fail "owner duration must be at most 60000 ms"
[ $((owner_duration_ms - migration_duration_ms)) -ge 2000 ] || \
	fail "owner duration must exceed migration duration by at least 2000 ms"

mkdir "$instance"
echo 4096 >"$instance/buffer_size_kb"
for event in cpu_accel/owner_stop_request cpu_accel/owner_exit_complete \
	     migrate/mm_migrate_pages_start migrate/mm_migrate_pages \
	     migrate/set_migration_pte migrate/remove_migration_pte; do
	echo 1 >"$instance/events/$event/enable"
done
echo 0 >"$instance/tracing_on"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

rm -f "$outdir/active"
CPU_ACCEL_ACTIVE_MARKER="$outdir/active" "$tool" run \
	--cpu "$target_cpu" --duration-ms "$owner_duration_ms" \
	--workload user-oslat >"$outdir/ctl.log" 2>&1 &
run_pid=$!

ready=0
for attempt in $(seq 1 500); do
	if [ -f "$outdir/active" ]; then
		ready=1
		break
	fi
	if ! kill -0 "$run_pid" 2>/dev/null; then
		break
	fi
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry marker timed out"

echo 1 >"$instance/tracing_on"
timeout --kill-after=2s "${helper_timeout_seconds}s" \
	"$helper" "$work_cpu" "$reader_cpu" "$node1" "$node2" \
	"$migration_duration_ms" >"$outdir/migration.log" 2>&1 &
helper_pid=$!
if wait "$helper_pid"; then
	helper_rc=0
else
	helper_rc=$?
fi
helper_pid=

if wait "$run_pid"; then
	run_rc=0
else
	run_rc=$?
fi
run_pid=
echo 0 >"$instance/tracing_on"

kill -TERM "$trace_pid" "$dmesg_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
trace_pid=
wait "$dmesg_pid" 2>/dev/null || true
dmesg_pid=

kernel_error_pattern='soft lockup|task .* blocked for more than|RCU.*stall'
kernel_error_pattern="$kernel_error_pattern|(^|[[:space:]])BUG:|Oops:|Kernel panic"
grep -Ei "$kernel_error_pattern" "$outdir/dmesg-new.log" \
	>"$outdir/kernel-errors.log" || true

[ "$helper_rc" -eq 0 ] || fail "migration helper failed ($helper_rc)"
[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
grep -q '^PASS: ' "$outdir/migration.log" || \
	fail "migration helper did not report PASS"

migrations=$(awk '/^PASS:/ {
	for (i = 1; i <= NF; i++)
		if ($i ~ /^migrations=/) {
			sub(/^migrations=/, "", $i)
			print $i
			exit
		}
}' "$outdir/migration.log")
moves_to_node1=$(awk -v key="moves_to_node${node1}=" '/^PASS:/ {
	for (i = 1; i <= NF; i++)
		if (index($i, key) == 1) {
			sub(key, "", $i)
			print $i
			exit
		}
}' "$outdir/migration.log")
moves_to_node2=$(awk -v key="moves_to_node${node2}=" '/^PASS:/ {
	for (i = 1; i <= NF; i++)
		if (index($i, key) == 1) {
			sub(key, "", $i)
			print $i
			exit
		}
}' "$outdir/migration.log")
reads=$(awk '/^PASS:/ {
	for (i = 1; i <= NF; i++)
		if ($i ~ /^reads=/) {
			sub(/^reads=/, "", $i)
			print $i
			exit
		}
}' "$outdir/migration.log")
final_node=$(awk '/^PASS:/ {
	for (i = 1; i <= NF; i++)
		if ($i ~ /^final_node=/) {
			sub(/^final_node=/, "", $i)
			print $i
			exit
		}
}' "$outdir/migration.log")

[ "$migrations" -ge 2 ] || fail "migration helper did not move both directions"
[ "$moves_to_node1" -ge 1 ] || fail "migration helper did not reach node $node1"
[ "$moves_to_node2" -ge 1 ] || fail "migration helper did not reach node $node2"
[ "$reads" -gt 0 ] || fail "reader thread did not touch the migrated page"
case "$final_node" in
"$node1"|"$node2") ;;
*) fail "final migration node is invalid: $final_node" ;;
esac

grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'mode=0' "$outdir/ctl.log" || fail "owner did not return to Linux mode"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "migration stress required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during migration stress"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during migration stress"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during migration stress"

for event in mm_migrate_pages_start set_migration_pte \
	     remove_migration_pte owner_exit_complete; do
	grep -q "$event:" "$outdir/trace.log" || \
		fail "required trace event is missing: $event"
done
if grep -Eq 'LOST [0-9]+ EVENTS' "$outdir/trace.log"; then
	fail "trace buffer reported lost events"
fi

echo "cpu_accel migration owner test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu reader_cpu=$reader_cpu"
echo "node1=$node1 node2=$node2 migrations=$migrations"
echo "moves_to_node1=$moves_to_node1 moves_to_node2=$moves_to_node2 reads=$reads"
echo "final_node=$final_node owner_duration_ms=$owner_duration_ms migration_duration_ms=$migration_duration_ms"
echo "logs: $outdir"
