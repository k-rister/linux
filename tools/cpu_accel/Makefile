# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -fno-stack-protector
CPPFLAGS += -D_GNU_SOURCE -Wno-cpp -I../../include/uapi -I../../usr/include

all: cpu-accelctl

test: cpu-accelctl
	CPU_ACCELCTL=./cpu-accelctl sh ./test-cpu-accel.sh

cpu-accelctl: cpu-accelctl.o libcpu_accel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

cpu-accelctl.o: cpu_accel_user_image.h
cpu-accelctl.o libcpu_accel.o: cpu_accel.h ../../include/uapi/linux/cpu_accel.h

clean:
	rm -f cpu-accelctl cpu-accelctl.o libcpu_accel.o

.PHONY: all clean test
