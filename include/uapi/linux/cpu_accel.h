/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CPU_ACCEL_H
#define _UAPI_LINUX_CPU_ACCEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* The first prototype uses a fixed-size shared control and sample area. */
#define CPU_ACCEL_ABI_VERSION		1
#define CPU_ACCEL_MAP_SIZE		(64U * 1024U)
#define CPU_ACCEL_MAX_SAMPLES		2048U

#define CPU_ACCEL_DEFAULT_DURATION_NS	(100ULL * 1000ULL * 1000ULL)
#define CPU_ACCEL_DEFAULT_PERIOD_NS	(1ULL * 1000ULL * 1000ULL)
#define CPU_ACCEL_MAX_DURATION_NS	(5ULL * 1000ULL * 1000ULL * 1000ULL)

enum cpu_accel_state {
	CPU_ACCEL_STATE_IDLE = 0,
	CPU_ACCEL_STATE_READY,
	CPU_ACCEL_STATE_RUNNING,
	CPU_ACCEL_STATE_COMPLETE,
	CPU_ACCEL_STATE_STOPPED,
	CPU_ACCEL_STATE_ERROR,
	CPU_ACCEL_STATE_WATCHDOG,
};

/* The first prototype always runs with local interrupts disabled. */
#define CPU_ACCEL_FLAG_IRQS_OFF		(1U << 0)
/* Keep the target owned until STOP or the configured watchdog fires. */
#define CPU_ACCEL_FLAG_PERSISTENT	(1U << 1)

struct cpu_accel_config {
	__u32 cpu;
	__u32 flags;
	__u64 duration_ns;
	__u64 period_ns;
};

struct cpu_accel_sample {
	__u64 timestamp_ns;
	__s64 lateness_ns;
};

struct cpu_accel_shared {
	__u32 abi_version;
	__u32 struct_size;
	__u32 state;
	__u32 cpu;
	__u32 flags;
	__u32 stop_requested;
	__u32 samples_valid;
	__u32 reserved0;
	__u64 sequence;
	__u64 start_ns;
	__u64 end_ns;
	__u64 duration_ns;
	__u64 period_ns;
	__u64 samples_produced;
	__u64 max_lateness_ns;
	__u64 min_lateness_ns;
	__u64 last_lateness_ns;
	struct cpu_accel_sample samples[CPU_ACCEL_MAX_SAMPLES];
};

#define CPU_ACCEL_IOC_MAGIC	'C'
#define CPU_ACCEL_IOC_CONFIG	_IOW(CPU_ACCEL_IOC_MAGIC, 0x00, \
					struct cpu_accel_config)
#define CPU_ACCEL_IOC_START	_IO(CPU_ACCEL_IOC_MAGIC, 0x01)
#define CPU_ACCEL_IOC_STOP	_IO(CPU_ACCEL_IOC_MAGIC, 0x02)
#define CPU_ACCEL_IOC_RESET	_IO(CPU_ACCEL_IOC_MAGIC, 0x03)
#define CPU_ACCEL_IOC_EXIT	_IO(CPU_ACCEL_IOC_MAGIC, 0x04)

#endif /* _UAPI_LINUX_CPU_ACCEL_H */
