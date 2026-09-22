/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CPU_ACCEL_H
#define _UAPI_LINUX_CPU_ACCEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* The first prototype uses a fixed-size shared control and sample area. */
#define CPU_ACCEL_ABI_VERSION		6
#define CPU_ACCEL_MAP_SIZE		(64U * 1024U)
#define CPU_ACCEL_MAX_SAMPLES		2048U
#define CPU_ACCEL_MAX_WORK_BYTES	(64U * 1024U)

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

enum cpu_accel_mode {
	CPU_ACCEL_MODE_LINUX = 0,
	CPU_ACCEL_MODE_ENTERING,
	CPU_ACCEL_MODE_ACCELERATOR,
	CPU_ACCEL_MODE_EXITING,
	CPU_ACCEL_MODE_RECOVERY,
};

/* The first prototype always runs with local interrupts disabled. */
#define CPU_ACCEL_FLAG_IRQS_OFF		(1U << 0)
/* Keep the target owned until STOP or the configured watchdog fires. */
#define CPU_ACCEL_FLAG_PERSISTENT	(1U << 1)
/* Refuse entry when scheduler or softirq work is already pending. */
#define CPU_ACCEL_FLAG_REQUIRE_QUIESCENT	(1U << 2)
/* Move active migratable IRQs away from the accelerator CPU before entry. */
#define CPU_ACCEL_FLAG_IRQ_QUARANTINE	(1U << 3)

enum cpu_accel_backend {
	CPU_ACCEL_BACKEND_GENERIC_SMP = 0,
	CPU_ACCEL_BACKEND_X86_STAGED_IPI,
};

enum cpu_accel_workload {
	CPU_ACCEL_WORKLOAD_TIMESTAMP = 0,
	CPU_ACCEL_WORKLOAD_MEMMOVE,
};

struct cpu_accel_config {
	__u32 cpu;
	__u32 flags;
	__u32 workload;
	__u32 reserved;
	__u64 work_bytes;
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
	__u32 backend;
	__u32 backend_flags;
	__u32 stop_requested;
	__u32 samples_valid;
	__u32 mode;
	__u64 sequence;
	__u64 start_ns;
	__u64 end_ns;
	__u64 duration_ns;
	__u64 period_ns;
	__u32 workload;
	__u32 reserved_workload;
	__u64 work_bytes;
	__u64 work_iterations;
	__u64 samples_produced;
	__u64 max_lateness_ns;
	__u64 min_lateness_ns;
	__u64 last_lateness_ns;
	__u64 lifecycle_entry_ns;
	__u64 lifecycle_exit_ns;
	__u64 irq_count;
	__u32 irq_quarantined;
	__u32 irq_quarantine_blockers;
	__u64 softirq_count;
	__u64 timer_softirq_count;
	__u64 hrtimer_softirq_count;
	__u64 rcu_softirq_count;
	__u64 sched_softirq_count;
	__u64 workqueue_queued;
	__u64 workqueue_executed;
	__u64 context_switches;
	__u64 need_resched_samples;
	__u64 arch_irq_count;
	__u64 arch_ipi_count;
	__u64 arch_tlb_count;
	__u32 need_resched_entry;
	__u32 need_resched_exit;
	__u32 softirq_pending_entry;
	__u32 softirq_pending_exit;
	__u32 preempt_count_entry;
	__u32 preempt_count_exit;
	__u32 arch_counters_valid;
	__u32 lifecycle_cpu_entry;
	__u32 lifecycle_cpu_exit;
	__u32 migration_detected;
	__u32 reserved1;
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
