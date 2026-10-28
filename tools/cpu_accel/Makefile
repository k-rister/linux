# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -fno-stack-protector -pthread
CPPFLAGS += -D_GNU_SOURCE -Wno-cpp -I../../include/uapi -I../../usr/include
KDIR ?= $(abspath ../..)

all: cpu-accelctl

test: cpu-accelctl test-tlb-generation test-tlb-cow-churn
	CPU_ACCELCTL=./cpu-accelctl sh ./test-cpu-accel.sh

test-sync-work-owner-stop: cpu-accelctl test-sync-wait-prime
	CPU_ACCELCTL=./cpu-accelctl CPU_ACCEL_SYNC_WAIT_PRIME=./test-sync-wait-prime \
		sh ./test-sync-work-owner-stop.sh

test-perf-owner-stop: cpu-accelctl test-perf-event-open
	CPU_ACCELCTL=./cpu-accelctl CPU_ACCEL_PERF_EVENT_HELPER=./test-perf-event-open \
		sh ./test-perf-owner-stop.sh

test-irq-storm: cpu-accelctl
	CPU_ACCELCTL=./cpu-accelctl sh ./test-irq-storm.sh

test-timerlat-owner: cpu-accelctl
	CPU_ACCELCTL=./cpu-accelctl sh ./test-timerlat-owner.sh

test-user-escape-fault: cpu-accelctl test-user-escape-rescue
	CPU_ACCELCTL=./cpu-accelctl CPU_ACCEL_ESCAPE_RESCUE=./test-user-escape-rescue \
		sh ./test-user-escape-fault.sh

test-rcu-nocb-admission: cpu-accelctl
	CPU_ACCELCTL=./cpu-accelctl sh ./test-rcu-nocb-admission.sh

test-rcu-callback-routing: cpu-accelctl rcu-probe
	CPU_ACCELCTL=./cpu-accelctl \
		CPU_ACCEL_RCU_PROBE=$(CURDIR)/rcu-probe/cpu_accel_rcu_callback_probe.ko \
		sh ./test-rcu-callback-routing.sh

rcu-probe:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/rcu-probe modules

cpu-accelctl: cpu-accelctl.o libcpu_accel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-tlb-generation: test-tlb-generation.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-tlb-cow-churn: test-tlb-cow-churn.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-seccomp-jit-churn: test-seccomp-jit-churn.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-sync-wait-prime: test-sync-wait-prime.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-perf-event-open: test-perf-event-open.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-user-escape-rescue: test-user-escape-rescue.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

cpu-accelctl.o: cpu_accel_user_image.h
cpu-accelctl.o libcpu_accel.o: cpu_accel.h ../../include/uapi/linux/cpu_accel.h
test-tlb-generation.o: ../../include/uapi/linux/cpu_accel.h
test-seccomp-jit-churn.o: CPPFLAGS := -D_GNU_SOURCE
test-sync-wait-prime.o: CPPFLAGS := -D_GNU_SOURCE
test-perf-event-open.o: CPPFLAGS := -D_GNU_SOURCE
test-user-escape-rescue.o: CPPFLAGS := -D_GNU_SOURCE -Wno-cpp -I../../include/uapi

clean:
	rm -f cpu-accelctl test-tlb-generation test-tlb-generation.o \
		test-tlb-cow-churn test-tlb-cow-churn.o \
		test-seccomp-jit-churn test-seccomp-jit-churn.o \
		test-sync-wait-prime test-sync-wait-prime.o \
		test-perf-event-open test-perf-event-open.o test-user-escape-rescue \
		test-user-escape-rescue.o \
		cpu-accelctl.o libcpu_accel.o
	$(MAKE) -C $(KDIR) M=$(CURDIR)/rcu-probe clean

.PHONY: all clean test test-sync-work-owner-stop test-perf-owner-stop \
	test-irq-storm test-timerlat-owner test-user-escape-fault \
	test-rcu-nocb-admission test-rcu-callback-routing rcu-probe
