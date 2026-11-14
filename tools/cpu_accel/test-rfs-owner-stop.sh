#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tool=${CPU_ACCEL_TESTCTL:-$script_dir/cpu-accel-testctl}
flow_tool=${CPU_ACCEL_RFS_FLOW:-$script_dir/test-rfs-flow}
target_cpu=${CPU_ACCEL_CPU:-3}
work_cpu=${CPU_ACCEL_RFS_WORK_CPU:-0}
duration_ms=${CPU_ACCEL_RFS_DURATION_MS:-5000}
observe_ms=${CPU_ACCEL_RFS_OBSERVE_MS:-500}
packet_count=${CPU_ACCEL_RFS_PACKETS:-32}
trace_root=${TRACEFS:-/sys/kernel/tracing}
prefix=${CPU_ACCEL_RFS_PREFIX:-10.254.31}
port=${CPU_ACCEL_RFS_PORT:-35091}
netdev_a=carfs$$
netdev_b=carfsp$$
netns=cpuacrfs$$
instance="$trace_root/instances/cpu_accel_rfs_$$"
outdir=${CPU_ACCEL_TRACE_DIR:-$(mktemp -d /tmp/cpu-accel-rfs.XXXXXX)}
fifo="$outdir/flow.fifo"
dest_ip="$prefix.1"
source_ip="$prefix.2"
receiver_pid=
sender_pid=
owner_pid=
trace_pid=
dmesg_pid=
fifo_open=0
netdev_created=0
netns_created=0
instance_created=0
rfs_sock_enabled=0
owner_started=0

fail()
{
	echo "cpu_accel RFS owner-stop test: $* (logs: $outdir)" >&2
	exit 1
}

cpu_is_online()
{
	[ "$1" -eq 0 ] && return 0
	[ "$(cat "/sys/devices/system/cpu/cpu$1/online")" = 1 ]
}

cleanup()
{
	if [ -n "$owner_pid" ]; then
		kill -TERM "$owner_pid" 2>/dev/null || true
		wait "$owner_pid" 2>/dev/null || true
		owner_pid=
	fi
	if [ -n "$sender_pid" ]; then
		kill -TERM "$sender_pid" 2>/dev/null || true
		wait "$sender_pid" 2>/dev/null || true
		sender_pid=
	fi
	if [ -n "$receiver_pid" ]; then
		kill -TERM "$receiver_pid" 2>/dev/null || true
		wait "$receiver_pid" 2>/dev/null || true
		receiver_pid=
	fi
	if [ "$fifo_open" -eq 1 ]; then
		exec 3>&-
		fifo_open=0
	fi
	if [ -n "$dmesg_pid" ]; then
		kill -TERM "$dmesg_pid" 2>/dev/null || true
		wait "$dmesg_pid" 2>/dev/null || true
		dmesg_pid=
	fi
	if [ -n "$trace_pid" ]; then
		if [ "$instance_created" -eq 1 ]; then
			echo 0 >"$instance/tracing_on" 2>/dev/null || true
		fi
		kill -TERM "$trace_pid" 2>/dev/null || true
		wait "$trace_pid" 2>/dev/null || true
		trace_pid=
	fi
	if [ "$instance_created" -eq 1 ]; then
		rmdir "$instance" 2>/dev/null || true
		instance_created=0
	fi
	if [ "$netdev_created" -eq 1 ]; then
		ip link del "$netdev_b" 2>/dev/null || true
		netdev_created=0
	fi
	if [ "$netns_created" -eq 1 ]; then
		ip netns del "$netns" 2>/dev/null || true
		netns_created=0
	fi
	if [ "$rfs_sock_enabled" -eq 1 ]; then
		echo 0 >/proc/sys/net/core/rps_sock_flow_entries
		rfs_sock_enabled=0
	fi
	if [ "$owner_started" -eq 1 ]; then
		"$tool" reset >/dev/null 2>&1 || true
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ "$target_cpu" -ne "$work_cpu" ] || fail "target and work CPUs must differ"
[ -x "$tool" ] || fail "cpu-accel-testctl is not executable: $tool"
[ -x "$flow_tool" ] || fail "RFS flow helper is not executable: $flow_tool"
[ -c /dev/cpu_accel ] || fail "/dev/cpu_accel is unavailable"
[ -d "$trace_root/events/cpu_accel" ] || fail "cpu_accel tracepoints are unavailable"
[ -d "$trace_root/instances" ] || fail "tracefs instances are unavailable"
cpu_is_online "$target_cpu" || fail "target CPU $target_cpu is offline"
cpu_is_online "$work_cpu" || fail "work CPU $work_cpu is offline"
command -v ip >/dev/null 2>&1 || fail "iproute2 is required"
command -v dmesg >/dev/null 2>&1 || fail "dmesg is required"

old_rps_sock=$(cat /proc/sys/net/core/rps_sock_flow_entries)
[ "$old_rps_sock" = 0 ] || \
	fail "net.core.rps_sock_flow_entries must initially be zero"

modprobe veth || fail "veth module is unavailable"
ip link add "$netdev_a" type veth peer name "$netdev_b" || \
	fail "could not create veth pair"
netdev_created=1
netdev_mac_a=$(cat "/sys/class/net/$netdev_a/address")
ip netns add "$netns" || fail "could not create network namespace"
netns_created=1
ip link set "$netdev_a" netns "$netns"
ip netns exec "$netns" ip addr add "$dest_ip/30" dev "$netdev_a"
ip addr add "$source_ip/30" dev "$netdev_b"
ip netns exec "$netns" ip link set "$netdev_a" up
ip netns exec "$netns" ip link set lo up
ip link set "$netdev_b" up
ip neigh replace "$dest_ip" lladdr "$netdev_mac_a" nud permanent dev "$netdev_b"
ip route get "$dest_ip" from "$source_ip" oif "$netdev_b" | \
	grep -q " dev $netdev_b " || \
	fail "temporary veth route was not selected"

rps_cpus="/sys/class/net/$netdev_a/queues/rx-0/rps_cpus"
rps_flow_cnt="/sys/class/net/$netdev_a/queues/rx-0/rps_flow_cnt"
rps_mask=$(printf '%x' "$((1 << work_cpu))")
ip netns exec "$netns" sh -c 'echo "$1" >"$2"' sh "$rps_mask" "$rps_cpus" || \
	fail "could not configure RPS fallback CPU"
ip netns exec "$netns" sh -c 'echo "$1" >"$2"' sh "$packet_count" "$rps_flow_cnt" || \
	fail "could not configure the receive-queue RFS table"
echo 4096 >/proc/sys/net/core/rps_sock_flow_entries
rfs_sock_enabled=1

for event in net/net_dev_queue net/netif_receive_skb cpu_accel/owner_exit_complete; do
	[ -e "$trace_root/events/$event/enable" ] || \
		fail "trace event is unavailable: $event"
done
mkdir "$instance"
instance_created=1
echo 4096 >"$instance/buffer_size_kb"
for event in net/net_dev_queue net/netif_receive_skb cpu_accel/owner_exit_complete; do
	echo 1 >"$instance/events/$event/enable"
done
echo 1 >"$instance/tracing_on"
cat "$instance/trace_pipe" >"$outdir/trace.log" &
trace_pid=$!
dmesg --follow-new >"$outdir/dmesg-new.log" 2>&1 &
dmesg_pid=$!

ip netns exec "$netns" taskset -c "$target_cpu" "$flow_tool" receive \
	"$dest_ip" "$port" \
	>"$outdir/receiver.log" 2>"$outdir/receiver.err" &
receiver_pid=$!
mkfifo "$fifo"
taskset -c "$work_cpu" "$flow_tool" send "$dest_ip" "$port" \
	"$source_ip" "$netdev_b" \
	<"$fifo" >"$outdir/sender.log" 2>"$outdir/sender.err" &
sender_pid=$!
exec 3>"$fifo"
fifo_open=1

ready=0
for attempt in $(seq 1 500); do
	if grep -q '^ready$' "$outdir/sender.log"; then
		ready=1
		break
	fi
	kill -0 "$sender_pid" 2>/dev/null || break
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "RFS sender did not become ready"

printf 'prime\n' >&3

ready=0
for attempt in $(seq 1 500); do
	if grep -q '^prime$' "$outdir/receiver.log"; then
		ready=1
		break
	fi
	kill -0 "$receiver_pid" 2>/dev/null || break
	kill -0 "$sender_pid" 2>/dev/null || break
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "RFS prime packet was not received"

rm -f "$outdir/active"
CPU_ACCEL_ACTIVE_MARKER="$outdir/active" "$tool" run \
	--cpu "$target_cpu" --duration-ms "$duration_ms" \
	--workload user-oslat >"$outdir/ctl.log" 2>&1 &
owner_pid=$!
owner_started=1
ready=0
for attempt in $(seq 1 500); do
	if [ -f "$outdir/active" ]; then
		ready=1
		break
	fi
	kill -0 "$owner_pid" 2>/dev/null || break
	sleep 0.01
done
[ "$ready" -eq 1 ] || fail "owner-entry marker timed out"

for packet in $(seq 1 "$packet_count"); do
	printf 'owned-%03d\n' "$packet" >&3
done
sleep "$(awk -v ms="$observe_ms" 'BEGIN { printf "%.3f", ms / 1000 }')"
during_count=$(grep -c '^owned-' "$outdir/receiver.log" || true)
[ "$during_count" -eq 0 ] || \
	fail "RFS fallback delivered $during_count packets during ownership"

if wait "$owner_pid"; then
	run_rc=0
else
	run_rc=$?
fi
owner_pid=

delivered_count=0
for attempt in $(seq 1 500); do
	delivered_count=$(grep -c '^owned-' "$outdir/receiver.log" || true)
	if [ "$delivered_count" -eq "$packet_count" ]; then
		break
	fi
	sleep 0.01
done

kill -TERM "$sender_pid" "$receiver_pid" 2>/dev/null || true
wait "$sender_pid" 2>/dev/null || true
sender_pid=
wait "$receiver_pid" 2>/dev/null || true
receiver_pid=
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

[ "$run_rc" -eq 0 ] || fail "bounded owner run failed"
[ "$delivered_count" -eq "$packet_count" ] || \
	fail "only $delivered_count/$packet_count RFS packets arrived after owner exit"
grep -q 'state=3 ' "$outdir/ctl.log" || fail "owner did not report COMPLETE"
grep -q 'user_escape_count=0' "$outdir/ctl.log" || \
	fail "RFS IPI required an owner escape"
grep -q 'context_switches=0' "$outdir/ctl.log" || \
	fail "owner context switched during the RFS test"
grep -q 'migration_detected=0' "$outdir/ctl.log" || \
	fail "owner migrated during the RFS test"
grep -Eq 'arch_call_function_deferred=[1-9][0-9]*' "$outdir/ctl.log" || \
	fail "owner did not defer the RFS call-function IPI"
[ -s "$outdir/kernel-errors.log" ] && \
	fail "kernel errors appeared during the RFS test"

queue_line=$(grep -E "net_dev_queue:.*dev=$netdev_b" "$outdir/trace.log" | \
	tail -n 1 || true)
receive_line=$(grep -E "netif_receive_skb:.*dev=$netdev_a" "$outdir/trace.log" | \
	tail -n 1 || true)
exit_line=$(grep -E "owner_exit_complete:.*cpu=$target_cpu .*stop_requested=0" \
	"$outdir/trace.log" | tail -n 1 || true)
[ -n "$queue_line" ] || fail "RFS transmit trace is missing"
[ -n "$receive_line" ] || fail "RFS receive trace is missing"
[ -n "$exit_line" ] || fail "RFS owner-exit trace is missing"

queue_timestamp=$(printf '%s\n' "$queue_line" | awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
receive_timestamp=$(printf '%s\n' "$receive_line" | \
	awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
exit_timestamp=$(printf '%s\n' "$exit_line" | awk '{sub(/:$/, "", $4); printf "%.6f\n", $4 + 0}')
awk -v queue="$queue_timestamp" -v receive="$receive_timestamp" \
	-v complete="$exit_timestamp" \
	'BEGIN { exit !(queue > 0 && receive > queue && complete > receive) }' || \
	fail "RFS traffic did not precede owner exit in the trace"

echo "cpu_accel RFS owner-stop test: PASS"
echo "target_cpu=$target_cpu work_cpu=$work_cpu packets=$packet_count"
echo "during_ownership_delivered=$during_count after_exit_delivered=$delivered_count"
echo "queue_line=$queue_line"
echo "receive_line=$receive_line"
echo "exit_line=$exit_line"
