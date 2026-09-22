// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel_stat.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/processor.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <uapi/linux/cpu_accel.h>

#ifdef CONFIG_X86
#include <asm/hardirq.h>
#endif

#include <trace/events/workqueue.h>

struct cpu_accel_observation {
	u64 lifecycle_entry_ns;
	u64 irq_entry;
	u64 softirq_entry;
	u64 timer_softirq_entry;
	u64 hrtimer_softirq_entry;
	u64 rcu_softirq_entry;
	u64 sched_softirq_entry;
	u64 context_switches_entry;
	u64 arch_irq_entry;
	u64 arch_ipi_entry;
	u64 arch_tlb_entry;
	u64 need_resched_samples;
	u32 need_resched_entry;
	u32 softirq_pending_entry;
	u32 preempt_count_entry;
	u32 cpu;
};

typedef void (*cpu_accel_entry_fn)(void *data);

#ifdef CONFIG_X86
/*
 * Staged x86 handoff: one normal IPI enters the callback, after which the
 * callback owns execution until it returns. A future direct APIC handoff can
 * replace this function without changing the lifecycle or control ABI.
 */
static int cpu_accel_arch_enter(unsigned int cpu, cpu_accel_entry_fn entry,
				void *data)
{
	return smp_call_function_single(cpu, entry, data, 1);
}

static u32 cpu_accel_backend_id(void)
{
	return CPU_ACCEL_BACKEND_X86_STAGED_IPI;
}
#else
static int cpu_accel_arch_enter(unsigned int cpu, cpu_accel_entry_fn entry,
				void *data)
{
	return smp_call_function_single(cpu, entry, data, 1);
}

static u32 cpu_accel_backend_id(void)
{
	return CPU_ACCEL_BACKEND_GENERIC_SMP;
}
#endif

#ifdef CONFIG_X86
static u64 cpu_accel_arch_irq_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);
	u64 count = 0;

	for (unsigned int index = 0; index < IRQ_COUNT_MAX; index++)
		count += READ_ONCE(stats->counts[index]);

	return count;
}

static u64 cpu_accel_arch_ipi_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);

	return READ_ONCE(stats->counts[IRQ_COUNT_RESCHEDULE]) +
		READ_ONCE(stats->counts[IRQ_COUNT_CALL_FUNCTION]);
}

static u64 cpu_accel_arch_tlb_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);

	return READ_ONCE(stats->counts[IRQ_COUNT_TLB]);
}

static bool cpu_accel_arch_counters_valid(void)
{
	return true;
}
#else
static u64 cpu_accel_arch_irq_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_ipi_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_tlb_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static bool cpu_accel_arch_counters_valid(void)
{
	return false;
}
#endif

static u64 cpu_accel_counter_delta(u64 end, u64 start)
{
	return end >= start ? end - start : 0;
}

enum cpu_accel_region_owner {
	CPU_ACCEL_REGION_LINUX = CPU_ACCEL_SHARED_OWNER_LINUX,
	CPU_ACCEL_REGION_READY = CPU_ACCEL_SHARED_OWNER_READY,
	CPU_ACCEL_REGION_ACCELERATOR = CPU_ACCEL_SHARED_OWNER_ACCELERATOR,
	CPU_ACCEL_REGION_COMPLETE = CPU_ACCEL_SHARED_OWNER_COMPLETE,
	CPU_ACCEL_REGION_ERROR = CPU_ACCEL_SHARED_OWNER_ERROR,
};

struct cpu_accel_region {
	void *address;
	size_t bytes;
	u64 epoch;
	u32 owner;
};

struct cpu_accel_device {
	struct miscdevice misc;
	/* Serializes control-plane state transitions. */
	struct mutex lock;
	struct completion lifecycle_done;
	struct task_struct *lifecycle_thread;
	struct timer_list watchdog_timer;
	struct irq_accel_quarantine *irq_quarantine;
	struct cpu_accel_shared *shared;
	struct cpu_accel_shared_region *shared_region;
	struct cpu_accel_config config;
	atomic_t opened;
	atomic_t enter_requested;
	atomic_t stop_requested;
	atomic_t watchdog_fired;
	atomic_t running;
	atomic64_t workqueue_queued;
	atomic64_t workqueue_executed;
	struct cpu_accel_region work_region;
	unsigned int sequence;
	unsigned int controller_cpu;
	unsigned int irq_quarantined;
	unsigned int irq_quarantine_blockers;
	int lifecycle_ret;
	bool configured;
	bool workqueue_reserved;
	bool work_region_allocated;
	bool work_region_shared;
};

static struct cpu_accel_device cpu_accel;

static int cpu_accel_restore_irq_quarantine(struct cpu_accel_device *dev)
{
	int ret;

	if (!dev->irq_quarantine)
		return 0;
	ret = irq_accel_restore_cpu(dev->irq_quarantine);
	if (!ret)
		dev->irq_quarantine = NULL;
	return ret;
}

static void cpu_accel_release_workqueue(struct cpu_accel_device *dev)
{
	if (!dev->workqueue_reserved)
		return;
	workqueue_accel_cpu_release(dev->config.cpu);
	dev->workqueue_reserved = false;
}

static bool cpu_accel_shared_workload(struct cpu_accel_device *dev)
{
	return dev->work_region_shared;
}

static struct cpu_accel_shared_entry *
cpu_accel_work_entry(struct cpu_accel_device *dev)
{
	if (!dev->shared_region ||
	    dev->config.shared_entry >= CPU_ACCEL_SHARED_ENTRY_COUNT)
		return NULL;
	return &dev->shared_region->entries[dev->config.shared_entry];
}

static void cpu_accel_publish_region_owner(struct cpu_accel_device *dev,
						 u32 owner)
{
	struct cpu_accel_shared_entry *entry;

	if (!cpu_accel_shared_workload(dev))
		return;
	entry = cpu_accel_work_entry(dev);
	if (!entry)
		return;
	entry->epoch = dev->work_region.epoch;
	entry->bytes = dev->config.work_bytes;
	/* Publish data, epoch, and length before publishing ownership. */
	smp_wmb();
	WRITE_ONCE(entry->owner, owner);
	WRITE_ONCE(dev->shared->shared_entry, dev->config.shared_entry);
	WRITE_ONCE(dev->shared->shared_owner, owner);
	WRITE_ONCE(dev->shared->shared_epoch, dev->work_region.epoch);
}

static int cpu_accel_region_recover(struct cpu_accel_device *dev)
{
	u32 owner = READ_ONCE(dev->work_region.owner);

	if (owner == CPU_ACCEL_REGION_LINUX)
		return 0;
	if (owner == CPU_ACCEL_REGION_ACCELERATOR && dev->lifecycle_thread)
		return -EBUSY;
	if (owner != CPU_ACCEL_REGION_ERROR &&
	    owner != CPU_ACCEL_REGION_ACCELERATOR)
		return -EBUSY;
	/* Publish recovery writes before making the region reusable. */
	smp_wmb();
	WRITE_ONCE(dev->work_region.owner, CPU_ACCEL_REGION_LINUX);
	cpu_accel_publish_region_owner(dev, CPU_ACCEL_REGION_LINUX);
	return 0;
}

static int cpu_accel_region_begin(struct cpu_accel_device *dev)
{
	u32 required_owner;

	if (!dev->work_region.address)
		return 0;
	required_owner = cpu_accel_shared_workload(dev) ?
		CPU_ACCEL_REGION_READY : CPU_ACCEL_REGION_LINUX;
	if (READ_ONCE(dev->work_region.owner) != required_owner)
		return -EBUSY;
	if (!cpu_accel_shared_workload(dev)) {
		dev->work_region.epoch++;
		if (!dev->work_region.epoch)
			dev->work_region.epoch++;
	}
	/* Publish the epoch before the target can observe accelerator ownership. */
	smp_wmb();
	WRITE_ONCE(dev->work_region.owner, CPU_ACCEL_REGION_ACCELERATOR);
	cpu_accel_publish_region_owner(dev, CPU_ACCEL_REGION_ACCELERATOR);
	return 0;
}

static void cpu_accel_region_end(struct cpu_accel_device *dev, u32 state)
{
	u32 owner;

	if (!dev->work_region.address)
		return;
	if (state == CPU_ACCEL_STATE_ERROR)
		owner = CPU_ACCEL_REGION_ERROR;
	else if (!cpu_accel_shared_workload(dev))
		owner = CPU_ACCEL_REGION_LINUX;
	else
		owner = CPU_ACCEL_REGION_COMPLETE;
	/* Publish workload writes before transferring region ownership. */
	smp_wmb();
	WRITE_ONCE(dev->work_region.owner, owner);
	cpu_accel_publish_region_owner(dev, owner);
}

static int cpu_accel_shared_reclaim(struct cpu_accel_device *dev,
					   u32 entry_index, bool force)
{
	u32 owner;

	if (!dev->work_region_shared ||
	    entry_index != dev->config.shared_entry ||
	    entry_index >= CPU_ACCEL_SHARED_ENTRY_COUNT)
		return -EINVAL;
	if (dev->lifecycle_thread)
		return -EBUSY;
	owner = READ_ONCE(dev->work_region.owner);
	if (owner == CPU_ACCEL_REGION_LINUX)
		return -EALREADY;
	if (owner == CPU_ACCEL_REGION_COMPLETE && !force)
		return -EACCES;
	if (owner != CPU_ACCEL_REGION_READY &&
	    owner != CPU_ACCEL_REGION_COMPLETE &&
	    owner != CPU_ACCEL_REGION_ERROR)
		return -EBUSY;
	/* Publish completed or discarded data before returning ownership. */
	smp_wmb();
	WRITE_ONCE(dev->work_region.owner, CPU_ACCEL_REGION_LINUX);
	cpu_accel_publish_region_owner(dev, CPU_ACCEL_REGION_LINUX);
	dev->work_region.bytes = 0;
	return 0;
}

static int cpu_accel_shared_ready(struct cpu_accel_device *dev,
					 const struct cpu_accel_shared_handoff *handoff)
{
	struct cpu_accel_shared_entry *entry;

	if (!dev->configured || !dev->work_region_shared ||
	    dev->lifecycle_thread || handoff->reserved ||
	    handoff->entry != dev->config.shared_entry ||
	    handoff->entry >= CPU_ACCEL_SHARED_ENTRY_COUNT ||
	    handoff->bytes != dev->config.work_bytes || handoff->bytes < 64 ||
	    handoff->bytes > CPU_ACCEL_SHARED_ENTRY_BYTES / 2)
		return -EINVAL;
	if (READ_ONCE(dev->work_region.owner) != CPU_ACCEL_REGION_LINUX)
		return -EBUSY;
	entry = cpu_accel_work_entry(dev);
	if (!entry)
		return -ENODEV;
	dev->work_region.epoch++;
	if (!dev->work_region.epoch)
		dev->work_region.epoch++;
	dev->work_region.address = entry->data;
	dev->work_region.bytes = handoff->bytes * 2;
	/* The caller initialized the data before this release handoff. */
	smp_wmb();
	WRITE_ONCE(dev->work_region.owner, CPU_ACCEL_REGION_READY);
	cpu_accel_publish_region_owner(dev, CPU_ACCEL_REGION_READY);
	return 0;
}

static int cpu_accel_free_workload(struct cpu_accel_device *dev)
{
	int ret;

	if (dev->work_region_shared &&
	    READ_ONCE(dev->work_region.owner) != CPU_ACCEL_REGION_LINUX) {
		ret = cpu_accel_shared_reclaim(dev, dev->config.shared_entry, true);
		if (ret)
			return ret;
	}
	ret = cpu_accel_region_recover(dev);
	if (ret)
		return ret;
	if (dev->work_region_allocated)
		kfree(dev->work_region.address);
	dev->work_region.address = NULL;
	dev->work_region.bytes = 0;
	dev->work_region.owner = CPU_ACCEL_REGION_LINUX;
	dev->work_region_allocated = false;
	dev->work_region_shared = false;
	return 0;
}

static int cpu_accel_prepare_workload(struct cpu_accel_device *dev,
					      const struct cpu_accel_config *config)
{
	void *buffer;
	struct cpu_accel_shared_entry *entry;
	size_t bytes;
	size_t size;

	if (config->workload == CPU_ACCEL_WORKLOAD_TIMESTAMP) {
		return cpu_accel_free_workload(dev);
	}
	if (config->workload == CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE) {
		if (config->shared_entry >= CPU_ACCEL_SHARED_ENTRY_COUNT ||
		    config->work_bytes < 64 ||
		    config->work_bytes > CPU_ACCEL_SHARED_ENTRY_BYTES / 2)
			return -EINVAL;
		if (cpu_accel_free_workload(dev))
			return -EBUSY;
		entry = &dev->shared_region->entries[config->shared_entry];
		dev->work_region.address = entry->data;
		dev->work_region.owner = CPU_ACCEL_REGION_LINUX;
		dev->work_region_allocated = false;
		dev->work_region_shared = true;
		return 0;
	}
	if (config->workload != CPU_ACCEL_WORKLOAD_MEMMOVE ||
	    config->work_bytes < 64 ||
	    config->work_bytes > CPU_ACCEL_MAX_WORK_BYTES)
		return -EINVAL;

	bytes = (size_t)config->work_bytes;
	size = bytes * 2;
	buffer = kmalloc(size, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	/* Touch both halves before the accelerator entry point runs. */
	memset(buffer, 0xa5, bytes);
	memset((char *)buffer + bytes, 0x5a, bytes);
	if (cpu_accel_free_workload(dev)) {
		kfree(buffer);
		return -EBUSY;
	}
	dev->work_region.address = buffer;
	dev->work_region.bytes = size;
	dev->work_region_allocated = true;
	dev->work_region_shared = false;
	return 0;
}

static bool cpu_accel_workqueue_target_active(struct cpu_accel_device *dev,
						int cpu)
{
	return cpu == READ_ONCE(dev->config.cpu) &&
		atomic_read(&dev->running);
}

static void cpu_accel_workqueue_queue(void *data, int req_cpu,
				      struct pool_workqueue *pwq,
				      struct work_struct *work)
{
	struct cpu_accel_device *dev = data;

	(void)pwq;
	(void)work;
	if (cpu_accel_workqueue_target_active(dev, req_cpu))
		atomic64_inc(&dev->workqueue_queued);
}

static void cpu_accel_workqueue_execute_start(void *data,
					       struct work_struct *work)
{
	struct cpu_accel_device *dev = data;

	(void)work;
	if (cpu_accel_workqueue_target_active(dev, raw_smp_processor_id()))
		atomic64_inc(&dev->workqueue_executed);
}

static int cpu_accel_register_workqueue_tracepoints(void)
{
	int ret;

	ret = register_trace_workqueue_queue_work(cpu_accel_workqueue_queue,
						  &cpu_accel);
	if (ret)
		return ret;

	ret = register_trace_workqueue_execute_start(
		cpu_accel_workqueue_execute_start, &cpu_accel);
	if (ret)
		unregister_trace_workqueue_queue_work(cpu_accel_workqueue_queue,
						      &cpu_accel);
	return ret;
}

static void cpu_accel_unregister_workqueue_tracepoints(void)
{
	unregister_trace_workqueue_execute_start(
		cpu_accel_workqueue_execute_start, &cpu_accel);
	unregister_trace_workqueue_queue_work(cpu_accel_workqueue_queue,
					      &cpu_accel);
	tracepoint_synchronize_unregister();
}

static void cpu_accel_watchdog(struct timer_list *timer)
{
	struct cpu_accel_device *dev =
		container_of(timer, struct cpu_accel_device, watchdog_timer);

	if (!atomic_read(&dev->enter_requested))
		return;

	atomic_set(&dev->watchdog_fired, 1);
	atomic_set(&dev->stop_requested, 1);
	WRITE_ONCE(dev->shared->stop_requested, 1);
}

static void cpu_accel_reset_shared(struct cpu_accel_device *dev)
{
	memset(dev->shared, 0, CPU_ACCEL_MAP_SIZE);
	dev->shared->abi_version = CPU_ACCEL_ABI_VERSION;
	dev->shared->struct_size = sizeof(*dev->shared);
	dev->shared->backend = cpu_accel_backend_id();
	dev->shared->mode = CPU_ACCEL_MODE_LINUX;
	dev->shared->workload = CPU_ACCEL_WORKLOAD_TIMESTAMP;
	dev->shared->shared_owner = CPU_ACCEL_REGION_LINUX;
	WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_IDLE);
}

static void cpu_accel_observation_begin(struct cpu_accel_observation *obs)
{
	obs->cpu = smp_processor_id();
	obs->lifecycle_entry_ns = ktime_get_mono_fast_ns();
	obs->irq_entry = kstat_cpu_irqs_sum(obs->cpu);
	obs->softirq_entry = kstat_cpu_softirqs_sum(obs->cpu);
	obs->timer_softirq_entry = kstat_softirqs_cpu(TIMER_SOFTIRQ, obs->cpu);
	obs->hrtimer_softirq_entry = kstat_softirqs_cpu(HRTIMER_SOFTIRQ, obs->cpu);
	obs->rcu_softirq_entry = kstat_softirqs_cpu(RCU_SOFTIRQ, obs->cpu);
	obs->sched_softirq_entry = kstat_softirqs_cpu(SCHED_SOFTIRQ, obs->cpu);
	obs->context_switches_entry = READ_ONCE(current->nvcsw) +
		READ_ONCE(current->nivcsw);
	obs->arch_irq_entry = cpu_accel_arch_irq_count(obs->cpu);
	obs->arch_ipi_entry = cpu_accel_arch_ipi_count(obs->cpu);
	obs->arch_tlb_entry = cpu_accel_arch_tlb_count(obs->cpu);
	obs->need_resched_entry = need_resched();
	obs->softirq_pending_entry = local_softirq_pending();
	obs->preempt_count_entry = preempt_count();
	obs->need_resched_samples = 0;
}

static void cpu_accel_observation_finish(struct cpu_accel_device *dev,
					 struct cpu_accel_observation *obs)
{
	struct cpu_accel_shared *shared = dev->shared;
	u64 now = ktime_get_mono_fast_ns();
	u64 irq_count = kstat_cpu_irqs_sum(obs->cpu);
	u64 softirq_count = kstat_cpu_softirqs_sum(obs->cpu);
	u64 timer_softirq_count = kstat_softirqs_cpu(TIMER_SOFTIRQ,
		obs->cpu);
	u64 hrtimer_softirq_count = kstat_softirqs_cpu(HRTIMER_SOFTIRQ,
		obs->cpu);
	u64 rcu_softirq_count = kstat_softirqs_cpu(RCU_SOFTIRQ, obs->cpu);
	u64 sched_softirq_count = kstat_softirqs_cpu(SCHED_SOFTIRQ,
		obs->cpu);
	u64 context_switches = READ_ONCE(current->nvcsw) +
		READ_ONCE(current->nivcsw);
	u64 arch_irq_count = cpu_accel_arch_irq_count(obs->cpu);
	u64 arch_ipi_count = cpu_accel_arch_ipi_count(obs->cpu);
	u64 arch_tlb_count = cpu_accel_arch_tlb_count(obs->cpu);
	u64 irq_delta = cpu_accel_counter_delta(irq_count, obs->irq_entry);
	u64 softirq_delta = cpu_accel_counter_delta(softirq_count,
		obs->softirq_entry);
	u64 context_switch_delta = cpu_accel_counter_delta(context_switches,
		obs->context_switches_entry);
	u64 arch_irq_delta = cpu_accel_counter_delta(arch_irq_count,
		obs->arch_irq_entry);
	u64 arch_ipi_delta = cpu_accel_counter_delta(arch_ipi_count,
		obs->arch_ipi_entry);
	u64 arch_tlb_delta = cpu_accel_counter_delta(arch_tlb_count,
		obs->arch_tlb_entry);
	u64 timer_softirq_delta = cpu_accel_counter_delta(timer_softirq_count,
		obs->timer_softirq_entry);
	u64 hrtimer_softirq_delta = cpu_accel_counter_delta(hrtimer_softirq_count,
		obs->hrtimer_softirq_entry);
	u64 rcu_softirq_delta = cpu_accel_counter_delta(rcu_softirq_count,
		obs->rcu_softirq_entry);
	u64 sched_softirq_delta = cpu_accel_counter_delta(sched_softirq_count,
		obs->sched_softirq_entry);
	u32 exit_cpu = smp_processor_id();

	shared->lifecycle_entry_ns = obs->lifecycle_entry_ns;
	shared->lifecycle_exit_ns = now;
	shared->irq_count = irq_delta;
	shared->irq_quarantined = dev->irq_quarantined;
	shared->irq_quarantine_blockers = dev->irq_quarantine_blockers;
	shared->softirq_count = softirq_delta;
	shared->timer_softirq_count = timer_softirq_delta;
	shared->hrtimer_softirq_count = hrtimer_softirq_delta;
	shared->rcu_softirq_count = rcu_softirq_delta;
	shared->sched_softirq_count = sched_softirq_delta;
	shared->workqueue_queued = atomic64_read(&dev->workqueue_queued);
	shared->workqueue_executed = atomic64_read(&dev->workqueue_executed);
	shared->context_switches = context_switch_delta;
	shared->need_resched_samples = obs->need_resched_samples;
	shared->arch_irq_count = arch_irq_delta;
	shared->arch_ipi_count = arch_ipi_delta;
	shared->arch_tlb_count = arch_tlb_delta;
	shared->need_resched_entry = obs->need_resched_entry;
	shared->need_resched_exit = need_resched();
	shared->softirq_pending_entry = obs->softirq_pending_entry;
	shared->softirq_pending_exit = local_softirq_pending();
	shared->preempt_count_entry = obs->preempt_count_entry;
	shared->preempt_count_exit = preempt_count();
	shared->arch_counters_valid = cpu_accel_arch_counters_valid();
	shared->lifecycle_cpu_entry = obs->cpu;
	shared->lifecycle_cpu_exit = exit_cpu;
	shared->migration_detected = obs->cpu != exit_cpu;
}

/*
 * This is the first architecture-neutral accelerator workload.  The
 * lifecycle backend dispatches this function synchronously to the target CPU.
 * The target stays online, but does not return to the scheduler while this
 * function is running.
 */
static void cpu_accel_run(struct cpu_accel_device *dev,
			  struct cpu_accel_observation *obs)
{
	struct cpu_accel_shared *shared = dev->shared;
	u64 start = 0, deadline = 0, now, lateness;
	u64 samples = 0;
	u64 max_lateness = 0;
	u64 min_lateness = U64_MAX;
	u64 work_iterations = 0;
	u64 work_epoch = READ_ONCE(dev->work_region.epoch);
	u32 samples_valid = 0;
	u32 final_state = CPU_ACCEL_STATE_COMPLETE;
	bool persistent = dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT;

	if (atomic_read(&dev->stop_requested)) {
		final_state = atomic_read(&dev->watchdog_fired) ?
			CPU_ACCEL_STATE_WATCHDOG : CPU_ACCEL_STATE_STOPPED;
		goto finish;
	}

	atomic_set(&dev->running, 1);
	WRITE_ONCE(shared->state, CPU_ACCEL_STATE_RUNNING);
	start = ktime_get_mono_fast_ns();
	shared->start_ns = start;
	deadline = start + dev->config.period_ns;

	for (;;) {
		now = ktime_get_mono_fast_ns();
		if (now < deadline) {
			cpu_relax();
			continue;
		}
		if (need_resched())
			obs->need_resched_samples++;

		lateness = now - deadline;
		if (samples_valid < CPU_ACCEL_MAX_SAMPLES) {
			shared->samples[samples_valid].timestamp_ns = now;
			shared->samples[samples_valid].lateness_ns = lateness;
			samples_valid++;
		}
		samples++;
		max_lateness = max(max_lateness, lateness);
		min_lateness = min(min_lateness, lateness);
		if (dev->work_region.address) {
			char *buffer = dev->work_region.address;
			size_t bytes = dev->work_region.bytes / 2;

			if (READ_ONCE(dev->work_region.owner) !=
				CPU_ACCEL_REGION_ACCELERATOR ||
			    READ_ONCE(dev->work_region.epoch) != work_epoch ||
			    bytes != (size_t)dev->config.work_bytes) {
				final_state = CPU_ACCEL_STATE_ERROR;
				break;
			}

			if (work_iterations & 1)
				memcpy(buffer + bytes, buffer, bytes);
			else
				memcpy(buffer, buffer + bytes, bytes);
			work_iterations++;
		}

		if (atomic_read(&dev->watchdog_fired)) {
			final_state = CPU_ACCEL_STATE_WATCHDOG;
			break;
		}
		if (atomic_read(&dev->stop_requested)) {
			final_state = CPU_ACCEL_STATE_STOPPED;
			break;
		}
		if (now - start >= dev->config.duration_ns) {
			final_state = persistent ? CPU_ACCEL_STATE_WATCHDOG :
				CPU_ACCEL_STATE_COMPLETE;
			break;
		}
		if (deadline > U64_MAX - dev->config.period_ns) {
			final_state = CPU_ACCEL_STATE_ERROR;
			break;
		}
		deadline += dev->config.period_ns;
	}

finish:
	now = ktime_get_mono_fast_ns();
	shared->end_ns = now;
	shared->samples_produced = samples;
	shared->samples_valid = samples_valid;
	shared->work_iterations = work_iterations;
	shared->max_lateness_ns = max_lateness;
	shared->min_lateness_ns = samples ? min_lateness : 0;
	shared->last_lateness_ns = samples_valid ?
		shared->samples[samples_valid - 1].lateness_ns : 0;
	shared->stop_requested = 0;
	cpu_accel_region_end(dev, final_state);
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->running, 0);
	cpu_accel_observation_finish(dev, obs);
	/* Publish all result fields before publishing the terminal state. */
	smp_wmb();
	WRITE_ONCE(shared->state, final_state);
}

static void cpu_accel_lifecycle_entry(void *data)
{
	struct cpu_accel_device *dev = data;
	struct cpu_accel_observation obs;
	unsigned long irq_flags;
	bool quiescent;

	preempt_disable();
	local_irq_save(irq_flags);
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_ACCELERATOR);
	cpu_accel_observation_begin(&obs);
	quiescent = !obs.need_resched_entry &&
		!obs.softirq_pending_entry;
	if ((dev->config.flags & CPU_ACCEL_FLAG_REQUIRE_QUIESCENT) &&
	    !quiescent) {
		cpu_accel_observation_finish(dev, &obs);
		cpu_accel_region_end(dev, CPU_ACCEL_STATE_ERROR);
		smp_wmb();
		WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_ERROR);
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		local_irq_restore(irq_flags);
		preempt_enable();
		return;
	}
	cpu_accel_run(dev, &obs);
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_EXITING);
	local_irq_restore(irq_flags);
	preempt_enable();
}

/*
 * Keep this dispatch in one place. Future architecture backends can replace
 * the staged SMP call with a direct accelerator entry/exit mechanism without
 * changing the control plane or workload implementation.
 */
static int cpu_accel_lifecycle_enter(struct cpu_accel_device *dev)
{
	return cpu_accel_arch_enter(dev->config.cpu,
				   cpu_accel_lifecycle_entry, dev);
}

static int cpu_accel_lifecycle_thread(void *data)
{
	struct cpu_accel_device *dev = data;
	int irq_ret;
	int ret;

	/* Keep the target online while the staged entry owns its execution. */
	cpus_read_lock();
	ret = cpu_accel_lifecycle_enter(dev);
	irq_ret = cpu_accel_restore_irq_quarantine(dev);
	if (!ret && irq_ret)
		ret = irq_ret;
	if (!irq_ret)
		cpu_accel_release_workqueue(dev);
	cpus_read_unlock();
	dev->lifecycle_ret = ret;
	atomic_set(&dev->enter_requested, 0);
	if (ret) {
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING)
			WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_ERROR);
	} else if (READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_ERROR) {
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_LINUX);
	}
	complete(&dev->lifecycle_done);
	module_put(THIS_MODULE);
	return 0;
}

static int cpu_accel_create_lifecycle_thread(struct cpu_accel_device *dev)
{
	if (dev->lifecycle_thread)
		return -EBUSY;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	dev->lifecycle_thread = kthread_create(cpu_accel_lifecycle_thread, dev,
					       "cpu_accel_ctl");
	if (IS_ERR(dev->lifecycle_thread)) {
		int ret = PTR_ERR(dev->lifecycle_thread);

		dev->lifecycle_thread = NULL;
		module_put(THIS_MODULE);
		return ret;
	}

	kthread_bind(dev->lifecycle_thread, dev->controller_cpu);
	wake_up_process(dev->lifecycle_thread);
	return 0;
}

static void cpu_accel_destroy_lifecycle_thread(struct cpu_accel_device *dev)
{
	if (!dev->lifecycle_thread)
		return;

	kthread_stop(dev->lifecycle_thread);
	dev->lifecycle_thread = NULL;
}

static int cpu_accel_lifecycle_exit(struct cpu_accel_device *dev)
{
	if (dev->lifecycle_thread) {
		if (!wait_for_completion_timeout(&dev->lifecycle_done,
						 msecs_to_jiffies(6000)))
			return -ETIMEDOUT;

		timer_delete_sync(&dev->watchdog_timer);
		cpu_accel_destroy_lifecycle_thread(dev);
	}
	if (!cpu_accel_shared_workload(dev))
		cpu_accel_region_recover(dev);
	return dev->lifecycle_ret;
}

static int cpu_accel_open(struct inode *inode, struct file *file)
{
	if (atomic_cmpxchg(&cpu_accel.opened, 0, 1))
		return -EBUSY;

	file->private_data = &cpu_accel;
	return 0;
}

static int cpu_accel_release(struct inode *inode, struct file *file)
{
	struct cpu_accel_device *dev = file->private_data;
	int ret;

	mutex_lock(&dev->lock);
	if (!dev->lifecycle_thread) {
		ret = cpu_accel_restore_irq_quarantine(dev);
		if (ret)
			pr_err("unable to restore accelerator IRQ quarantine for CPU %u: %d\n",
			       dev->config.cpu, ret);
		else
			cpu_accel_release_workqueue(dev);
	}
	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
	}
	if (dev->lifecycle_thread) {
		ret = cpu_accel_lifecycle_exit(dev);
		if (ret)
			pr_err("unable to exit accelerator CPU %u after release: %d\n",
			       dev->config.cpu, ret);
	}
	mutex_unlock(&dev->lock);

	atomic_set(&dev->opened, 0);
	return 0;
}

static int cpu_accel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cpu_accel_device *dev = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;

	if (!vma->vm_pgoff && size == CPU_ACCEL_MAP_SIZE)
		return remap_vmalloc_range(vma, dev->shared, 0);
	if (vma->vm_pgoff == (CPU_ACCEL_SHARED_MAP_OFFSET >> PAGE_SHIFT) &&
	    size == CPU_ACCEL_SHARED_MAP_SIZE)
		return remap_vmalloc_range(vma, dev->shared_region, 0);

	return -EINVAL;
}

static int cpu_accel_start_locked(struct cpu_accel_device *dev)
{
	unsigned int irq_quarantined = 0;
	unsigned int irq_quarantine_blockers = 0;
	int current_cpu;
	int ret;

	if (!dev->configured || dev->lifecycle_thread)
		return -EINVAL;
	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING)
		return -EBUSY;
	ret = cpu_accel_restore_irq_quarantine(dev);
	if (ret)
		return ret;
	cpu_accel_release_workqueue(dev);

	current_cpu = get_cpu();
	if (current_cpu == dev->config.cpu) {
		put_cpu();
		return -EBUSY;
	}
	dev->controller_cpu = current_cpu;
	put_cpu();

	cpus_read_lock();
	ret = cpu_online(dev->config.cpu) ? 0 : -ENODEV;
	if (!ret) {
		ret = workqueue_accel_cpu_reserve(dev->config.cpu);
		dev->workqueue_reserved = !ret;
	}
	if (!ret && dev->config.flags & CPU_ACCEL_FLAG_IRQ_QUARANTINE)
		ret = irq_accel_quarantine_cpu(dev->config.cpu,
					       &dev->irq_quarantine,
					       &irq_quarantined,
					       &irq_quarantine_blockers);
	cpus_read_unlock();
	if (ret) {
		if (irq_quarantine_blockers)
			dev->irq_quarantine_blockers = irq_quarantine_blockers;
		if (dev->irq_quarantine) {
			int irq_ret;

			irq_ret = cpu_accel_restore_irq_quarantine(dev);
			if (irq_ret)
				pr_err("cpu_accel: IRQ quarantine cleanup failed for CPU %u: %d\n",
				       dev->config.cpu, irq_ret);
		}
		if (!dev->irq_quarantine)
			cpu_accel_release_workqueue(dev);
		dev->shared->irq_quarantined = 0;
		dev->shared->irq_quarantine_blockers =
			irq_quarantine_blockers;
		return ret;
	}
	dev->irq_quarantined = irq_quarantined;
	dev->irq_quarantine_blockers = irq_quarantine_blockers;
	ret = cpu_accel_region_begin(dev);
	if (ret) {
		int irq_ret = cpu_accel_restore_irq_quarantine(dev);

		if (irq_ret)
			pr_err("cpu_accel: region admission cleanup failed for CPU %u: %d\n",
			       dev->config.cpu, irq_ret);
		cpu_accel_release_workqueue(dev);
		return ret;
	}

	dev->sequence++;
	dev->shared->sequence = dev->sequence;
	dev->shared->samples_produced = 0;
	dev->shared->samples_valid = 0;
	dev->shared->work_iterations = 0;
	dev->shared->stop_requested = 0;
	dev->shared->start_ns = 0;
	dev->shared->end_ns = 0;
	dev->shared->max_lateness_ns = 0;
	dev->shared->min_lateness_ns = 0;
	dev->shared->last_lateness_ns = 0;
	dev->shared->lifecycle_entry_ns = 0;
	dev->shared->lifecycle_exit_ns = 0;
	dev->shared->irq_count = 0;
	dev->shared->irq_quarantined = irq_quarantined;
	dev->shared->irq_quarantine_blockers = irq_quarantine_blockers;
	dev->shared->softirq_count = 0;
	dev->shared->timer_softirq_count = 0;
	dev->shared->hrtimer_softirq_count = 0;
	dev->shared->rcu_softirq_count = 0;
	dev->shared->sched_softirq_count = 0;
	dev->shared->workqueue_queued = 0;
	dev->shared->workqueue_executed = 0;
	dev->shared->context_switches = 0;
	dev->shared->need_resched_samples = 0;
	dev->shared->arch_irq_count = 0;
	dev->shared->arch_ipi_count = 0;
	dev->shared->arch_tlb_count = 0;
	dev->shared->need_resched_entry = 0;
	dev->shared->need_resched_exit = 0;
	dev->shared->softirq_pending_entry = 0;
	dev->shared->softirq_pending_exit = 0;
	dev->shared->preempt_count_entry = 0;
	dev->shared->preempt_count_exit = 0;
	dev->shared->arch_counters_valid = 0;
	dev->shared->lifecycle_cpu_entry = 0;
	dev->shared->lifecycle_cpu_exit = 0;
	dev->shared->migration_detected = 0;
	atomic64_set(&dev->workqueue_queued, 0);
	atomic64_set(&dev->workqueue_executed, 0);
	reinit_completion(&dev->lifecycle_done);
	dev->lifecycle_ret = -EINPROGRESS;
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->enter_requested, 1);
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_ENTERING);
	WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_READY);

	ret = cpu_accel_create_lifecycle_thread(dev);
	if (ret) {
		if (dev->irq_quarantine) {
			int irq_ret;

			irq_ret = cpu_accel_restore_irq_quarantine(dev);
			if (irq_ret)
				pr_err("cpu_accel: IRQ quarantine cleanup failed for CPU %u: %d\n",
				       dev->config.cpu, irq_ret);
		}
		if (!dev->irq_quarantine)
			cpu_accel_release_workqueue(dev);
		cpu_accel_region_recover(dev);
		atomic_set(&dev->enter_requested, 0);
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_ERROR);
	} else if (dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT) {
		unsigned long watchdog_jiffies;

		watchdog_jiffies = max_t(unsigned long, 1,
					 nsecs_to_jiffies(dev->config.duration_ns));
		mod_timer(&dev->watchdog_timer, jiffies + watchdog_jiffies);
	}
	return ret;
}

static long cpu_accel_ioctl(struct file *file, unsigned int command,
			    unsigned long argument)
{
	struct cpu_accel_device *dev = file->private_data;
	struct cpu_accel_config config;
	struct cpu_accel_shared_handoff handoff;
	u32 shared_entry;
	int ret = 0;

	if (_IOC_TYPE(command) != CPU_ACCEL_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&dev->lock);
	switch (command) {
	case CPU_ACCEL_IOC_CONFIG:
		if (copy_from_user(&config, (void __user *)argument,
				   sizeof(config))) {
			ret = -EFAULT;
			break;
		}
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread) {
			ret = -EBUSY;
			break;
		}
		if (!config.flags)
			config.flags = CPU_ACCEL_FLAG_IRQS_OFF;
		if (!(config.flags & CPU_ACCEL_FLAG_IRQS_OFF) ||
		    config.flags & ~(CPU_ACCEL_FLAG_IRQS_OFF |
				     CPU_ACCEL_FLAG_PERSISTENT |
				     CPU_ACCEL_FLAG_REQUIRE_QUIESCENT |
				     CPU_ACCEL_FLAG_IRQ_QUARANTINE) ||
		    config.reserved || config.reserved2 ||
		    config.workload > CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE ||
		    config.work_bytes > CPU_ACCEL_MAX_WORK_BYTES ||
		    (config.workload != CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE &&
		     config.shared_entry) ||
		    (config.workload == CPU_ACCEL_WORKLOAD_TIMESTAMP &&
		     config.work_bytes) ||
		    (config.workload == CPU_ACCEL_WORKLOAD_MEMMOVE &&
		     config.work_bytes < 64) ||
		    (config.workload == CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE &&
		     (config.shared_entry >= CPU_ACCEL_SHARED_ENTRY_COUNT ||
		      config.work_bytes < 64 ||
		      config.work_bytes > CPU_ACCEL_SHARED_ENTRY_BYTES / 2)) ||
		    !config.period_ns || !config.duration_ns ||
		    config.duration_ns > CPU_ACCEL_MAX_DURATION_NS ||
		    config.cpu >= nr_cpu_ids || config.cpu == 0) {
			ret = -EINVAL;
			break;
		}

		cpus_read_lock();
		if (!cpu_online(config.cpu))
			ret = -ENODEV;
		cpus_read_unlock();
		if (ret)
			break;

		ret = cpu_accel_prepare_workload(dev, &config);
		if (ret)
			break;

		dev->config = config;
		dev->configured = true;
		cpu_accel_reset_shared(dev);
		dev->shared->cpu = config.cpu;
		dev->shared->flags = config.flags;
		dev->shared->workload = config.workload;
		dev->shared->shared_entry = config.shared_entry;
		dev->shared->shared_owner = dev->work_region.owner;
		dev->shared->shared_epoch = dev->work_region.epoch;
		dev->shared->work_bytes = config.work_bytes;
		dev->shared->duration_ns = config.duration_ns;
		dev->shared->period_ns = config.period_ns;
		break;

	case CPU_ACCEL_IOC_START:
		ret = cpu_accel_start_locked(dev);
		break;

	case CPU_ACCEL_IOC_STOP:
		if (READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_RUNNING &&
		    READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_READY) {
			ret = -EALREADY;
			break;
		}
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_EXITING);
		break;

	case CPU_ACCEL_IOC_EXIT:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY) {
			ret = -EBUSY;
			break;
		}
		ret = cpu_accel_lifecycle_exit(dev);
		break;

	case CPU_ACCEL_IOC_SHARED_READY:
		if (copy_from_user(&handoff, (void __user *)argument,
				   sizeof(handoff))) {
			ret = -EFAULT;
			break;
		}
		ret = cpu_accel_shared_ready(dev, &handoff);
		break;

	case CPU_ACCEL_IOC_SHARED_RECLAIM:
		if (copy_from_user(&shared_entry, (void __user *)argument,
				   sizeof(shared_entry))) {
			ret = -EFAULT;
			break;
		}
		ret = cpu_accel_shared_reclaim(dev, shared_entry, false);
		break;

	case CPU_ACCEL_IOC_RESET:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread) {
			ret = -EBUSY;
			break;
		}
		ret = cpu_accel_free_workload(dev);
		if (ret)
			break;
		dev->configured = false;
		dev->sequence = 0;
		cpu_accel_reset_shared(dev);
		break;

	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&dev->lock);
	return ret;
}

static const struct file_operations cpu_accel_fops = {
	.owner		= THIS_MODULE,
	.open		= cpu_accel_open,
	.release	= cpu_accel_release,
	.mmap		= cpu_accel_mmap,
	.unlocked_ioctl	= cpu_accel_ioctl,
};

static int __init cpu_accel_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct cpu_accel_shared) > CPU_ACCEL_MAP_SIZE);
	BUILD_BUG_ON(sizeof(struct cpu_accel_shared_region) >
		     CPU_ACCEL_SHARED_MAP_SIZE);

	mutex_init(&cpu_accel.lock);
	init_completion(&cpu_accel.lifecycle_done);
	atomic_set(&cpu_accel.opened, 0);
	atomic_set(&cpu_accel.enter_requested, 0);
	atomic_set(&cpu_accel.stop_requested, 0);
	atomic_set(&cpu_accel.watchdog_fired, 0);
	atomic_set(&cpu_accel.running, 0);
	atomic64_set(&cpu_accel.workqueue_queued, 0);
	atomic64_set(&cpu_accel.workqueue_executed, 0);
	timer_setup(&cpu_accel.watchdog_timer, cpu_accel_watchdog, 0);
	cpu_accel.shared = vmalloc_user(CPU_ACCEL_MAP_SIZE);
	if (!cpu_accel.shared)
		return -ENOMEM;
	cpu_accel.shared_region = vmalloc_user(CPU_ACCEL_SHARED_MAP_SIZE);
	if (!cpu_accel.shared_region) {
		vfree(cpu_accel.shared);
		return -ENOMEM;
	}
	memset(cpu_accel.shared_region, 0, CPU_ACCEL_SHARED_MAP_SIZE);
	cpu_accel.shared_region->abi_version = CPU_ACCEL_ABI_VERSION;
	cpu_accel.shared_region->struct_size =
		sizeof(*cpu_accel.shared_region);
	cpu_accel.shared_region->entry_count = CPU_ACCEL_SHARED_ENTRY_COUNT;
	cpu_accel.shared_region->entry_size = sizeof(struct cpu_accel_shared_entry);
	cpu_accel_reset_shared(&cpu_accel);
	ret = cpu_accel_register_workqueue_tracepoints();
	if (ret) {
		vfree(cpu_accel.shared_region);
		vfree(cpu_accel.shared);
		return ret;
	}

	cpu_accel.misc.minor = MISC_DYNAMIC_MINOR;
	cpu_accel.misc.name = "cpu_accel";
	cpu_accel.misc.fops = &cpu_accel_fops;
	cpu_accel.misc.mode = 0600;
	ret = misc_register(&cpu_accel.misc);
	if (ret) {
		cpu_accel_unregister_workqueue_tracepoints();
		vfree(cpu_accel.shared_region);
		vfree(cpu_accel.shared);
		return ret;
	}

	pr_info("single-CPU accelerator lifecycle prototype loaded\n");
	return 0;
}

static void __exit cpu_accel_exit(void)
{
	mutex_lock(&cpu_accel.lock);
	if (READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&cpu_accel.stop_requested, 1);
		WRITE_ONCE(cpu_accel.shared->stop_requested, 1);
	}
	if (cpu_accel.lifecycle_thread)
		cpu_accel_lifecycle_exit(&cpu_accel);
	timer_delete_sync(&cpu_accel.watchdog_timer);
	if (cpu_accel.work_region_shared)
		cpu_accel_shared_reclaim(&cpu_accel,
					cpu_accel.config.shared_entry, true);
	cpu_accel_free_workload(&cpu_accel);
	mutex_unlock(&cpu_accel.lock);

	misc_deregister(&cpu_accel.misc);
	cpu_accel_unregister_workqueue_tracepoints();
	vfree(cpu_accel.shared_region);
	cpu_accel.shared_region = NULL;
	vfree(cpu_accel.shared);
	cpu_accel.shared = NULL;
}

module_init(cpu_accel_init);
module_exit(cpu_accel_exit);

MODULE_DESCRIPTION("Single-CPU accelerator lifecycle foundation prototype");
MODULE_LICENSE("GPL");
