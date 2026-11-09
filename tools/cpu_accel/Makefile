# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -fno-stack-protector -pthread
CPPFLAGS += -D_GNU_SOURCE -Wno-cpp -I../../include/uapi -I../../usr/include
KDIR ?= $(abspath ../..)

all: cpu-accel-testctl

test: cpu-accel-testctl test-region-lifecycle test-tlb-generation test-tlb-cow-churn
	./test-region-lifecycle
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-cpu-accel.sh

test-sync-work-owner-stop: cpu-accel-testctl test-sync-wait-prime
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl CPU_ACCEL_SYNC_WAIT_PRIME=./test-sync-wait-prime \
		sh ./test-sync-work-owner-stop.sh

test-perf-owner-stop: cpu-accel-testctl test-perf-event-open
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl CPU_ACCEL_PERF_EVENT_HELPER=./test-perf-event-open \
		sh ./test-perf-owner-stop.sh

test-irq-storm: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-irq-storm.sh

test-timerlat-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-timerlat-owner.sh

test-watchdog-owner-admission: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-watchdog-owner-admission.sh

test-resctrl-owner-stop: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-resctrl-owner-stop.sh

test-resctrl-task-owner-stop: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-resctrl-task-owner-stop.sh

test-resctrl-monitor-owner-stop: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-resctrl-monitor-owner-stop.sh

test-rfs-owner-stop: cpu-accel-testctl test-rfs-flow
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl CPU_ACCEL_RFS_FLOW=./test-rfs-flow \
		sh ./test-rfs-owner-stop.sh

test-veth-teardown-owner: cpu-accel-testctl test-rfs-flow
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl CPU_ACCEL_RFS_FLOW=./test-rfs-flow \
		sh ./test-veth-teardown-owner.sh

test-veth-newlink-fault-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl \
		sh ./test-veth-newlink-fault-owner.sh

test-dirty-writeback-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-dirty-writeback-owner.sh

test-amd-pstate-shmem-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-amd-pstate-shmem-owner.sh

test-thermal-cooling-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-thermal-cooling-owner.sh

test-cpuidle-state-owner: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-cpuidle-state-owner.sh

test-user-escape-fault: cpu-accel-testctl test-user-escape-rescue
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl CPU_ACCEL_ESCAPE_RESCUE=./test-user-escape-rescue \
		sh ./test-user-escape-fault.sh

test-rcu-nocb-admission: cpu-accel-testctl
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl sh ./test-rcu-nocb-admission.sh

test-rcu-callback-routing: cpu-accel-testctl rcu-probe
	CPU_ACCEL_TESTCTL=./cpu-accel-testctl \
		CPU_ACCEL_RCU_PROBE=$(CURDIR)/rcu-probe/cpu_accel_rcu_callback_probe.ko \
		sh ./test-rcu-callback-routing.sh

rcu-probe:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/rcu-probe modules

cpu-accel-testctl: cpu-accel-testctl.o libcpu_accel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-tlb-generation: test-tlb-generation.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-tlb-cow-churn: test-tlb-cow-churn.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-region-lifecycle: test-region-lifecycle.o libcpu_accel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-seccomp-jit-churn: test-seccomp-jit-churn.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-sync-wait-prime: test-sync-wait-prime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-perf-event-open: test-perf-event-open.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-user-escape-rescue: test-user-escape-rescue.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-rfs-flow: test-rfs-flow.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

cpu-accel-testctl.o: cpu_accel_user_image.h cpu_accel_workload.h
cpu-accel-testctl.o libcpu_accel.o: cpu_accel.h ../../include/uapi/linux/cpu_accel.h
test-tlb-generation.o: ../../include/uapi/linux/cpu_accel.h
test-region-lifecycle.o: cpu_accel.h cpu_accel_workload.h \
	../../include/uapi/linux/cpu_accel.h
test-seccomp-jit-churn.o: CPPFLAGS := -D_GNU_SOURCE
test-sync-wait-prime.o: CPPFLAGS := -D_GNU_SOURCE
test-perf-event-open.o: CPPFLAGS := -D_GNU_SOURCE
test-user-escape-rescue.o: CPPFLAGS := -D_GNU_SOURCE -Wno-cpp -I../../include/uapi

clean:
	rm -f cpu-accel-testctl test-tlb-generation test-tlb-generation.o \
		test-tlb-cow-churn test-tlb-cow-churn.o \
		test-seccomp-jit-churn test-seccomp-jit-churn.o \
		test-sync-wait-prime test-sync-wait-prime.o \
		test-perf-event-open test-perf-event-open.o test-user-escape-rescue \
		test-user-escape-rescue.o test-region-lifecycle \
		test-region-lifecycle.o test-rfs-flow test-rfs-flow.o \
		cpu-accel-testctl.o libcpu_accel.o
	$(MAKE) -C $(KDIR) M=$(CURDIR)/rcu-probe clean

.PHONY: all clean test test-sync-work-owner-stop test-perf-owner-stop \
	test-irq-storm test-timerlat-owner test-user-escape-fault \
	test-watchdog-owner-admission test-resctrl-owner-stop \
	test-resctrl-task-owner-stop \
	test-resctrl-monitor-owner-stop \
	test-rfs-owner-stop \
	test-veth-teardown-owner \
	test-veth-newlink-fault-owner \
	test-dirty-writeback-owner \
	test-amd-pstate-shmem-owner \
	test-thermal-cooling-owner \
	test-cpuidle-state-owner \
	test-rcu-nocb-admission \
	test-rcu-callback-routing rcu-probe
