/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CPU_ACCEL_H
#define _UAPI_LINUX_CPU_ACCEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* ABI 25 adds experimental static multi-worker domain commands. */
#define CPU_ACCEL_ABI_VERSION		25
#define CPU_ACCEL_ABI_MIN_VERSION	23
#define CPU_ACCEL_MAP_SIZE		(64U * 1024U)
#define CPU_ACCEL_MAX_WORKERS		16U
#define CPU_ACCEL_MAX_WORK_BYTES	(1ULL * 1024ULL * 1024ULL * 1024ULL)
#define CPU_ACCEL_MAX_MEMMOVE_WORK_BYTES	(64ULL * 1024ULL)

#define CPU_ACCEL_DEFAULT_DURATION_NS	(100ULL * 1000ULL * 1000ULL)
#define CPU_ACCEL_DEFAULT_PERIOD_NS	(1ULL * 1000ULL * 1000ULL)
#define CPU_ACCEL_MAX_DURATION_NS	(24ULL * 60ULL * 60ULL * 1000ULL * 1000ULL * 1000ULL)

enum cpu_accel_state {
	CPU_ACCEL_STATE_IDLE = 0,
	CPU_ACCEL_STATE_READY,
	CPU_ACCEL_STATE_RUNNING,
	CPU_ACCEL_STATE_COMPLETE,
	CPU_ACCEL_STATE_STOPPED,
	CPU_ACCEL_STATE_ERROR,
	CPU_ACCEL_STATE_WATCHDOG,
	CPU_ACCEL_STATE_ESCAPED,
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
	CPU_ACCEL_BACKEND_X86_RING3,
	CPU_ACCEL_BACKEND_X86_DIRECT_APIC,
};

/* Capabilities reported by the selected backend for the configured workload. */
#define CPU_ACCEL_BACKEND_FLAG_USER_ESCAPE	(1U << 0)
/* Remote reschedule IPIs are deferred while the x86 direct backend owns a CPU. */
#define CPU_ACCEL_BACKEND_FLAG_RESCHEDULE_DEFER	(1U << 1)
/* Call-function IPIs are deferred while the x86 direct backend owns a CPU. */
#define CPU_ACCEL_BACKEND_FLAG_CALL_FUNCTION_DEFER	(1U << 2)

enum cpu_accel_recovery_state {
	CPU_ACCEL_RECOVERY_NONE = 0,
	CPU_ACCEL_RECOVERY_REQUESTED,
	CPU_ACCEL_RECOVERY_SUCCEEDED,
	CPU_ACCEL_RECOVERY_UNSUPPORTED,
	CPU_ACCEL_RECOVERY_TIMEOUT,
	CPU_ACCEL_RECOVERY_FAILED,
};

enum cpu_accel_workload {
	CPU_ACCEL_WORKLOAD_TIMESTAMP = 0,
	CPU_ACCEL_WORKLOAD_MEMMOVE,
	CPU_ACCEL_WORKLOAD_USER_REGION,
	CPU_ACCEL_WORKLOAD_USER_CYCLICTEST,
	/* Noncooperative ring-3 fault-injection workload; requires USER_ESCAPE. */
	CPU_ACCEL_WORKLOAD_USER_HANG,
	/* Test-only ring-3 workload that reads one reclaimable file-backed page. */
	CPU_ACCEL_WORKLOAD_USER_RECLAIM,
	/* Test-only ring-3 workload that takes a signal checkpoint each period. */
	CPU_ACCEL_WORKLOAD_USER_DEATH,
	/* Tight-loop inter-iteration (oslat) latency probe on the owned CPU. */
	CPU_ACCEL_WORKLOAD_USER_OSLAT,
	/* Static linear pipeline using bounded SPSC queues. */
	CPU_ACCEL_WORKLOAD_USER_PIPELINE,
};

struct cpu_accel_config {
	__u32 cpu;
	__u32 flags;
	__u32 workload;
	__u32 reserved;
	__u64 work_bytes;
	__u64 duration_ns;
	__u64 period_ns;
	/* User image metadata; zero for kernel-backed workloads. */
	__u64 user_entry_ip;
	__u64 user_stack_top;
	__u64 user_stack_bytes;
	__u64 user_image_start;
	__u64 user_image_bytes;
	__u64 user_arg;
	__u64 user_escape_ip;
	/* One read-only test mapping; zero for all other workloads. */
	__u64 user_reclaim_page;
};

struct cpu_accel_shared {
	__u32 abi_version;
	__u32 struct_size;
	/*
	 * Compile-time layout fingerprint (offsetof-based). The module and tool
	 * verify it at open(), catching reordered or inserted fields even when
	 * abi_version and struct_size agree.
	 */
	__u64 layout_fingerprint;
	__u32 state;
	__u32 cpu;
	__u32 flags;
	__u32 backend;
	__u32 backend_flags;
	__u32 recovery_state;
	__s32 recovery_error;
	__u64 recovery_requested_ns;
	__u64 recovery_completed_ns;
	__u32 recovery_attempts;
	__u32 reserved_recovery;
	__u32 stop_requested;
	__u32 region_count;
	__u32 mode;
	__u64 sequence;
	__u64 start_ns;
	__u64 end_ns;
	__u64 duration_ns;
	__u64 period_ns;
	__u32 workload;
	__u32 reserved_workload;
	__u64 region_set_epoch;
	__u64 lifecycle_entry_ns;
	__u64 lifecycle_exit_ns;
	/* User image interval: ring-3 entry through terminal USER_EXIT. */
	__u64 user_active_start_ns;
	__u64 user_active_end_ns;
	__u64 user_escape_count;
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
	/* Native x86 flush batches targeting this CPU while accelerator-owned. */
	__u64 arch_tlb_shootdown_targets;
	__u64 arch_reschedule_deferred;
	__u64 arch_call_function_deferred;
	__u64 arch_nmi_count;
	__u64 arch_mce_exception_count;
	__u64 arch_mce_poll_count;
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
};

/*
 * CPU_ACCEL_LAYOUT_FINGERPRINT() packs the offsetof() of four sentinel fields
 * into one 64-bit value. Each offset occupies its own 16-bit lane.
 */
#define CPU_ACCEL_LAYOUT_FINGERPRINT()					\
	(((__u64)offsetof(struct cpu_accel_shared, duration_ns) & 0xFFFFULL) | \
	(((__u64)offsetof(struct cpu_accel_shared, region_set_epoch) & 0xFFFFULL) << 16) | \
	(((__u64)offsetof(struct cpu_accel_shared, context_switches) & 0xFFFFULL) << 32) | \
	(((__u64)offsetof(struct cpu_accel_shared, migration_detected) & 0xFFFFULL) << 48))

#define CPU_ACCEL_REGION_PERM_OWNER_READ	(1U << 0)
#define CPU_ACCEL_REGION_PERM_OWNER_WRITE	(1U << 1)
#define CPU_ACCEL_REGION_PERM_MANAGER_READ	(1U << 2)
#define CPU_ACCEL_REGION_PERM_MANAGER_WRITE	(1U << 3)

#define CPU_ACCEL_REGION_PERM_BIDIRECTIONAL \
	(CPU_ACCEL_REGION_PERM_OWNER_READ | \
	 CPU_ACCEL_REGION_PERM_OWNER_WRITE | \
	 CPU_ACCEL_REGION_PERM_MANAGER_READ | \
	 CPU_ACCEL_REGION_PERM_MANAGER_WRITE)
#define CPU_ACCEL_REGION_PERM_MANAGER_TO_OWNER \
	(CPU_ACCEL_REGION_PERM_OWNER_READ | \
	 CPU_ACCEL_REGION_PERM_MANAGER_READ | \
	 CPU_ACCEL_REGION_PERM_MANAGER_WRITE)
#define CPU_ACCEL_REGION_PERM_OWNER_TO_MANAGER \
	(CPU_ACCEL_REGION_PERM_OWNER_READ | \
	 CPU_ACCEL_REGION_PERM_OWNER_WRITE | \
	 CPU_ACCEL_REGION_PERM_MANAGER_READ)

enum cpu_accel_region_state {
	CPU_ACCEL_REGION_CREATED = 0,
	CPU_ACCEL_REGION_ATTACHED,
	CPU_ACCEL_REGION_ACTIVE,
	CPU_ACCEL_REGION_COMPLETE,
	CPU_ACCEL_REGION_ERROR,
};

struct cpu_accel_capabilities {
	__u32 max_region_count;
	__u32 reserved;
	__u64 max_region_bytes;
	__u64 max_total_region_bytes;
	__u64 supported_region_permissions;
};

struct cpu_accel_abi_info {
	__u32 current_abi;
	__u32 min_abi;
	__u32 max_abi;
	__u32 selected_abi;
	__u32 reserved;
};

struct cpu_accel_abi_select {
	__u32 requested_abi;
	__u32 selected_abi;
	__u32 reserved;
};

struct cpu_accel_arch_telemetry {
	__u64 arch_irq_count;
	__u64 arch_ipi_count;
	__u64 arch_tlb_count;
	__u64 arch_tlb_shootdown_targets;
	__u64 arch_reschedule_deferred;
	__u64 arch_call_function_deferred;
	__u64 arch_nmi_count;
	__u64 arch_mce_exception_count;
	__u64 arch_mce_poll_count;
	__u32 arch_counters_valid;
	__u32 reserved;
};

struct cpu_accel_x86_image_admission {
	__u64 user_entry_ip;
	__u64 user_stack_top;
	__u64 user_stack_bytes;
	__u64 user_image_start;
	__u64 user_image_bytes;
	__u64 user_arg;
	__u64 user_escape_ip;
	__u64 user_reclaim_page;
	__u32 image_handle;
	__u32 reserved;
};

struct cpu_accel_region_create {
	__u64 size_bytes;
	__u32 permissions;
	__s32 numa_node;
	__u32 region_type;
	__u32 region_id;
	__u64 mmap_token;
	__u64 epoch;
};

struct cpu_accel_region_ref {
	__u32 region_id;
	__u32 reserved;
	__u64 epoch;
};

struct cpu_accel_region_info {
	__u32 region_id;
	__u32 state;
	__u32 permissions;
	__s32 numa_node;
	__u32 manager_mappings;
	__u32 owner_mappings;
	__u64 size_bytes;
	__u64 mmap_token;
	__u64 epoch;
};

struct cpu_accel_domain_config {
	__u32 cpu_count;
	__u32 flags;
	__u32 workload;
	__u32 reserved;
	__u64 work_bytes;
	__u64 duration_ns;
	__u64 period_ns;
	__u32 cpus[CPU_ACCEL_MAX_WORKERS];
	__u32 reserved_words[4];
	__u64 mmap_tokens[CPU_ACCEL_MAX_WORKERS];
};

struct cpu_accel_domain_worker_join {
	__u64 user_entry_ip;
	__u64 user_stack_top;
	__u64 user_stack_bytes;
	__u64 user_image_start;
	__u64 user_image_bytes;
	__u64 user_arg;
	__u64 user_escape_ip;
	__u64 user_reclaim_page;
	__u32 cpu;
	__u32 worker_index;
	__u32 reserved;
};

struct cpu_accel_domain_worker_escape {
	__u32 worker_index;
	__u32 reserved;
};

struct cpu_accel_domain_status {
	__u32 state;
	__u32 worker_count;
	__u32 joined_workers;
	__u32 active_workers;
	__u32 terminal_workers;
	__s32 result;
	__u32 reserved;
	__u64 start_ns;
	__u64 end_ns;
	__u32 worker_states[CPU_ACCEL_MAX_WORKERS];
	__s32 worker_results[CPU_ACCEL_MAX_WORKERS];
};

#define CPU_ACCEL_REGION_TYPE_WORKLOAD		0U
#define CPU_ACCEL_REGION_TYPE_SPSC_QUEUE	1U

#define CPU_ACCEL_MAX_SPSC_QUEUES \
	(CPU_ACCEL_MAX_WORKERS - 1U)
#define CPU_ACCEL_SPSC_MAX_CAPACITY		1024U
#define CPU_ACCEL_SPSC_ABI_VERSION		1U
#define CPU_ACCEL_SPSC_MAGIC			0x53505343U
#define CPU_ACCEL_SPSC_CACHE_LINE		64U
#define CPU_ACCEL_SPSC_SLOTS_OFFSET		256U
#define CPU_ACCEL_SPSC_OWNER_PRODUCER_READY	1U
#define CPU_ACCEL_SPSC_OWNER_CONSUMER_READY	2U
#define CPU_ACCEL_SPSC_PRODUCER_DONE		4U

struct cpu_accel_spsc_producer_line {
	__u64 head;
	__u64 enqueued;
	__u64 full_events;
	__u64 checksum;
	__u64 reserved[4];
};

struct cpu_accel_spsc_consumer_line {
	__u64 tail;
	__u64 dequeued;
	__u64 empty_events;
	__u64 checksum;
	__u64 reserved[4];
};

struct cpu_accel_spsc_slot {
	__u64 sequence;
	__u64 value;
};

struct cpu_accel_spsc_queue {
	__u32 magic;
	__u32 abi_version;
	__u32 header_size;
	__u32 capacity;
	__u32 slot_mask;
	__u32 producer_worker;
	__u32 consumer_worker;
	__u32 owner_ready;
	__u64 region_size;
	__u64 epoch;
	__u64 mmap_token;
	__u8 header_pad[8];
	struct cpu_accel_spsc_producer_line producer;
	__u8 producer_pad[CPU_ACCEL_SPSC_CACHE_LINE -
			  sizeof(struct cpu_accel_spsc_producer_line)];
	struct cpu_accel_spsc_consumer_line consumer;
	__u8 consumer_pad[CPU_ACCEL_SPSC_CACHE_LINE -
			  sizeof(struct cpu_accel_spsc_consumer_line)];
	__u8 slots_pad[CPU_ACCEL_SPSC_CACHE_LINE];
	struct cpu_accel_spsc_slot slots[];
};

struct cpu_accel_domain_queue_config {
	__u32 queue_count;
	__u32 reserved;
	__u32 region_ids[CPU_ACCEL_MAX_SPSC_QUEUES];
	__u32 capacities[CPU_ACCEL_MAX_SPSC_QUEUES];
};

#define CPU_ACCEL_IOC_MAGIC	'C'
#define CPU_ACCEL_IOC_CONFIG	_IOW(CPU_ACCEL_IOC_MAGIC, 0x00, \
						struct cpu_accel_config)
#define CPU_ACCEL_IOC_START	_IO(CPU_ACCEL_IOC_MAGIC, 0x01)
#define CPU_ACCEL_IOC_STOP	_IO(CPU_ACCEL_IOC_MAGIC, 0x02)
#define CPU_ACCEL_IOC_RESET	_IO(CPU_ACCEL_IOC_MAGIC, 0x03)
#define CPU_ACCEL_IOC_EXIT	_IO(CPU_ACCEL_IOC_MAGIC, 0x04)
#define CPU_ACCEL_IOC_GET_CAPABILITIES	_IOR(CPU_ACCEL_IOC_MAGIC, 0x05, \
						struct cpu_accel_capabilities)
#define CPU_ACCEL_IOC_REGION_CREATE	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x06, \
						struct cpu_accel_region_create)
#define CPU_ACCEL_IOC_REGION_ATTACH	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x07, \
						struct cpu_accel_region_ref)
#define CPU_ACCEL_IOC_REGION_DETACH	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x08, \
						struct cpu_accel_region_ref)
#define CPU_ACCEL_IOC_REGION_DESTROY	_IOW(CPU_ACCEL_IOC_MAGIC, 0x09, \
						struct cpu_accel_region_ref)
#define CPU_ACCEL_IOC_REGION_GET_INFO	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x0a, \
						struct cpu_accel_region_info)
#define CPU_ACCEL_IOC_USER_EXIT	_IO(CPU_ACCEL_IOC_MAGIC, 0x0b)
#define CPU_ACCEL_IOC_USER_ESCAPE	_IO(CPU_ACCEL_IOC_MAGIC, 0x0c)

#define CPU_ACCEL_IOC_GET_ABI_INFO	_IOR(CPU_ACCEL_IOC_MAGIC, 0x0d, \
						struct cpu_accel_abi_info)
#define CPU_ACCEL_IOC_SET_ABI_VERSION	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x0e, \
						struct cpu_accel_abi_select)
#define CPU_ACCEL_IOC_GET_ARCH_TELEMETRY	_IOR(CPU_ACCEL_IOC_MAGIC, 0x0f, \
						struct cpu_accel_arch_telemetry)
#define CPU_ACCEL_IOC_X86_IMAGE_ADMISSION	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x10, \
						struct cpu_accel_x86_image_admission)
#define CPU_ACCEL_IOC_DOMAIN_CONFIG	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x11, \
							struct cpu_accel_domain_config)
#define CPU_ACCEL_IOC_DOMAIN_START	_IO(CPU_ACCEL_IOC_MAGIC, 0x12)
#define CPU_ACCEL_IOC_DOMAIN_WORKER_JOIN	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x13, \
							struct cpu_accel_domain_worker_join)
#define CPU_ACCEL_IOC_DOMAIN_STATUS	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x14, \
							struct cpu_accel_domain_status)

#define CPU_ACCEL_IOC_DOMAIN_WORKER_ESCAPE	_IOW(CPU_ACCEL_IOC_MAGIC, 0x15, \
							struct cpu_accel_domain_worker_escape)
#define CPU_ACCEL_IOC_DOMAIN_QUEUE_CONFIG	_IOWR(CPU_ACCEL_IOC_MAGIC, 0x16, \
							struct cpu_accel_domain_queue_config)

#endif /* _UAPI_LINUX_CPU_ACCEL_H */
