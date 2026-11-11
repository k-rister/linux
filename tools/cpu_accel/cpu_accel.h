/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPU_ACCEL_H
#define CPU_ACCEL_H

#include <stdbool.h>
#include <stdint.h>

#include <linux/cpu_accel.h>

struct cpu_accel_handle {
	int fd;
	volatile struct cpu_accel_shared *shared;
	volatile struct cpu_accel_shared *worker_shared;
};

int cpu_accel_open(struct cpu_accel_handle *handle);
int cpu_accel_open_control(struct cpu_accel_handle *handle);
/* On success, handle owns fd; on failure, the caller still owns fd. */
int cpu_accel_attach_fd(struct cpu_accel_handle *handle, int fd);
int cpu_accel_attach_worker_fd(struct cpu_accel_handle *handle, int fd);
void cpu_accel_close(struct cpu_accel_handle *handle);
int cpu_accel_get_abi_info(struct cpu_accel_handle *handle,
			   struct cpu_accel_abi_info *info);
int cpu_accel_set_abi_version(struct cpu_accel_handle *handle,
			      struct cpu_accel_abi_select *select);
int cpu_accel_get_arch_telemetry(struct cpu_accel_handle *handle,
				 struct cpu_accel_arch_telemetry *telemetry);
int cpu_accel_x86_image_admission(struct cpu_accel_handle *handle,
				  struct cpu_accel_x86_image_admission *image);
int cpu_accel_configure(struct cpu_accel_handle *handle,
			const struct cpu_accel_config *config);
int cpu_accel_start(struct cpu_accel_handle *handle);
int cpu_accel_stop(struct cpu_accel_handle *handle);
int cpu_accel_exit(struct cpu_accel_handle *handle);
int cpu_accel_user_escape(struct cpu_accel_handle *handle);
int cpu_accel_reset(struct cpu_accel_handle *handle);
int cpu_accel_domain_config(struct cpu_accel_handle *handle,
			    struct cpu_accel_domain_config *config);
int cpu_accel_domain_start(struct cpu_accel_handle *handle);
int cpu_accel_domain_worker_join(
	struct cpu_accel_handle *handle,
	struct cpu_accel_domain_worker_join *join);
int cpu_accel_domain_worker_escape(
	struct cpu_accel_handle *handle,
	struct cpu_accel_domain_worker_escape *escape);
int cpu_accel_domain_status(struct cpu_accel_handle *handle,
			    struct cpu_accel_domain_status *status);
volatile struct cpu_accel_shared *cpu_accel_worker_mmap(
	struct cpu_accel_handle *handle, uint64_t mmap_token);
int cpu_accel_get_capabilities(struct cpu_accel_handle *handle,
			       struct cpu_accel_capabilities *capabilities);
int cpu_accel_region_create(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_create *create);
int cpu_accel_region_attach(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_ref *ref);
int cpu_accel_region_detach(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_ref *ref);
int cpu_accel_region_destroy(struct cpu_accel_handle *handle,
			     struct cpu_accel_region_ref *ref);
int cpu_accel_region_get_info(struct cpu_accel_handle *handle,
			      struct cpu_accel_region_info *info);
void *cpu_accel_region_mmap(struct cpu_accel_handle *handle,
			    const struct cpu_accel_region_create *create,
			    bool manager);
int cpu_accel_wait(struct cpu_accel_handle *handle, unsigned int timeout_ms);

#endif
