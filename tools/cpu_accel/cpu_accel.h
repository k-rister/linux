/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPU_ACCEL_H
#define CPU_ACCEL_H

#include <stdint.h>

#include <linux/cpu_accel.h>

struct cpu_accel_handle {
	int fd;
	volatile struct cpu_accel_shared *shared;
	struct cpu_accel_shared_region *shared_region;
};

int cpu_accel_open(struct cpu_accel_handle *handle);
void cpu_accel_close(struct cpu_accel_handle *handle);
int cpu_accel_configure(struct cpu_accel_handle *handle,
			const struct cpu_accel_config *config);
int cpu_accel_start(struct cpu_accel_handle *handle);
int cpu_accel_stop(struct cpu_accel_handle *handle);
int cpu_accel_exit(struct cpu_accel_handle *handle);
int cpu_accel_user_escape(struct cpu_accel_handle *handle);
int cpu_accel_reset(struct cpu_accel_handle *handle);
int cpu_accel_shared_ready(struct cpu_accel_handle *handle,
				   uint32_t entry, uint64_t bytes);
int cpu_accel_shared_reclaim(struct cpu_accel_handle *handle,
				     uint32_t entry);
int cpu_accel_wait(struct cpu_accel_handle *handle, unsigned int timeout_ms);

#endif
