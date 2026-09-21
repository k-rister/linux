# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11
CPPFLAGS += -D_GNU_SOURCE -I../../usr/include -I../../include/uapi

all: cpu-accelctl

cpu-accelctl: cpu-accelctl.o libcpu_accel.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f cpu-accelctl cpu-accelctl.o libcpu_accel.o

.PHONY: all clean
