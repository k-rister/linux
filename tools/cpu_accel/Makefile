# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -fno-stack-protector -pthread
CPPFLAGS += -D_GNU_SOURCE -Wno-cpp -I../../include/uapi -I../../usr/include

all: cpu-accelctl

test: cpu-accelctl test-tlb-generation test-tlb-cow-churn
	CPU_ACCELCTL=./cpu-accelctl sh ./test-cpu-accel.sh

test-sync-work-owner-stop: cpu-accelctl test-sync-wait-prime
	CPU_ACCELCTL=./cpu-accelctl CPU_ACCEL_SYNC_WAIT_PRIME=./test-sync-wait-prime \
		sh ./test-sync-work-owner-stop.sh

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

cpu-accelctl.o: cpu_accel_user_image.h
cpu-accelctl.o libcpu_accel.o: cpu_accel.h ../../include/uapi/linux/cpu_accel.h
test-tlb-generation.o: ../../include/uapi/linux/cpu_accel.h
test-seccomp-jit-churn.o: CPPFLAGS := -D_GNU_SOURCE
test-sync-wait-prime.o: CPPFLAGS := -D_GNU_SOURCE

clean:
	rm -f cpu-accelctl test-tlb-generation test-tlb-generation.o \
		test-tlb-cow-churn test-tlb-cow-churn.o \
		test-seccomp-jit-churn test-seccomp-jit-churn.o \
		test-sync-wait-prime test-sync-wait-prime.o \
		cpu-accelctl.o libcpu_accel.o

.PHONY: all clean test test-sync-work-owner-stop
