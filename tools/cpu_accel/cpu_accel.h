/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPU_ACCEL_H
#define CPU_ACCEL_H

#include <linux/cpu_accel.h>

struct cpu_accel_handle {
	int fd;
	volatile struct cpu_accel_shared *shared;
};

int cpu_accel_open(struct cpu_accel_handle *handle);
void cpu_accel_close(struct cpu_accel_handle *handle);
int cpu_accel_configure(struct cpu_accel_handle *handle,
			const struct cpu_accel_config *config);
int cpu_accel_start(struct cpu_accel_handle *handle);
int cpu_accel_stop(struct cpu_accel_handle *handle);
int cpu_accel_reset(struct cpu_accel_handle *handle);
int cpu_accel_wait(struct cpu_accel_handle *handle, unsigned int timeout_ms);

#endif
