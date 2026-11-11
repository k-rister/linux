// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel_stat.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/processor.h>
#include <linux/ptrace.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <trace/events/sched.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <uapi/linux/cpu_accel.h>

#ifdef CONFIG_X86
#include <asm/apic.h>
#include <asm/cpu_accel.h>
#include <asm/hardirq.h>
#include <asm/irq_vectors.h>
#include <asm/nmi.h>
#include <asm/processor-flags.h>
#include <asm/ptrace.h>
#include <asm/segment.h>
#include <asm/vsyscall.h>
#include <xen/xen.h>
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
	u64 arch_tlb_shootdown_targets_entry;
	u64 arch_reschedule_deferred_entry;
	u64 arch_call_function_deferred_entry;
	u64 arch_nmi_entry;
	u64 arch_mce_exception_entry;
	u64 arch_mce_poll_entry;
	u64 need_resched_samples;
	u32 need_resched_entry;
	u32 softirq_pending_entry;
	u32 preempt_count_entry;
	u32 cpu;
};

typedef void (*cpu_accel_entry_fn)(void *data);

#ifdef CONFIG_X86_LOCAL_APIC
static void cpu_accel_arch_stop_direct(void *data, unsigned int stop_flags);
#endif
#ifdef CONFIG_X86
static void cpu_accel_owner_death(void *data, struct task_struct *task,
				  bool group_dead);
static void cpu_accel_owner_exit_early(void *data, struct task_struct *task);
#endif

static bool cpu_accel_user_workload(u32 workload)
{
	return workload == CPU_ACCEL_WORKLOAD_USER_REGION ||
		workload == CPU_ACCEL_WORKLOAD_USER_CYCLICTEST ||
		workload == CPU_ACCEL_WORKLOAD_USER_HANG ||
		workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM ||
		workload == CPU_ACCEL_WORKLOAD_USER_DEATH ||
		workload == CPU_ACCEL_WORKLOAD_USER_OSLAT;
}

/*
 * The user-death workload is a test-only stand-in for a fatal #MC killing the
 * owner.  It runs in a normal, interruptible and preemptible context (rather
 * than the low-latency preempt/irq-disabled one) so that a SIGKILL can reach
 * it and its syscall/exit paths stay legal; only driver ownership is set up.
 */
static bool cpu_accel_user_death_safe(u32 workload)
{
	return workload == CPU_ACCEL_WORKLOAD_USER_DEATH;
}

#ifdef CONFIG_X86
static int cpu_accel_arch_enter(unsigned int cpu, cpu_accel_entry_fn entry,
				void *data)
{
#ifdef CONFIG_X86_LOCAL_APIC
	return x86_cpu_accel_direct_enter(cpu, entry,
					  cpu_accel_arch_stop_direct, data);
#else
	return smp_call_function_single(cpu, entry, data, 1);
#endif
}

static u32 cpu_accel_backend_id(void)
{
#ifdef CONFIG_X86_LOCAL_APIC
	return CPU_ACCEL_BACKEND_X86_DIRECT_APIC;
#else
	return CPU_ACCEL_BACKEND_X86_STAGED_IPI;
#endif
}

static u32 cpu_accel_backend_id_for_workload(u32 workload)
{
	if (cpu_accel_user_workload(workload))
		return CPU_ACCEL_BACKEND_X86_RING3;

#ifdef CONFIG_X86_LOCAL_APIC
	return CPU_ACCEL_BACKEND_X86_DIRECT_APIC;
#else
	return CPU_ACCEL_BACKEND_X86_STAGED_IPI;
#endif
}

static u32 cpu_accel_backend_flags_for_workload(u32 workload)
{
#ifdef CONFIG_X86_LOCAL_APIC
	u32 flags = CPU_ACCEL_BACKEND_FLAG_RESCHEDULE_DEFER |
		CPU_ACCEL_BACKEND_FLAG_CALL_FUNCTION_DEFER;

	if (cpu_accel_user_workload(workload))
		flags |= CPU_ACCEL_BACKEND_FLAG_USER_ESCAPE;
	return flags;
#else
	(void)workload;
#endif
	return 0;
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

static u32 cpu_accel_backend_id_for_workload(u32 workload)
{
	(void)workload;
	return CPU_ACCEL_BACKEND_GENERIC_SMP;
}

static u32 cpu_accel_backend_flags_for_workload(u32 workload)
{
	(void)workload;
	return 0;
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

static u64 cpu_accel_arch_nmi_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);

	return READ_ONCE(stats->counts[IRQ_COUNT_NMI]);
}

#ifdef CONFIG_X86_MCE
static u64 cpu_accel_arch_mce_exception_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);

	return READ_ONCE(stats->counts[IRQ_COUNT_MCE_EXCEPTION]);
}

static u64 cpu_accel_arch_mce_poll_count(unsigned int cpu)
{
	const irq_cpustat_t *stats = per_cpu_ptr(&irq_stat, cpu);

	return READ_ONCE(stats->counts[IRQ_COUNT_MCE_POLL]);
}
#else
static u64 cpu_accel_arch_mce_exception_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_mce_poll_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}
#endif

static u64 cpu_accel_arch_tlb_shootdown_targets(unsigned int cpu)
{
	return x86_cpu_accel_tlb_shootdown_targets(cpu);
}

static u64 cpu_accel_arch_reschedule_deferred(unsigned int cpu)
{
	return x86_cpu_accel_reschedule_deferred(cpu);
}

static u64 cpu_accel_arch_call_function_deferred(unsigned int cpu)
{
	return x86_cpu_accel_call_function_deferred(cpu);
}

static bool cpu_accel_arch_counters_valid(void)
{
#ifdef CONFIG_X86_MCE
	return true;
#else
	return false;
#endif
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

static u64 cpu_accel_arch_nmi_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_mce_exception_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_mce_poll_count(unsigned int cpu)
{
	(void)cpu;
	return U64_MAX;
}

static u64 cpu_accel_arch_tlb_shootdown_targets(unsigned int cpu)
{
	(void)cpu;
	return 0;
}

static u64 cpu_accel_arch_reschedule_deferred(unsigned int cpu)
{
	(void)cpu;
	return 0;
}

static u64 cpu_accel_arch_call_function_deferred(unsigned int cpu)
{
	(void)cpu;
	return 0;
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

#define CPU_ACCEL_MAX_REGIONS		64U

struct cpu_accel_region_vma {
	struct cpu_accel_region *region;
	bool manager;
};

struct cpu_accel_region {
	void *address;
	size_t bytes;
	u64 epoch;
	u64 token;
	struct cpu_accel_region_vma manager_vma;
	struct cpu_accel_region_vma owner_vma;
	u32 permissions;
	atomic_t manager_mappings;
	atomic_t owner_mappings;
	int numa_node;
	u8 state;
	bool attached;
};

struct cpu_accel_user_image {
	struct task_struct *task;
	struct mm_struct *mm;
	struct page **pages;
	unsigned int nr_pages;
	unsigned int pinned_pages;
	unsigned long entry_ip;
	unsigned long stack_top;
	unsigned long stack_bytes;
	unsigned long image_start;
	unsigned long image_bytes;
	unsigned long arg;
	unsigned long escape_ip;
#ifdef CONFIG_X86
	struct pt_regs return_regs;
#endif
	bool mm_locked;
	bool active;
	bool owner_active;
};

struct cpu_accel_device {
	struct miscdevice misc;
	/* Serializes control-plane state transitions. */
	struct mutex lock;
	/* Protects the region table independently of VMA mmap locks. */
	struct mutex region_lock;
	struct completion lifecycle_done;
	struct task_struct *lifecycle_thread;
	struct timer_list watchdog_timer;
	struct timer_list stop_escalation_timer;
	struct work_struct owner_death_work;
	struct irq_accel_quarantine *irq_quarantine;
	struct cpu_accel_shared *shared;
	wait_queue_head_t poll_wait;
	u32 selected_abi;
	/* mm of the process that opened the device as the control companion. */
	struct mm_struct *companion_mm;
	struct cpu_accel_config config;
	struct cpu_accel_region *regions[CPU_ACCEL_MAX_REGIONS];
	struct cpu_accel_region *attached_regions[CPU_ACCEL_MAX_REGIONS];
	atomic_t opened;
	atomic_t enter_requested;
	atomic_t stop_requested;
	atomic_t watchdog_fired;
	atomic_t user_escape_requested;
	atomic_t user_escape_seen;
	atomic_t running;
	atomic64_t workqueue_queued;
	atomic64_t workqueue_executed;
	atomic64_t user_escape_count;
	/* Set in the stop NMI when it completed the owner's exit. */
	atomic64_t user_stop_exit_tlb_targets;
	atomic_t user_stop_exit_done;
	struct cpu_accel_region work_region;
	unsigned int sequence;
	unsigned int controller_cpu;
	unsigned int irq_quarantined;
	unsigned int irq_quarantine_blockers;
	u64 region_total_bytes;
	u64 region_set_epoch;
	int lifecycle_ret;
	bool configured;
	bool workqueue_reserved;
	bool work_region_allocated;
	struct cpu_accel_user_image user_image;
	struct cpu_accel_observation user_observation;
};

static struct cpu_accel_device cpu_accel;
static atomic64_t cpu_accel_region_token_sequence = ATOMIC64_INIT(0);
static u64 cpu_accel_max_region_bytes = 1ULL << 30;
static u64 cpu_accel_max_total_region_bytes = 1ULL << 32;
static unsigned int cpu_accel_max_region_count = CPU_ACCEL_MAX_REGIONS;

module_param_named(max_region_bytes, cpu_accel_max_region_bytes, ullong, 0644);
MODULE_PARM_DESC(max_region_bytes, "Maximum bytes per cpu-accel region");
module_param_named(max_total_region_bytes, cpu_accel_max_total_region_bytes,
		   ullong, 0644);
MODULE_PARM_DESC(max_total_region_bytes,
		 "Maximum total cpu-accel region bytes");
module_param_named(max_region_count, cpu_accel_max_region_count, uint, 0644);
MODULE_PARM_DESC(max_region_count, "Maximum cpu-accel regions");

#ifdef CONFIG_X86_LOCAL_APIC
static void cpu_accel_arch_stop_direct(void *data, unsigned int stop_flags)
{
	struct cpu_accel_device *dev = data;

	/*
	 * Draining callers let the bounded kernel run complete on its own;
	 * only terminating requests publish the stop flag.
	 */
	if (stop_flags & X86_CPU_ACCEL_STOP_DRAIN)
		return;

	atomic_set(&dev->stop_requested, 1);
	WRITE_ONCE(dev->shared->stop_requested, 1);
}
#endif

#ifdef CONFIG_X86
static unsigned int cpu_accel_user_escape_drop_count;
module_param_named(user_escape_drop_count, cpu_accel_user_escape_drop_count,
			   uint, 0644);
MODULE_PARM_DESC(user_escape_drop_count,
		 "suppress the NMI of this many upcoming x86 user escape "
		 "attempts for fault-injection testing");
#endif

#ifdef CONFIG_X86
static int cpu_accel_arch_send_user_escape(unsigned int cpu)
{
#ifdef CONFIG_X86_LOCAL_APIC
	cpumask_t mask;

	cpumask_clear(&mask);
	cpumask_set_cpu(cpu, &mask);
	__apic_send_IPI_mask(&mask, NMI_VECTOR);
	return 0;
#else
	(void)cpu;
	return -EOPNOTSUPP;
#endif
}

/*
 * Consume a fault-injection drop on the sending side so that unrelated
 * NMIs reaching the target cannot eat a drop slot; the attempt then fails
 * its deadline like a genuinely lost NMI.
 */
static int cpu_accel_user_escape_send(unsigned int cpu)
{
	if (READ_ONCE(cpu_accel_user_escape_drop_count)) {
		unsigned int drops = READ_ONCE(cpu_accel_user_escape_drop_count);

		if (cmpxchg(&cpu_accel_user_escape_drop_count, drops,
			     drops - 1) == drops)
			return 0;
	}
	return cpu_accel_arch_send_user_escape(cpu);
}

#ifdef CONFIG_X86_LOCAL_APIC
/* Grace past the owner's own deadline before a drain escalates. */
#define CPU_ACCEL_STOP_ESCALATION_GRACE_NS	(100 * NSEC_PER_MSEC)
/* Owners without a bounded duration escalate after this window. */
#define CPU_ACCEL_STOP_MIN_DURATION_NS		(100 * NSEC_PER_MSEC)

static unsigned long cpu_accel_stop_escalation_when(struct cpu_accel_device *dev)
{
	u64 now = ktime_get_mono_fast_ns();
	u64 duration = max_t(u64, dev->config.duration_ns,
			     CPU_ACCEL_STOP_MIN_DURATION_NS);
	u64 deadline_ns = now + duration + CPU_ACCEL_STOP_ESCALATION_GRACE_NS;

	if (READ_ONCE(dev->shared->start_ns))
		deadline_ns = READ_ONCE(dev->shared->start_ns) +
			duration + CPU_ACCEL_STOP_ESCALATION_GRACE_NS;
	if (deadline_ns <= now)
		return jiffies;
	return jiffies + nsecs_to_jiffies(deadline_ns - now);
}

static void cpu_accel_stop_escalation(struct timer_list *timer)
{
	struct cpu_accel_device *dev =
		container_of(timer, struct cpu_accel_device,
			     stop_escalation_timer);
	unsigned int cpu;

	if (!READ_ONCE(dev->user_image.active))
		return;

	cpu = READ_ONCE(dev->config.cpu);
	/*
	 * The owner did not exit on its own within the drain window;
	 * escalate to the owner-terminating escape NMI.
	 */
	atomic_set(&dev->user_escape_requested, 1);
	smp_wmb();
	cpu_accel_arch_send_user_escape(cpu);
}

static void cpu_accel_arch_stop_user(void *data, unsigned int stop_flags)
{
	struct cpu_accel_device *dev = data;

	if (!READ_ONCE(dev->user_image.active))
		return;

	if (stop_flags & X86_CPU_ACCEL_STOP_DRAIN) {
		/*
		 * Hotplug maintenance: let the bounded owner complete and
		 * exit on its own; escalate only if it is still active past
		 * its own deadline. The callback itself must not wait.
		 */
		mod_timer(&dev->stop_escalation_timer,
			  cpu_accel_stop_escalation_when(dev));
		return;
	}

	atomic_set(&dev->stop_requested, 1);
	WRITE_ONCE(dev->shared->stop_requested, 1);
	atomic_set(&dev->user_escape_requested, 1);
	smp_wmb();
	cpu_accel_arch_send_user_escape(READ_ONCE(dev->config.cpu));
}
#endif

static int cpu_accel_user_nmi(unsigned int type, struct pt_regs *regs)
{
	struct cpu_accel_device *dev = &cpu_accel;
	struct cpu_accel_user_image *image = &dev->user_image;
	u64 targets;

	(void)type;

	/*
	 * Only the admitted owner task may be redirected to the escape
	 * code; a stale active flag (owner died without its exit path)
	 * must not retarget an unrelated user-mode task on the CPU.
	 * Compare the task, not the mm: sibling threads share an mm,
	 * and while active holds, image->task is a held reference.
	 */
	if (!atomic_read(&dev->user_escape_requested) ||
	    !READ_ONCE(image->active) ||
	    raw_smp_processor_id() != READ_ONCE(dev->config.cpu) ||
	    !user_mode(regs) ||
	    current != image->task)
		return NMI_DONE;

	/*
	 * Complete the owner exit before the escape tail re-enables
	 * interrupts; queued call-function work must observe a fully exited
	 * owner.
	 */
	targets = x86_cpu_accel_user_stop_exit(READ_ONCE(dev->config.cpu));
	atomic64_set(&dev->user_stop_exit_tlb_targets, targets);
	atomic_set(&dev->user_stop_exit_done, 1);

	/* The image and stack were admitted and pinned before this NMI. */
	regs->ip = image->escape_ip;
	regs->sp = image->stack_top;
	regs->di = image->arg;
	regs->flags = (regs->flags & ~X86_EFLAGS_TF) | X86_EFLAGS_FIXED;
	atomic64_inc(&dev->user_escape_count);
	atomic_set(&dev->user_escape_seen, 1);
	WRITE_ONCE(dev->shared->recovery_state,
		   CPU_ACCEL_RECOVERY_SUCCEEDED);
	WRITE_ONCE(dev->shared->stop_requested, 1);
	return NMI_HANDLED;
}
NOKPROBE_SYMBOL(cpu_accel_user_nmi);
#endif

static bool cpu_accel_state_terminal(u32 state)
{
	return state == CPU_ACCEL_STATE_COMPLETE ||
		state == CPU_ACCEL_STATE_STOPPED ||
		state == CPU_ACCEL_STATE_ERROR ||
		state == CPU_ACCEL_STATE_WATCHDOG ||
		state == CPU_ACCEL_STATE_ESCAPED;
}

static bool cpu_accel_user_terminal(struct cpu_accel_device *dev)
{
	return cpu_accel_state_terminal(READ_ONCE(dev->shared->state));
}

static void cpu_accel_state_write(struct cpu_accel_device *dev, u32 state)
{
	WRITE_ONCE(dev->shared->state, state);
	if (cpu_accel_state_terminal(state))
		wake_up_poll(&dev->poll_wait, EPOLLIN | EPOLLRDNORM);
}

static int cpu_accel_user_escape(struct cpu_accel_device *dev)
{
#ifdef CONFIG_X86
	unsigned long deadline;
	unsigned int cpu;
	int ret;

	mutex_lock(&dev->lock);
	if (!dev->user_image.active ||
	    !cpu_accel_user_workload(dev->config.workload) ||
	    !cpu_online(dev->config.cpu)) {
		mutex_unlock(&dev->lock);
		return -EINVAL;
	}
	if (!(READ_ONCE(dev->shared->backend_flags) &
	      CPU_ACCEL_BACKEND_FLAG_USER_ESCAPE)) {
		WRITE_ONCE(dev->shared->recovery_state,
			   CPU_ACCEL_RECOVERY_UNSUPPORTED);
		WRITE_ONCE(dev->shared->recovery_error, -EOPNOTSUPP);
		WRITE_ONCE(dev->shared->recovery_completed_ns,
			   ktime_get_mono_fast_ns());
		mutex_unlock(&dev->lock);
		return -EOPNOTSUPP;
	}
	cpu = dev->config.cpu;
	atomic_set(&dev->user_escape_requested, 1);
	atomic_set(&dev->user_escape_seen, 0);
	WRITE_ONCE(dev->shared->recovery_state,
		   CPU_ACCEL_RECOVERY_REQUESTED);
	WRITE_ONCE(dev->shared->recovery_error, 0);
	WRITE_ONCE(dev->shared->recovery_requested_ns,
		   ktime_get_mono_fast_ns());
	WRITE_ONCE(dev->shared->recovery_completed_ns, 0);
	WRITE_ONCE(dev->shared->recovery_attempts,
		   READ_ONCE(dev->shared->recovery_attempts) + 1);
	WRITE_ONCE(dev->shared->stop_requested, 1);
	mutex_unlock(&dev->lock);

	/* Publish the escape request before raising the target CPU's NMI. */
	smp_wmb();
	ret = cpu_accel_user_escape_send(cpu);
	if (ret) {
		mutex_lock(&dev->lock);
		atomic_set(&dev->user_escape_requested, 0);
		WRITE_ONCE(dev->shared->recovery_state,
			   ret == -EOPNOTSUPP ? CPU_ACCEL_RECOVERY_UNSUPPORTED :
			   CPU_ACCEL_RECOVERY_FAILED);
		WRITE_ONCE(dev->shared->recovery_error, ret);
		WRITE_ONCE(dev->shared->recovery_completed_ns,
			   ktime_get_mono_fast_ns());
		mutex_unlock(&dev->lock);
		return ret;
	}

	deadline = jiffies + HZ;
	while (time_before(jiffies, deadline)) {
		if (cpu_accel_user_terminal(dev))
			break;
		cpu_relax();
	}

	mutex_lock(&dev->lock);
	if (cpu_accel_user_terminal(dev)) {
		ret = atomic_read(&dev->user_escape_seen) ? 0 : -EALREADY;
		if (ret) {
			WRITE_ONCE(dev->shared->recovery_state,
				   CPU_ACCEL_RECOVERY_FAILED);
			WRITE_ONCE(dev->shared->recovery_error, ret);
			WRITE_ONCE(dev->shared->recovery_completed_ns,
				   ktime_get_mono_fast_ns());
		}
	} else {
		ret = -ETIMEDOUT;
		WRITE_ONCE(dev->shared->recovery_state,
			   CPU_ACCEL_RECOVERY_TIMEOUT);
		WRITE_ONCE(dev->shared->recovery_error, ret);
		WRITE_ONCE(dev->shared->recovery_completed_ns,
			   ktime_get_mono_fast_ns());
	}
	atomic_set(&dev->user_escape_requested, 0);
	mutex_unlock(&dev->lock);
	return ret;
#else
	(void)dev;
	return -EOPNOTSUPP;
#endif
}

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

static bool cpu_accel_region_permissions_valid(u32 permissions)
{
	return permissions == CPU_ACCEL_REGION_PERM_BIDIRECTIONAL ||
		permissions == CPU_ACCEL_REGION_PERM_MANAGER_TO_OWNER ||
		permissions == CPU_ACCEL_REGION_PERM_OWNER_TO_MANAGER;
}

static struct cpu_accel_region *
cpu_accel_region_find(struct cpu_accel_device *dev, u32 region_id)
{
	if (!region_id || region_id > CPU_ACCEL_MAX_REGIONS)
		return NULL;
	return dev->regions[region_id - 1];
}

static u64 cpu_accel_region_next_epoch(u64 epoch)
{
	return epoch == U64_MAX ? 1 : epoch + 1;
}

static void cpu_accel_region_vm_open(struct vm_area_struct *vma)
{
	struct cpu_accel_region_vma *region_vma = vma->vm_private_data;

	if (region_vma->manager)
		atomic_inc(&region_vma->region->manager_mappings);
	else
		atomic_inc(&region_vma->region->owner_mappings);
}

static void cpu_accel_region_vm_close(struct vm_area_struct *vma)
{
	struct cpu_accel_region_vma *region_vma = vma->vm_private_data;

	if (region_vma->manager)
		atomic_dec(&region_vma->region->manager_mappings);
	else
		atomic_dec(&region_vma->region->owner_mappings);
}

static int cpu_accel_region_vm_may_split(struct vm_area_struct *vma,
					 unsigned long start)
{
	(void)vma;
	(void)start;
	return -EINVAL;
}

static int cpu_accel_region_vm_mremap(struct vm_area_struct *vma)
{
	(void)vma;
	return -EINVAL;
}

static int cpu_accel_region_vm_mprotect(struct vm_area_struct *vma,
					unsigned long start, unsigned long end,
					unsigned long newflags)
{
	vm_flags_t permission_mask = VM_READ | VM_WRITE | VM_EXEC | VM_SHARED;

	(void)start;
	(void)end;
	if ((newflags & permission_mask) != (vma->vm_flags & permission_mask))
		return -EINVAL;
	return 0;
}

static const struct vm_operations_struct cpu_accel_region_vm_ops = {
	.open = cpu_accel_region_vm_open,
	.close = cpu_accel_region_vm_close,
	.may_split = cpu_accel_region_vm_may_split,
	.mremap = cpu_accel_region_vm_mremap,
	.mprotect = cpu_accel_region_vm_mprotect,
};

static void cpu_accel_control_vm_open(struct vm_area_struct *vma)
{
	(void)vma;
}

static void cpu_accel_control_vm_close(struct vm_area_struct *vma)
{
	(void)vma;
}

static int cpu_accel_control_vm_may_split(struct vm_area_struct *vma,
					  unsigned long start)
{
	(void)vma;
	(void)start;
	return -EINVAL;
}

static int cpu_accel_control_vm_mremap(struct vm_area_struct *vma)
{
	(void)vma;
	return -EINVAL;
}

static int cpu_accel_control_vm_mprotect(struct vm_area_struct *vma,
					 unsigned long start,
					 unsigned long end,
					 unsigned long newflags)
{
	vm_flags_t permission_mask = VM_READ | VM_WRITE | VM_EXEC | VM_SHARED;

	(void)start;
	(void)end;
	if ((newflags & permission_mask) != (vma->vm_flags & permission_mask))
		return -EINVAL;
	return 0;
}

static const struct vm_operations_struct cpu_accel_control_vm_ops = {
	.open = cpu_accel_control_vm_open,
	.close = cpu_accel_control_vm_close,
	.may_split = cpu_accel_control_vm_may_split,
	.mremap = cpu_accel_control_vm_mremap,
	.mprotect = cpu_accel_control_vm_mprotect,
};

static int cpu_accel_region_recover(struct cpu_accel_device *dev)
{
	if (!dev->work_region_allocated)
		return 0;
	if (dev->lifecycle_thread)
		return -EBUSY;
	kfree(dev->work_region.address);
	dev->work_region.address = NULL;
	dev->work_region.bytes = 0;
	dev->work_region_allocated = false;
	return 0;
}

static int cpu_accel_region_begin(struct cpu_accel_device *dev)
{
	if (!dev->work_region_allocated)
		return 0;
	dev->work_region.epoch =
		cpu_accel_region_next_epoch(dev->work_region.epoch);
	return 0;
}

static void cpu_accel_region_end(struct cpu_accel_device *dev, u32 state)
{
	if (!dev->work_region_allocated)
		return;
	(void)state;
	kfree(dev->work_region.address);
	dev->work_region.address = NULL;
	dev->work_region.bytes = 0;
	dev->work_region_allocated = false;
}

static bool cpu_accel_region_control_idle_locked(struct cpu_accel_device *dev)
{
	if (current->mm != dev->companion_mm)
		return false;
	return READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_READY &&
		READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_RUNNING &&
		!dev->lifecycle_thread && !dev->user_image.active && !dev->configured;
}

static void cpu_accel_region_finish(struct cpu_accel_device *dev, bool error)
{
	unsigned int index;

	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->attached_regions[index];

		if (!region)
			continue;
		region->state = error ? CPU_ACCEL_REGION_ERROR :
			CPU_ACCEL_REGION_COMPLETE;
	}
}

static void cpu_accel_region_detach_all(struct cpu_accel_device *dev)
{
	unsigned int index;

	mutex_lock(&dev->region_lock);
	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->regions[index];

		if (!region || !region->attached)
			continue;
		region->attached = false;
		region->state = CPU_ACCEL_REGION_CREATED;
		region->epoch = cpu_accel_region_next_epoch(region->epoch);
	}
	memset(dev->attached_regions, 0, sizeof(dev->attached_regions));
	mutex_unlock(&dev->region_lock);
}

static unsigned int cpu_accel_region_snapshot(struct cpu_accel_device *dev)
{
	unsigned int count = 0;
	unsigned int index;

	memset(dev->attached_regions, 0, sizeof(dev->attached_regions));
	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->regions[index];

		if (!region || !region->attached)
			continue;
		if (region->state != CPU_ACCEL_REGION_ATTACHED)
			goto fail;
		if (atomic_read(&region->manager_mappings) != 1 ||
		    atomic_read(&region->owner_mappings) != 1)
			goto fail;
		dev->attached_regions[count++] = region;
	}
	return count;

fail:
	memset(dev->attached_regions, 0, sizeof(dev->attached_regions));
	return U32_MAX;
}

static void cpu_accel_region_clear_snapshot(struct cpu_accel_device *dev,
					    bool active)
{
	unsigned int index;

	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->attached_regions[index];

		if (region && active)
			region->state = CPU_ACCEL_REGION_ATTACHED;
	}
	memset(dev->attached_regions, 0, sizeof(dev->attached_regions));
}

static void cpu_accel_region_activate_snapshot(struct cpu_accel_device *dev)
{
	unsigned int index;

	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->attached_regions[index];

		if (region)
			region->state = CPU_ACCEL_REGION_ACTIVE;
	}
}

static int cpu_accel_region_mappings_complete(struct cpu_accel_device *dev)
{
	unsigned int index;
	int ret = 0;

	mutex_lock(&dev->region_lock);
	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->attached_regions[index];

		if (region && (atomic_read(&region->manager_mappings) != 1 ||
			       atomic_read(&region->owner_mappings) != 1)) {
			ret = -EACCES;
			break;
		}
	}
	mutex_unlock(&dev->region_lock);
	return ret;
}

static void cpu_accel_regions_release(struct cpu_accel_device *dev)
{
	unsigned int index;

	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		struct cpu_accel_region *region = dev->regions[index];

		if (!region)
			continue;
		if (atomic_read(&region->manager_mappings) ||
		    atomic_read(&region->owner_mappings)) {
			pr_warn("cpu_accel: region %u still mapped at release\n",
				index + 1);
			continue;
		}
		account_locked_vm(dev->companion_mm,
				  region->bytes >> PAGE_SHIFT, false);
		vfree(region->address);
		kfree(region);
		dev->regions[index] = NULL;
	}
	memset(dev->attached_regions, 0, sizeof(dev->attached_regions));
	dev->region_total_bytes = 0;
}

static unsigned int cpu_accel_region_count_limit(void)
{
	return min_t(unsigned int, cpu_accel_max_region_count,
		     CPU_ACCEL_MAX_REGIONS);
}

static int cpu_accel_region_create_locked(struct cpu_accel_device *dev,
					  struct cpu_accel_region_create *create)
{
	struct cpu_accel_region *region;
	unsigned long pages;
	u64 token_sequence;
	unsigned int index;
	int ret;

	if (!cpu_accel_region_control_idle_locked(dev) || create->reserved ||
	    create->size_bytes > ULONG_MAX ||
	    !create->size_bytes || !PAGE_ALIGNED(create->size_bytes) ||
	    create->size_bytes > cpu_accel_max_region_bytes ||
	    !cpu_accel_region_permissions_valid(create->permissions))
		return -EINVAL;
	if (create->numa_node != NUMA_NO_NODE &&
	    (create->numa_node < 0 || create->numa_node >= MAX_NUMNODES ||
	     !node_state(create->numa_node, N_ONLINE)))
		return -EINVAL;
	if (dev->region_total_bytes > cpu_accel_max_total_region_bytes ||
	    cpu_accel_max_total_region_bytes - dev->region_total_bytes <
	    create->size_bytes)
		return -ENOMEM;

	/*
	 * Do not hold region_lock across account_locked_vm(): it takes the
	 * companion mmap write lock, while mmap takes that lock before
	 * region_lock.
	 */
	mutex_lock(&dev->region_lock);
	for (index = 0; index < cpu_accel_region_count_limit(); index++) {
		if (!dev->regions[index])
			break;
	}
	if (index == cpu_accel_region_count_limit()) {
		mutex_unlock(&dev->region_lock);
		return -ENOSPC;
	}
	mutex_unlock(&dev->region_lock);

	pages = create->size_bytes >> PAGE_SHIFT;
	ret = account_locked_vm(dev->companion_mm, pages, true);
	if (ret)
		return ret;

	region = kzalloc_obj(*region, GFP_KERNEL);
	if (!region) {
		ret = -ENOMEM;
		goto fail_account;
	}
	region->address = vmalloc_user_node(create->size_bytes,
					    create->numa_node);
	if (!region->address) {
		ret = -ENOMEM;
		goto fail_region;
	}

	token_sequence = atomic64_inc_return(&cpu_accel_region_token_sequence);
	if (!token_sequence || token_sequence > (ULONG_MAX >> PAGE_SHIFT)) {
		ret = -ENOSPC;
		goto fail_vmalloc;
	}

	region->bytes = create->size_bytes;
	region->epoch = 1;
	region->token = token_sequence << PAGE_SHIFT;
	region->permissions = create->permissions;
	region->numa_node = create->numa_node;
	region->state = CPU_ACCEL_REGION_CREATED;
	region->manager_vma.region = region;
	region->manager_vma.manager = true;
	region->owner_vma.region = region;
	atomic_set(&region->manager_mappings, 0);
	atomic_set(&region->owner_mappings, 0);

	/* The device lock serializes region-table mutation across this gap. */
	mutex_lock(&dev->region_lock);
	dev->regions[index] = region;
	dev->region_total_bytes += create->size_bytes;
	mutex_unlock(&dev->region_lock);

	create->region_id = index + 1;
	create->mmap_token = region->token;
	create->epoch = region->epoch;
	create->size_bytes = region->bytes;
	return 0;

fail_vmalloc:
	vfree(region->address);
fail_region:
	kfree(region);
fail_account:
	account_locked_vm(dev->companion_mm, pages, false);
	return ret;
}

static int cpu_accel_region_attach_locked(struct cpu_accel_device *dev,
					  struct cpu_accel_region_ref *ref)
{
	struct cpu_accel_region *region;

	if (!cpu_accel_region_control_idle_locked(dev) || ref->reserved)
		return -EINVAL;
	mutex_lock(&dev->region_lock);
	region = cpu_accel_region_find(dev, ref->region_id);
	if (!region || region->state != CPU_ACCEL_REGION_CREATED ||
	    ref->epoch != region->epoch ||
	    atomic_read(&region->manager_mappings) != 1 ||
	    atomic_read(&region->owner_mappings)) {
		mutex_unlock(&dev->region_lock);
		return -EINVAL;
	}
	region->attached = true;
	region->state = CPU_ACCEL_REGION_ATTACHED;
	region->epoch = cpu_accel_region_next_epoch(region->epoch);
	ref->epoch = region->epoch;
	mutex_unlock(&dev->region_lock);
	return 0;
}

static int cpu_accel_region_detach_locked(struct cpu_accel_device *dev,
					  struct cpu_accel_region_ref *ref)
{
	struct cpu_accel_region *region;

	if (!cpu_accel_region_control_idle_locked(dev) || ref->reserved)
		return -EINVAL;
	mutex_lock(&dev->region_lock);
	region = cpu_accel_region_find(dev, ref->region_id);
	if (!region || !region->attached || ref->epoch != region->epoch ||
	    atomic_read(&region->owner_mappings)) {
		mutex_unlock(&dev->region_lock);
		return -EINVAL;
	}
	region->attached = false;
	region->state = CPU_ACCEL_REGION_CREATED;
	region->epoch = cpu_accel_region_next_epoch(region->epoch);
	ref->epoch = region->epoch;
	mutex_unlock(&dev->region_lock);
	return 0;
}

static int cpu_accel_region_destroy_locked(struct cpu_accel_device *dev,
					   const struct cpu_accel_region_ref *ref)
{
	struct cpu_accel_region *region;
	unsigned long pages;

	if (!cpu_accel_region_control_idle_locked(dev) || ref->reserved)
		return -EINVAL;
	mutex_lock(&dev->region_lock);
	region = cpu_accel_region_find(dev, ref->region_id);
	if (!region) {
		mutex_unlock(&dev->region_lock);
		return -EINVAL;
	}
	if (region->attached ||
	    atomic_read(&region->manager_mappings) ||
	    atomic_read(&region->owner_mappings)) {
		mutex_unlock(&dev->region_lock);
		return -EBUSY;
	}
	if (ref->epoch != region->epoch) {
		mutex_unlock(&dev->region_lock);
		return -EINVAL;
	}
	pages = region->bytes >> PAGE_SHIFT;
	vfree(region->address);
	dev->regions[ref->region_id - 1] = NULL;
	dev->region_total_bytes -= region->bytes;
	kfree(region);
	mutex_unlock(&dev->region_lock);
	account_locked_vm(dev->companion_mm, pages, false);
	return 0;
}

static int cpu_accel_region_info_locked(struct cpu_accel_device *dev,
					struct cpu_accel_region_info *info)
{
	struct cpu_accel_region *region;

	if (info->region_id > CPU_ACCEL_MAX_REGIONS)
		return -EINVAL;
	mutex_lock(&dev->region_lock);
	region = cpu_accel_region_find(dev, info->region_id);
	if (!region) {
		mutex_unlock(&dev->region_lock);
		return -ENOENT;
	}
	info->state = region->state;
	info->permissions = region->permissions;
	info->numa_node = region->numa_node;
	info->manager_mappings = atomic_read(&region->manager_mappings);
	info->owner_mappings = atomic_read(&region->owner_mappings);
	info->size_bytes = region->bytes;
	info->mmap_token = region->token;
	info->epoch = region->epoch;
	mutex_unlock(&dev->region_lock);
	return 0;
}

static int cpu_accel_free_workload(struct cpu_accel_device *dev)
{
	if (dev->work_region_allocated)
		kfree(dev->work_region.address);
	dev->work_region.address = NULL;
	dev->work_region.bytes = 0;
	dev->work_region_allocated = false;
	return 0;
}

static int cpu_accel_prepare_workload(struct cpu_accel_device *dev,
					      const struct cpu_accel_config *config)
{
	void *buffer;
	size_t bytes;
	size_t size;

	if (config->workload == CPU_ACCEL_WORKLOAD_TIMESTAMP)
		return cpu_accel_free_workload(dev);
	if (cpu_accel_user_workload(config->workload))
		return cpu_accel_free_workload(dev);
	if (config->workload != CPU_ACCEL_WORKLOAD_MEMMOVE ||
	    config->work_bytes < 64 ||
	    config->work_bytes > CPU_ACCEL_MAX_MEMMOVE_WORK_BYTES)
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

static int cpu_accel_register_tracepoints(void)
{
	int ret;

	ret = register_trace_workqueue_queue_work(cpu_accel_workqueue_queue,
						  &cpu_accel);
	if (ret)
		return ret;

	ret = register_trace_workqueue_execute_start(
		cpu_accel_workqueue_execute_start, &cpu_accel);
	if (ret)
		goto out_queue_work;

#ifdef CONFIG_X86
	ret = register_trace_sched_process_exit_early(cpu_accel_owner_exit_early,
						      NULL);
	if (ret)
		goto out_execute_start;

	ret = register_trace_sched_process_exit(cpu_accel_owner_death, NULL);
	if (ret)
		goto out_exit_early;
#endif
	return 0;
#ifdef CONFIG_X86
out_exit_early:
	unregister_trace_sched_process_exit_early(cpu_accel_owner_exit_early,
						  NULL);
out_execute_start:
	unregister_trace_workqueue_execute_start(
		cpu_accel_workqueue_execute_start, &cpu_accel);
#endif
out_queue_work:
	unregister_trace_workqueue_queue_work(cpu_accel_workqueue_queue,
					      &cpu_accel);
	return ret;
}

static void cpu_accel_unregister_tracepoints(void)
{
#ifdef CONFIG_X86
	unregister_trace_sched_process_exit(cpu_accel_owner_death, NULL);
	unregister_trace_sched_process_exit_early(cpu_accel_owner_exit_early,
						 NULL);
#endif
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
	dev->shared->abi_version = dev->selected_abi;
	dev->shared->struct_size = sizeof(*dev->shared);
	dev->shared->layout_fingerprint = CPU_ACCEL_LAYOUT_FINGERPRINT();
	dev->shared->backend = cpu_accel_backend_id();
	dev->shared->backend_flags = 0;
	dev->shared->recovery_state = CPU_ACCEL_RECOVERY_NONE;
	dev->shared->mode = CPU_ACCEL_MODE_LINUX;
	dev->shared->workload = CPU_ACCEL_WORKLOAD_TIMESTAMP;
	cpu_accel_state_write(dev, CPU_ACCEL_STATE_IDLE);
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
	obs->arch_tlb_shootdown_targets_entry =
		cpu_accel_arch_tlb_shootdown_targets(obs->cpu);
	obs->arch_reschedule_deferred_entry =
		cpu_accel_arch_reschedule_deferred(obs->cpu);
	obs->arch_call_function_deferred_entry =
		cpu_accel_arch_call_function_deferred(obs->cpu);
	obs->arch_nmi_entry = cpu_accel_arch_nmi_count(obs->cpu);
	obs->arch_mce_exception_entry =
		cpu_accel_arch_mce_exception_count(obs->cpu);
	obs->arch_mce_poll_entry = cpu_accel_arch_mce_poll_count(obs->cpu);
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
	u64 arch_tlb_shootdown_targets =
		cpu_accel_arch_tlb_shootdown_targets(obs->cpu);
	u64 arch_reschedule_deferred =
		cpu_accel_arch_reschedule_deferred(obs->cpu);
	u64 arch_call_function_deferred =
		cpu_accel_arch_call_function_deferred(obs->cpu);
	u64 arch_nmi_count = cpu_accel_arch_nmi_count(obs->cpu);
	u64 arch_mce_exception_count =
		cpu_accel_arch_mce_exception_count(obs->cpu);
	u64 arch_mce_poll_count = cpu_accel_arch_mce_poll_count(obs->cpu);
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
	u64 arch_tlb_shootdown_targets_delta = cpu_accel_counter_delta(
		arch_tlb_shootdown_targets,
		obs->arch_tlb_shootdown_targets_entry);
	u64 arch_reschedule_deferred_delta =
		cpu_accel_counter_delta(arch_reschedule_deferred,
					obs->arch_reschedule_deferred_entry);
	u64 arch_call_function_deferred_delta =
		cpu_accel_counter_delta(arch_call_function_deferred,
					obs->arch_call_function_deferred_entry);
	u64 arch_nmi_delta = cpu_accel_counter_delta(arch_nmi_count,
						     obs->arch_nmi_entry);
	u64 arch_mce_exception_delta =
		cpu_accel_counter_delta(arch_mce_exception_count,
					obs->arch_mce_exception_entry);
	u64 arch_mce_poll_delta =
		cpu_accel_counter_delta(arch_mce_poll_count,
					obs->arch_mce_poll_entry);
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
	shared->arch_tlb_shootdown_targets =
		arch_tlb_shootdown_targets_delta;
	shared->arch_reschedule_deferred = arch_reschedule_deferred_delta;
	shared->arch_call_function_deferred = arch_call_function_deferred_delta;
	shared->arch_nmi_count = arch_nmi_delta;
	shared->arch_mce_exception_count = arch_mce_exception_delta;
	shared->arch_mce_poll_count = arch_mce_poll_delta;
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

#ifdef CONFIG_X86
#define CPU_ACCEL_MAX_USER_IMAGE_BYTES	(4UL * 1024UL * 1024UL)
#define CPU_ACCEL_MAX_USER_STACK_BYTES	(1UL * 1024UL * 1024UL)
#define CPU_ACCEL_MAX_USER_RECLAIM_BYTES	(64UL * 1024UL * 1024UL)

static int cpu_accel_validate_user_range(struct mm_struct *mm,
					unsigned long start, unsigned long bytes,
					unsigned long required_flags)
{
	unsigned long addr = start;
	unsigned long end;

	if (!bytes || bytes > ULONG_MAX - start ||
	    !access_ok((void __user *)start, bytes))
		return -EINVAL;
	end = start + bytes;
	while (addr < end) {
		struct vm_area_struct *vma = vma_lookup(mm, addr);
		unsigned long next;

		if (!vma || addr < vma->vm_start ||
		    (vma->vm_flags & required_flags) != required_flags ||
		    (required_flags & VM_EXEC && vma->vm_flags & VM_WRITE))
			return -EACCES;
		next = min(end, vma->vm_end);
		if (next <= addr)
			return -EFAULT;
		addr = next;
	}
	return 0;
}

static bool cpu_accel_vma_has_permissions(struct vm_area_struct *vma,
					  vm_flags_t required,
					  vm_flags_t forbidden)
{
	return (vma->vm_flags & required) == required &&
		!(vma->vm_flags & forbidden);
}

static bool cpu_accel_region_vma_permissions_match(
		struct vm_area_struct *vma, struct cpu_accel_region *region,
		bool manager)
{
	vm_flags_t permission_mask =
		VM_READ | VM_WRITE | VM_EXEC | VM_SHARED;
	vm_flags_t allowed = VM_SHARED;

	if (vma->vm_end - vma->vm_start != region->bytes ||
	    vma->vm_pgoff != (region->token >> PAGE_SHIFT))
		return false;
	allowed |= (manager && (region->permissions &
			 CPU_ACCEL_REGION_PERM_MANAGER_READ)) ||
	    (!manager && (region->permissions &
			  CPU_ACCEL_REGION_PERM_OWNER_READ)) ? VM_READ : 0;
	allowed |= (manager && (region->permissions &
			 CPU_ACCEL_REGION_PERM_MANAGER_WRITE)) ||
	    (!manager && (region->permissions &
			  CPU_ACCEL_REGION_PERM_OWNER_WRITE)) ? VM_WRITE : 0;
	return (vma->vm_flags & permission_mask) == allowed;
}

static bool cpu_accel_region_vma_matches(struct vm_area_struct *vma,
					 struct cpu_accel_region *region,
					 bool manager)
{
	struct cpu_accel_region_vma *region_vma = vma->vm_private_data;

	if (!region_vma || region_vma->region != region ||
	    region_vma->manager != manager)
		return false;

	return cpu_accel_region_vma_permissions_match(vma, region, manager);
}

static int cpu_accel_validate_control_and_regions(struct cpu_accel_device *dev,
						  struct mm_struct *mm)
{
	struct vm_area_struct *vma;
	bool control_found = false;
	bool region_found[CPU_ACCEL_MAX_REGIONS] = {};
	unsigned int index;

	VMA_ITERATOR(vmi, mm, 0);
	for_each_vma(vmi, vma) {
		struct cpu_accel_region_vma *region_vma;

		if (vma->vm_ops == &cpu_accel_control_vm_ops &&
		    vma->vm_file && vma->vm_file->private_data == dev &&
		    !vma->vm_pgoff &&
		    vma->vm_end - vma->vm_start == CPU_ACCEL_MAP_SIZE &&
		    cpu_accel_vma_has_permissions(vma, VM_READ | VM_SHARED,
						  VM_WRITE | VM_EXEC)) {
			if (control_found)
				return -EACCES;
			control_found = true;
			continue;
		}
		if (vma->vm_ops != &cpu_accel_region_vm_ops ||
		    !vma->vm_file || vma->vm_file->private_data != dev)
			continue;
		region_vma = vma->vm_private_data;
		if (!region_vma || region_vma->manager)
			return -EACCES;
		for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
			if (dev->attached_regions[index] == region_vma->region)
				break;
		}
		if (index == CPU_ACCEL_MAX_REGIONS ||
		    region_found[index] ||
		    !cpu_accel_region_vma_matches(vma, region_vma->region,
						  false))
			return -EACCES;
		region_found[index] = true;
	}

	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		if (dev->attached_regions[index] && !region_found[index])
			return -EACCES;
	}
	if (!control_found)
		return -EACCES;
	return 0;
}

static int cpu_accel_validate_user_vma_set(struct cpu_accel_device *dev,
					  struct mm_struct *mm,
					  unsigned long image_start,
					  unsigned long image_end,
					  unsigned long stack_start,
					  unsigned long stack_end,
					  unsigned long rseq_start,
					  unsigned long rseq_end,
					  unsigned long reclaim_page)
{
	struct vm_area_struct *vma;
	VMA_ITERATOR(vmi, mm, 0);
	bool image_found = false;
	bool stack_found = false;
	int ret;
	bool rseq_found = !rseq_start;
	bool reclaim_found = !reclaim_page;

	for_each_vma(vmi, vma) {
		bool contains_rseq = rseq_start &&
			vma->vm_start <= rseq_start && vma->vm_end >= rseq_end;

		if (contains_rseq) {
			if (!cpu_accel_vma_has_permissions(vma,
							 VM_READ | VM_WRITE,
							 VM_EXEC | VM_SHARED))
				return -EACCES;
			rseq_found = true;
		}

		/* This fixed architecture mapping cannot be removed by munmap(). */
		if (vma->vm_start == VSYSCALL_ADDR &&
		    vma->vm_end == VSYSCALL_ADDR + PAGE_SIZE && !vma->vm_file &&
		    cpu_accel_vma_has_permissions(vma, VM_EXEC,
						 VM_READ | VM_WRITE))
			continue;
		if (vma->vm_start == image_start && vma->vm_end == image_end &&
		    vma->vm_file &&
		    cpu_accel_vma_has_permissions(vma, VM_READ | VM_EXEC,
						 VM_WRITE | VM_SHARED)) {
			image_found = true;
			continue;
		}
		if (vma->vm_start == stack_start && vma->vm_end == stack_end &&
		    !vma->vm_file &&
		    cpu_accel_vma_has_permissions(vma, VM_READ | VM_WRITE,
						 VM_EXEC | VM_SHARED)) {
			stack_found = true;
			continue;
		}
		if (reclaim_page && vma->vm_start == reclaim_page &&
		    vma->vm_end > reclaim_page &&
		    vma->vm_end - reclaim_page <=
				CPU_ACCEL_MAX_USER_RECLAIM_BYTES && vma->vm_file &&
		    vma->vm_file->f_inode &&
		    S_ISREG(file_inode(vma->vm_file)->i_mode) &&
		    cpu_accel_vma_has_permissions(vma, VM_READ,
						  VM_WRITE | VM_EXEC | VM_IO |
						  VM_PFNMAP) &&
		    (vma->vm_flags & (VM_MAYSHARE | VM_MAYWRITE))) {
			/* Accept read-only shared and private regular-file mappings. */
			reclaim_found = true;
			continue;
		}
		if (vma->vm_ops == &cpu_accel_control_vm_ops ||
		    vma->vm_ops == &cpu_accel_region_vm_ops) {
			continue;
		}
		if (rseq_start && vma->vm_start == rseq_start &&
		    vma->vm_end == rseq_end &&
		    cpu_accel_vma_has_permissions(vma, VM_READ | VM_WRITE,
						 VM_EXEC | VM_SHARED)) {
			/* The user-return RSEQ slowpath may update this page. */
			rseq_found = true;
			continue;
		}

		return -EACCES;
	}

	ret = cpu_accel_validate_control_and_regions(dev, mm);
	if (ret)
		return ret;

	return image_found && stack_found &&
		rseq_found && reclaim_found ? 0 : -EACCES;
}

static void cpu_accel_user_image_release(struct cpu_accel_device *dev)
{
	struct cpu_accel_user_image *image = &dev->user_image;

	/* Keep mm writers out until CPU ownership and TLB state are reconciled. */
#ifdef CONFIG_X86_LOCAL_APIC
	if (image->owner_active) {
		u64 targets = x86_cpu_accel_user_exit(dev->config.cpu);

		/*
		 * Prefer the stop-context snapshot: the release path runs
		 * after the NMI exit, so its read can include targets counted
		 * after ownership ended.
		 */
		if (atomic_read(&dev->user_stop_exit_done))
			targets = atomic64_read(&dev->user_stop_exit_tlb_targets);
		dev->shared->arch_tlb_shootdown_targets =
			cpu_accel_counter_delta(targets, dev->user_observation.
				arch_tlb_shootdown_targets_entry);
		image->owner_active = false;
	}
	atomic_set(&dev->user_stop_exit_done, 0);
	atomic64_set(&dev->user_stop_exit_tlb_targets, 0);
#endif
	if (image->mm_locked) {
		mmap_write_unlock(image->mm);
		image->mm_locked = false;
	}
	if (image->pages) {
		unpin_user_pages(image->pages, image->pinned_pages);
		kvfree(image->pages);
	}
	if (image->mm)
		mmput(image->mm);
	if (image->task)
		put_task_struct(image->task);
	memset(image, 0, sizeof(*image));
}

static int cpu_accel_user_image_prepare(struct cpu_accel_device *dev)
{
	struct cpu_accel_user_image *image = &dev->user_image;
	struct mm_struct *mm = current->mm;
	unsigned long stack_start;
	unsigned long rseq_start = 0;
	unsigned long rseq_end = 0;
	unsigned long reclaim_page = dev->config.user_reclaim_page;
	unsigned int image_pages;
	unsigned int stack_pages;
	unsigned int rseq_pages;
	long pinned;
	int ret;

	if (!mm)
		return -EINVAL;
	if (mm == dev->companion_mm)
		return -EXDEV;
	if (atomic_read(&mm->mm_users) != 1 || get_nr_threads(current) != 1)
		return -EBUSY;
	if (!PAGE_ALIGNED(dev->config.user_image_start) ||
	    !PAGE_ALIGNED(dev->config.user_image_bytes) ||
	    !PAGE_ALIGNED(dev->config.user_stack_top) ||
	    !PAGE_ALIGNED(dev->config.user_stack_bytes) ||
	    !dev->config.user_image_bytes || !dev->config.user_stack_bytes ||
	    dev->config.user_image_bytes > CPU_ACCEL_MAX_USER_IMAGE_BYTES ||
	    dev->config.user_stack_bytes > CPU_ACCEL_MAX_USER_STACK_BYTES ||
	    dev->config.user_stack_top < dev->config.user_stack_bytes)
		return -EINVAL;
	stack_start = dev->config.user_stack_top -
		dev->config.user_stack_bytes;
#ifdef CONFIG_RSEQ
	if (current->rseq.usrptr) {
		unsigned long rseq_addr =
			(unsigned long)current->rseq.usrptr;
		unsigned long rseq_bytes = current->rseq.len;
		unsigned long rseq_limit;

		if (!rseq_bytes || rseq_bytes > PAGE_SIZE ||
		    rseq_addr > ULONG_MAX - rseq_bytes)
			return -EINVAL;
		rseq_limit = rseq_addr + rseq_bytes;
		if (rseq_limit > ULONG_MAX - (PAGE_SIZE - 1))
			return -EINVAL;
		rseq_start = rseq_addr & PAGE_MASK;
		rseq_end = PAGE_ALIGN(rseq_limit);
		if (rseq_end <= rseq_start)
			return -EINVAL;
	}
#endif
	if (dev->config.user_image_start + dev->config.user_image_bytes <
		dev->config.user_image_start ||
		dev->config.user_stack_top < stack_start)
		return -EINVAL;
	if (dev->config.user_entry_ip < dev->config.user_image_start ||
	    dev->config.user_entry_ip >= dev->config.user_image_start +
		dev->config.user_image_bytes ||
	    dev->config.user_escape_ip < dev->config.user_image_start ||
	    dev->config.user_escape_ip >= dev->config.user_image_start +
		dev->config.user_image_bytes ||
	    dev->config.user_arg < stack_start ||
	    dev->config.user_arg >= dev->config.user_stack_top)
		return -EINVAL;
	if (dev->config.user_image_start + dev->config.user_image_bytes >
		stack_start && dev->config.user_image_start <
		dev->config.user_stack_top)
		return -EINVAL;
	if (dev->config.workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM) {
		if (!reclaim_page || !PAGE_ALIGNED(reclaim_page) ||
		    reclaim_page > ULONG_MAX - PAGE_SIZE ||
		    !access_ok((void __user *)reclaim_page, PAGE_SIZE) ||
		    (reclaim_page < dev->config.user_image_start +
				dev->config.user_image_bytes &&
		     dev->config.user_image_start < reclaim_page + PAGE_SIZE) ||
		    (reclaim_page < dev->config.user_stack_top &&
		     stack_start < reclaim_page + PAGE_SIZE) ||
		    (rseq_start && reclaim_page < rseq_end &&
		     rseq_start < reclaim_page + PAGE_SIZE))
			return -EINVAL;
	} else if (reclaim_page) {
		return -EINVAL;
	}

	image_pages = dev->config.user_image_bytes >> PAGE_SHIFT;
	stack_pages = dev->config.user_stack_bytes >> PAGE_SHIFT;
	rseq_pages = (rseq_end - rseq_start) >> PAGE_SHIFT;
	image->pages = kvmalloc_array(image_pages + stack_pages + rseq_pages,
				      sizeof(*image->pages), GFP_KERNEL);
	if (!image->pages)
		return -ENOMEM;
	image->nr_pages = image_pages + stack_pages + rseq_pages;

	image->mm = get_task_mm(current);
	if (!image->mm) {
		ret = -ESRCH;
		goto fail;
	}
	mmap_read_lock(image->mm);
	ret = cpu_accel_validate_user_range(image->mm,
					    dev->config.user_image_start,
					    dev->config.user_image_bytes,
					    VM_READ | VM_EXEC);
	if (!ret)
		ret = cpu_accel_validate_user_range(image->mm, stack_start,
						    dev->config.user_stack_bytes,
						    VM_READ | VM_WRITE);
	if (!ret && reclaim_page)
		ret = cpu_accel_validate_user_range(image->mm, reclaim_page,
						    PAGE_SIZE, VM_READ);
	mmap_read_unlock(image->mm);
	if (ret)
		goto fail;

	pinned = pin_user_pages_fast(dev->config.user_image_start,
				     image_pages, 0, image->pages);
	if (pinned > 0)
		image->pinned_pages = pinned;
	if (pinned != image_pages) {
		ret = pinned < 0 ? (int)pinned : -EFAULT;
		goto fail;
	}
	pinned = pin_user_pages_fast(stack_start, stack_pages, FOLL_WRITE,
				     image->pages + image_pages);
	if (pinned > 0)
		image->pinned_pages += pinned;
	if (pinned != stack_pages) {
		ret = pinned < 0 ? (int)pinned : -EFAULT;
		goto fail;
	}
	if (rseq_pages) {
		/* Keep the registered user area resident for kernel RSEQ paths. */
		pinned = pin_user_pages_fast(rseq_start, rseq_pages, FOLL_WRITE,
					     image->pages + image_pages + stack_pages);
		if (pinned > 0)
			image->pinned_pages += pinned;
		if (pinned != rseq_pages) {
			ret = pinned < 0 ? (int)pinned : -EFAULT;
			goto fail;
		}
	}
	mmap_write_lock(image->mm);
	image->mm_locked = true;
	ret = cpu_accel_region_mappings_complete(dev);
	if (ret)
		goto fail;
	ret = cpu_accel_validate_user_range(image->mm,
					    dev->config.user_image_start,
					    dev->config.user_image_bytes,
					    VM_READ | VM_EXEC);
	if (!ret)
		ret = cpu_accel_validate_user_range(image->mm, stack_start,
						    dev->config.user_stack_bytes,
						    VM_READ | VM_WRITE);
	if (!ret && reclaim_page)
		ret = cpu_accel_validate_user_range(image->mm, reclaim_page,
						    PAGE_SIZE, VM_READ);
	if (!ret)
		ret = cpu_accel_validate_user_vma_set(dev, image->mm,
						       dev->config.user_image_start,
						       dev->config.user_image_start +
						       dev->config.user_image_bytes,
						       stack_start,
						       dev->config.user_stack_top,
						       rseq_start, rseq_end,
						       reclaim_page);
	if (ret)
		goto fail;

	get_task_struct(current);
	image->task = current;
	image->entry_ip = dev->config.user_entry_ip;
	image->stack_top = dev->config.user_stack_top;
	image->stack_bytes = dev->config.user_stack_bytes;
	image->image_start = dev->config.user_image_start;
	image->image_bytes = dev->config.user_image_bytes;
	image->arg = dev->config.user_arg;
	image->escape_ip = dev->config.user_escape_ip;
	return 0;

fail:
	cpu_accel_user_image_release(dev);
	return ret;
}

static int cpu_accel_arch_user_enter(struct cpu_accel_device *dev)
{
	struct cpu_accel_user_image *image = &dev->user_image;
	struct pt_regs *regs = current_pt_regs();
	bool death_safe = cpu_accel_user_death_safe(dev->config.workload);
#ifdef CONFIG_X86_LOCAL_APIC
	u64 tlb_targets_entry = 0;
	int ret = 0;
#endif

#ifdef CONFIG_X86_LOCAL_APIC
	/*
	 * Accelerated owners register the mmu-owner stop callback (escape NMI
	 * on a later stop/MCE).  Death-safe owners are ordinary tasks and need
	 * no stop hook, so leave owner_active false.
	 */
	if (!death_safe) {
		ret = x86_cpu_accel_user_enter(raw_smp_processor_id(), image->mm,
					       cpu_accel_arch_stop_user, dev,
					       &tlb_targets_entry);
		if (ret)
			return ret;
		image->owner_active = true;
	}
#endif
	cpu_accel_observation_begin(&dev->user_observation);
#ifdef CONFIG_X86_LOCAL_APIC
	dev->user_observation.arch_tlb_shootdown_targets_entry =
		tlb_targets_entry;
#endif
	WRITE_ONCE(dev->shared->start_ns,
		   dev->user_observation.lifecycle_entry_ns);
	WRITE_ONCE(dev->shared->lifecycle_entry_ns,
		   dev->user_observation.lifecycle_entry_ns);
	WRITE_ONCE(dev->shared->user_active_start_ns,
		   dev->user_observation.lifecycle_entry_ns);
	image->return_regs = *regs;
	regs->ip = image->entry_ip;
	regs->sp = image->stack_top;
	regs->csx = __USER_CS;
	regs->ssx = __USER_DS;
	regs->di = image->arg;
	/*
	 * Force the syscall exit path to use IRET.  Accelerated owners enter
	 * with IF cleared (no interrupts in the protected interval); death-safe
	 * owners keep IF set so a fatal signal can interrupt them.
	 */
	regs->flags = death_safe ?
		(X86_EFLAGS_FIXED | X86_EFLAGS_IF) : X86_EFLAGS_FIXED;
	regs->cx = ~regs->ip;
	regs->r11 = ~regs->flags;
	return 0;
}

static void cpu_accel_arch_user_exit(const struct pt_regs *return_regs)
{
	struct pt_regs *regs = current_pt_regs();

	/*
	 * Restore every user register; the compiler may keep the handle in any
	 * callee-saved register across the USER_EXIT ioctl.
	 */
	*regs = *return_regs;
	regs->flags = return_regs->flags |
		X86_EFLAGS_IF | X86_EFLAGS_FIXED;
	regs->ax = 0;
}
#else
static int cpu_accel_user_image_prepare(struct cpu_accel_device *dev)
{
	(void)dev;
	return -EOPNOTSUPP;
}

static void cpu_accel_user_image_release(struct cpu_accel_device *dev)
{
	(void)dev;
}

static int cpu_accel_arch_user_enter(struct cpu_accel_device *dev)
{
	(void)dev;
	return -EOPNOTSUPP;
}
#endif

static int cpu_accel_user_exit_locked(struct cpu_accel_device *dev)
{
#ifdef CONFIG_X86
	struct pt_regs return_regs = dev->user_image.return_regs;
	bool escaped = atomic_read(&dev->user_escape_seen);
	int irq_ret;

	if (!dev->user_image.active || dev->user_image.task != current ||
	    dev->user_image.mm != current->mm)
		return -EPERM;

	WRITE_ONCE(dev->shared->user_active_end_ns,
		   ktime_get_mono_fast_ns());
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_EXITING);
	/*
	 * The protected user interval ends at the terminal syscall boundary;
	 * exclude all following Linux-owned cleanup from activity telemetry.
	 */
	atomic_set(&dev->running, 0);
	dev->shared->end_ns = ktime_get_mono_fast_ns();
	dev->shared->stop_requested = 0;
	cpu_accel_observation_finish(dev, &dev->user_observation);
	dev->shared->end_ns = dev->shared->lifecycle_exit_ns;
	dev->shared->user_escape_count =
		atomic64_read(&dev->user_escape_count);
	if (escaped) {
		WRITE_ONCE(dev->shared->recovery_state,
			   CPU_ACCEL_RECOVERY_SUCCEEDED);
		WRITE_ONCE(dev->shared->recovery_error, 0);
		WRITE_ONCE(dev->shared->recovery_completed_ns,
			   READ_ONCE(dev->shared->user_active_end_ns));
	}
	dev->user_image.active = false;
#ifdef CONFIG_X86_LOCAL_APIC
	timer_delete(&dev->stop_escalation_timer);
#endif
	atomic_set(&dev->enter_requested, 0);
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->user_escape_requested, 0);

	/* The user interval is over before Linux-owned cleanup resumes. */
	cpu_accel_user_image_release(dev);
	local_irq_enable();
	irq_ret = cpu_accel_restore_irq_quarantine(dev);
	if (!irq_ret)
		cpu_accel_release_workqueue(dev);
	dev->lifecycle_ret = irq_ret;
	if (irq_ret) {
		/* NMI escape succeeded, but Linux ownership was not fully restored. */
		WRITE_ONCE(dev->shared->recovery_state,
			   CPU_ACCEL_RECOVERY_FAILED);
		WRITE_ONCE(dev->shared->recovery_error, irq_ret);
		WRITE_ONCE(dev->shared->recovery_completed_ns,
			   ktime_get_mono_fast_ns());
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
	} else {
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_LINUX);
		cpu_accel_state_write(dev, escaped ?
			CPU_ACCEL_STATE_ESCAPED : CPU_ACCEL_STATE_COMPLETE);
	}
	cpu_accel_region_finish(dev, escaped || irq_ret);
	/* Publish terminal telemetry before restoring the saved user frame. */
	smp_wmb();
	local_irq_disable();
	/*
	 * Module code cannot use preempt_enable_no_resched(); keep this decrement
	 * non-scheduling while interrupts remain disabled.  Accelerated owners
	 * entered with preempt_disable() (pc 0->1); death-safe owners entered
	 * with pc already 0, so only decrement the accelerated case.
	 */
	if (!cpu_accel_user_death_safe(dev->config.workload)) {
		barrier();
		preempt_count_dec();
	}
	cpu_accel_arch_user_exit(&return_regs);
	return irq_ret;
#else
	(void)dev;
	return -EOPNOTSUPP;
#endif
}

#ifdef CONFIG_X86
/*
 * Accelerated owners enter the user image with interrupts and preemption
 * disabled and holding the image mm write lock (see cpu_accel_arch_user_enter
 * and cpu_accel_user_image_prepare).  A fatal signal -- a killed owner, a
 * fatal #MC delivered as SIGBUS at the next return to user mode -- takes it
 * into do_exit(), which may sleep (acct_collect, exit_mm): illegal while
 * preemption is disabled, and exit_mm's mmap_read_lock() would deadlock
 * against the held write lock.  Death-safe owners are ordinary tasks (no
 * context to restore) but still hold the mm write lock, so they need the
 * unlock too.  Do the mode-appropriate part here, early in do_exit() and
 * before acct_collect().  The full image release (mmput, state) is deferred
 * to the sched_process_exit death work, which runs later in the same do_exit().
 */
static void cpu_accel_owner_exit_early(void *data, struct task_struct *task)
{
	struct cpu_accel_user_image *image = &cpu_accel.user_image;

	/* Fires for every process exit; the compare is the whole cost. */
	(void)data;
	if (task != READ_ONCE(image->task))
		return;
	if (!READ_ONCE(image->active))
		return;

	/*
	 * Accelerated owners (owner_active): balance the entry's
	 * preempt_disable(); with interrupts still disabled this decrement
	 * cannot reschedule, then re-enable interrupts so do_exit() may
	 * sleep.  Death-safe owners are already a normal task, so skip this
	 * (a decrement would underflow their preempt count).
	 */
	if (READ_ONCE(image->owner_active)) {
		preempt_count_dec();
		local_irq_enable();
	}
	/*
	 * Both modes: drop the mm write lock so exit_mm's read lock cannot
	 * deadlock.  mm_locked=false tells the deferred release to skip its
	 * own unlock.
	 */
	if (READ_ONCE(image->mm_locked)) {
		mmap_write_unlock(image->mm);
		WRITE_ONCE(image->mm_locked, false);
	}
}

/*
 * The owner can die without running its exit path (for example a fatal
 * #MC kills it with SIGBUS at the next return to user mode). Release the
 * image once the task has fully exited so the driver state does not stay
 * stale and the work region and CPU ownership can reconcile.
 */
static void cpu_accel_owner_death(void *data, struct task_struct *task,
				  bool group_dead)
{
	/* Fires for every process exit; the compare is the whole cost. */
	(void)data;
	(void)group_dead;
	if (task != READ_ONCE(cpu_accel.user_image.task))
		return;
	if (!READ_ONCE(cpu_accel.user_image.active))
		return;

	schedule_work(&cpu_accel.owner_death_work);
}

static void cpu_accel_owner_death_work(struct work_struct *work)
{
	struct cpu_accel_device *dev =
		container_of(work, struct cpu_accel_device, owner_death_work);
	struct cpu_accel_user_image *image = &dev->user_image;
	u64 now;
	int irq_ret;

	mutex_lock(&dev->lock);
	/*
	 * The image is released only through the exit path or here, both
	 * under this lock, so a live image is still the probed owner's.
	 * PF_EXITING is set at the start of do_exit(), so it holds for the
	 * whole death window, unlike TASK_DEAD which is set at the end.
	 */
	if (!image->active ||
	    !(READ_ONCE(image->task->flags) & PF_EXITING)) {
		mutex_unlock(&dev->lock);
		return;
	}
	now = ktime_get_mono_fast_ns();
	WRITE_ONCE(dev->shared->user_active_end_ns, now);
	dev->shared->end_ns = now;
	dev->shared->stop_requested = 0;
	atomic_set(&dev->running, 0);
	dev->shared->user_escape_count =
		atomic64_read(&dev->user_escape_count);
	WRITE_ONCE(dev->shared->recovery_state, CPU_ACCEL_RECOVERY_FAILED);
	WRITE_ONCE(dev->shared->recovery_error, -ESRCH);
	WRITE_ONCE(dev->shared->recovery_completed_ns, now);
	dev->user_image.active = false;
	atomic_set(&dev->enter_requested, 0);
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->user_escape_requested, 0);
#ifdef CONFIG_X86_LOCAL_APIC
	timer_delete_sync(&dev->stop_escalation_timer);
#endif
	/*
	 * The owner mm is still alive here: the driver holds its own
	 * reference and the mmu-owner registry keeps one until
	 * user_exit() unregisters, so the standard release stays safe.
	 */
	cpu_accel_user_image_release(dev);
	irq_ret = cpu_accel_restore_irq_quarantine(dev);
	if (!irq_ret)
		cpu_accel_release_workqueue(dev);
	cpu_accel_region_finish(dev, true);
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_LINUX);
	cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
	mutex_unlock(&dev->lock);
}
#endif

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
	u64 start = 0, deadline = 0, now;
	u64 work_iterations = 0;
	u32 final_state = CPU_ACCEL_STATE_COMPLETE;
	bool persistent = dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT;

	if (atomic_read(&dev->stop_requested)) {
		final_state = atomic_read(&dev->watchdog_fired) ?
			CPU_ACCEL_STATE_WATCHDOG : CPU_ACCEL_STATE_STOPPED;
		goto finish;
	}

	atomic_set(&dev->running, 1);
	cpu_accel_state_write(dev, CPU_ACCEL_STATE_RUNNING);
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

		if (dev->work_region.address) {
			char *buffer = dev->work_region.address;
			size_t bytes = dev->work_region.bytes / 2;

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
	shared->stop_requested = 0;
	cpu_accel_region_end(dev, final_state);
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->running, 0);
	cpu_accel_observation_finish(dev, obs);
	/* Publish all result fields before publishing the terminal state. */
	smp_wmb();
	cpu_accel_state_write(dev, final_state);
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
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		/* Publish the failed admission before restoring execution. */
		smp_wmb();
		cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
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
 * Keep this dispatch in one place so architecture backends do not leak into
 * the control plane or workload implementation.
 */
static int cpu_accel_lifecycle_enter(struct cpu_accel_device *dev)
{
	u64 targets_entry =
		cpu_accel_arch_tlb_shootdown_targets(dev->config.cpu);
	int ret;

	ret = cpu_accel_arch_enter(dev->config.cpu,
				   cpu_accel_lifecycle_entry, dev);
	if (!ret)
		dev->shared->arch_tlb_shootdown_targets =
			cpu_accel_counter_delta(
				cpu_accel_arch_tlb_shootdown_targets(
					dev->config.cpu), targets_entry);

	return ret;
}

static int cpu_accel_lifecycle_thread(void *data)
{
	struct cpu_accel_device *dev = data;
	int irq_ret;
	int ret;

	/* Keep the target online while the direct entry owns its execution. */
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
			cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
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
	cpu_accel_region_recover(dev);
	return dev->lifecycle_ret;
}

static int cpu_accel_open(struct inode *inode, struct file *file)
{
	struct mm_struct *mm = get_task_mm(current);

	if (!mm)
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN)) {
		mmput(mm);
		return -EPERM;
	}
	if (atomic_cmpxchg(&cpu_accel.opened, 0, 1))
		goto busy;

	cpu_accel.companion_mm = mm;
	file->private_data = &cpu_accel;
	return 0;

busy:
	mmput(mm);
	return -EBUSY;
}

static int cpu_accel_release(struct inode *inode, struct file *file)
{
	struct cpu_accel_device *dev = file->private_data;
	int ret;

	mutex_lock(&dev->lock);
	if (!dev->lifecycle_thread && !dev->user_image.active) {
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
	if (!dev->lifecycle_thread && !dev->user_image.active)
		cpu_accel_regions_release(dev);
	mutex_unlock(&dev->lock);
	if (dev->companion_mm) {
		mmput(dev->companion_mm);
		dev->companion_mm = NULL;
	}

	atomic_set(&dev->opened, 0);
	return 0;
}

static int cpu_accel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cpu_accel_device *dev = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	struct cpu_accel_region *region;
	bool manager;
	unsigned int index;
	int ret;

	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING)
		return -EBUSY;

	mutex_lock(&dev->region_lock);

	if (!vma->vm_pgoff && size == CPU_ACCEL_MAP_SIZE) {
		if (dev->configured) {
			mutex_unlock(&dev->region_lock);
			return -EBUSY;
		}
		if (!cpu_accel_vma_has_permissions(vma, VM_READ | VM_SHARED,
						   VM_WRITE | VM_EXEC)) {
			mutex_unlock(&dev->region_lock);
			return -EACCES;
		}
		vma->vm_ops = &cpu_accel_control_vm_ops;
		ret = remap_vmalloc_range(vma, dev->shared, 0);
		mutex_unlock(&dev->region_lock);
		return ret;
	}

	if (vma->vm_flags & VM_EXEC)
		goto invalid_permissions;
	if (!(vma->vm_flags & VM_SHARED))
		goto not_shared;
	manager = current->mm == dev->companion_mm;

	region = NULL;
	for (index = 0; index < CPU_ACCEL_MAX_REGIONS; index++) {
		if (dev->regions[index] &&
		    dev->regions[index]->token == (vma->vm_pgoff << PAGE_SHIFT)) {
			region = dev->regions[index];
			break;
		}
	}
	if (!region || region->bytes != size ||
	    (dev->configured && !region->attached) ||
	    (manager ? atomic_read(&region->manager_mappings) :
		       atomic_read(&region->owner_mappings)) ||
	    (!manager && !region->attached)) {
		mutex_unlock(&dev->region_lock);
		return -EINVAL;
	}
	if (!cpu_accel_region_vma_permissions_match(vma, region, manager)) {
		mutex_unlock(&dev->region_lock);
		return -EACCES;
	}
	ret = remap_vmalloc_range(vma, region->address, 0);
	if (ret) {
		mutex_unlock(&dev->region_lock);
		return ret;
	}
	vma->vm_ops = &cpu_accel_region_vm_ops;
	vma->vm_private_data = manager ? &region->manager_vma :
		&region->owner_vma;
	if (manager)
		atomic_inc(&region->manager_mappings);
	else
		atomic_inc(&region->owner_mappings);
	mutex_unlock(&dev->region_lock);

	return 0;

invalid_permissions:
	mutex_unlock(&dev->region_lock);
	return -EACCES;

not_shared:
	mutex_unlock(&dev->region_lock);
	return -EINVAL;
}

static int cpu_accel_start_locked(struct cpu_accel_device *dev)
{
	unsigned int irq_quarantined = 0;
	unsigned int irq_quarantine_blockers = 0;
	bool user_workload = cpu_accel_user_workload(dev->config.workload);
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

	if (user_workload) {
		current_cpu = raw_smp_processor_id();
		if (current_cpu != dev->config.cpu ||
		    cpumask_weight(current->cpus_ptr) != 1 ||
		    !cpumask_test_cpu(dev->config.cpu, current->cpus_ptr))
			return -EXDEV;
	} else {
		current_cpu = get_cpu();
		if (current_cpu == dev->config.cpu) {
			put_cpu();
			return -EBUSY;
		}
		dev->controller_cpu = current_cpu;
		put_cpu();
	}
	dev->controller_cpu = current_cpu;

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
	dev->shared->stop_requested = 0;
	dev->shared->start_ns = 0;
	dev->shared->end_ns = 0;
	dev->shared->lifecycle_entry_ns = 0;
	dev->shared->lifecycle_exit_ns = 0;
	dev->shared->user_active_start_ns = 0;
	dev->shared->user_active_end_ns = 0;
	dev->shared->user_escape_count = 0;
	dev->shared->recovery_state = CPU_ACCEL_RECOVERY_NONE;
	dev->shared->recovery_error = 0;
	dev->shared->recovery_requested_ns = 0;
	dev->shared->recovery_completed_ns = 0;
	dev->shared->recovery_attempts = 0;
	dev->shared->reserved_recovery = 0;
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
	dev->shared->arch_tlb_shootdown_targets = 0;
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
	atomic64_set(&dev->user_escape_count, 0);
	atomic_set(&dev->user_escape_requested, 0);
	atomic_set(&dev->user_escape_seen, 0);
	reinit_completion(&dev->lifecycle_done);
	dev->lifecycle_ret = -EINPROGRESS;
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->enter_requested, 1);
	WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_ENTERING);
	cpu_accel_state_write(dev, CPU_ACCEL_STATE_READY);

	if (user_workload) {
		ret = cpu_accel_user_image_prepare(dev);
		if (ret)
			goto user_start_fail;
		dev->user_image.active = true;
		atomic_set(&dev->running, 1);
		dev->lifecycle_ret = 0;
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_ACCELERATOR);
		cpu_accel_state_write(dev, CPU_ACCEL_STATE_RUNNING);
		local_irq_disable();
		ret = cpu_accel_arch_user_enter(dev);
		/*
		 * Accelerated owners must not be preempted or interrupted in
		 * the protected interval, so disable preemption here (their
		 * IF is cleared by the entry).  Death-safe owners stay a
		 * normal task and are left preemptible.
		 */
		if (!ret && !cpu_accel_user_death_safe(dev->config.workload))
			preempt_disable();
		local_irq_enable();
		if (!ret && dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT) {
			unsigned long watchdog_jiffies;

			watchdog_jiffies = max_t(unsigned long, 1,
						 nsecs_to_jiffies(dev->config.duration_ns));
			mod_timer(&dev->watchdog_timer,
				  jiffies + watchdog_jiffies);
		}
		if (!ret)
			return 0;
		dev->user_image.active = false;
		cpu_accel_user_image_release(dev);

user_start_fail:
		if (dev->irq_quarantine) {
			int irq_ret;

			irq_ret = cpu_accel_restore_irq_quarantine(dev);
			if (irq_ret)
				pr_err("cpu_accel: user image cleanup failed for CPU %u: %d\n",
				       dev->config.cpu, irq_ret);
		}
		if (!dev->irq_quarantine)
			cpu_accel_release_workqueue(dev);
		cpu_accel_region_finish(dev, true);
		atomic_set(&dev->running, 0);
		atomic_set(&dev->enter_requested, 0);
		WRITE_ONCE(dev->shared->mode, CPU_ACCEL_MODE_RECOVERY);
		cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
		return ret;
	}

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
		cpu_accel_state_write(dev, CPU_ACCEL_STATE_ERROR);
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
	struct cpu_accel_capabilities capabilities;
	struct cpu_accel_abi_info abi_info;
	struct cpu_accel_abi_select abi_select;
	struct cpu_accel_arch_telemetry arch_telemetry;
	struct cpu_accel_x86_image_admission x86_image;
	struct cpu_accel_region_create region_create;
	struct cpu_accel_region_ref region_ref;
	struct cpu_accel_region_info region_info;
	unsigned int attached_count;
	int ret = 0;

	if (_IOC_TYPE(command) != CPU_ACCEL_IOC_MAGIC)
		return -ENOTTY;
	if (command == CPU_ACCEL_IOC_USER_ESCAPE)
		return cpu_accel_user_escape(dev);

	mutex_lock(&dev->lock);
	switch (command) {
	case CPU_ACCEL_IOC_GET_ABI_INFO:
		abi_info = (struct cpu_accel_abi_info){
			.current_abi = CPU_ACCEL_ABI_VERSION,
			.min_abi = CPU_ACCEL_ABI_MIN_VERSION,
			.max_abi = CPU_ACCEL_ABI_VERSION,
			.selected_abi = dev->selected_abi,
		};
		if (copy_to_user((void __user *)argument, &abi_info,
				 sizeof(abi_info)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_SET_ABI_VERSION:
		if (copy_from_user(&abi_select, (void __user *)argument,
				   sizeof(abi_select))) {
			ret = -EFAULT;
			break;
		}
		if (abi_select.reserved ||
		    abi_select.requested_abi < CPU_ACCEL_ABI_MIN_VERSION ||
		    abi_select.requested_abi > CPU_ACCEL_ABI_VERSION) {
			ret = -EINVAL;
			break;
		}
		abi_select.selected_abi = abi_select.requested_abi;
		if (copy_to_user((void __user *)argument, &abi_select,
				 sizeof(abi_select))) {
			ret = -EFAULT;
		} else {
			dev->selected_abi = abi_select.requested_abi;
			dev->shared->abi_version = dev->selected_abi;
		}
		break;

	case CPU_ACCEL_IOC_GET_ARCH_TELEMETRY:
		arch_telemetry = (struct cpu_accel_arch_telemetry){
			.arch_irq_count = dev->shared->arch_irq_count,
			.arch_ipi_count = dev->shared->arch_ipi_count,
			.arch_tlb_count = dev->shared->arch_tlb_count,
			.arch_tlb_shootdown_targets =
				dev->shared->arch_tlb_shootdown_targets,
			.arch_reschedule_deferred =
				dev->shared->arch_reschedule_deferred,
			.arch_call_function_deferred =
				dev->shared->arch_call_function_deferred,
			.arch_nmi_count = dev->shared->arch_nmi_count,
			.arch_mce_exception_count =
				dev->shared->arch_mce_exception_count,
			.arch_mce_poll_count =
				dev->shared->arch_mce_poll_count,
			.arch_counters_valid =
				dev->shared->arch_counters_valid,
		};
		if (copy_to_user((void __user *)argument, &arch_telemetry,
				 sizeof(arch_telemetry)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_X86_IMAGE_ADMISSION:
		if (copy_from_user(&x86_image, (void __user *)argument,
				   sizeof(x86_image))) {
			ret = -EFAULT;
			break;
		}
		if (x86_image.reserved || x86_image.image_handle ||
		    !x86_image.user_entry_ip || !x86_image.user_stack_top ||
		    !x86_image.user_stack_bytes ||
		    !x86_image.user_image_start ||
		    !x86_image.user_image_bytes || !x86_image.user_arg ||
		    !x86_image.user_escape_ip) {
			ret = -EINVAL;
			break;
		}
		x86_image.image_handle = 1;
		if (copy_to_user((void __user *)argument, &x86_image,
				 sizeof(x86_image)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_CONFIG:
		if (copy_from_user(&config, (void __user *)argument,
				   sizeof(config))) {
			ret = -EFAULT;
			break;
		}
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread || dev->user_image.active) {
			ret = -EBUSY;
			break;
		}
		if (cpu_accel_user_workload(config.workload) &&
		    current->mm == dev->companion_mm) {
			ret = -EXDEV;
			break;
		}
		if (dev->configured) {
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
			    config.reserved ||
		    config.workload > CPU_ACCEL_WORKLOAD_USER_OSLAT ||
		    config.work_bytes > CPU_ACCEL_MAX_WORK_BYTES ||
		    (config.workload == CPU_ACCEL_WORKLOAD_TIMESTAMP &&
		     config.work_bytes) ||
		    (config.workload == CPU_ACCEL_WORKLOAD_MEMMOVE &&
		     (config.work_bytes < 64 ||
		      config.work_bytes >
		      CPU_ACCEL_MAX_MEMMOVE_WORK_BYTES)) ||
			(config.workload == CPU_ACCEL_WORKLOAD_USER_HANG &&
			 !(cpu_accel_backend_flags_for_workload(config.workload) &
			   CPU_ACCEL_BACKEND_FLAG_USER_ESCAPE)) ||
			(cpu_accel_user_workload(config.workload) &&
				 ((config.workload != CPU_ACCEL_WORKLOAD_USER_REGION &&
				   config.work_bytes) ||
				  (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION &&
				   (!config.work_bytes ||
				    config.work_bytes > CPU_ACCEL_MAX_WORK_BYTES)) ||
			  config.user_entry_ip == 0 || config.user_stack_top == 0 ||
			  config.user_stack_bytes == 0 || config.user_image_start == 0 ||
			  config.user_image_bytes == 0 || config.user_arg == 0 ||
			  config.user_escape_ip == 0 ||
			  ((config.workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM) !=
			   !!config.user_reclaim_page))) ||
			(!cpu_accel_user_workload(config.workload) &&
			 (config.user_entry_ip || config.user_stack_top ||
			  config.user_stack_bytes || config.user_image_start ||
			  config.user_image_bytes || config.user_arg ||
			  config.user_escape_ip || config.user_reclaim_page)) ||
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

		mutex_lock(&dev->region_lock);
		attached_count = cpu_accel_region_snapshot(dev);
		if (attached_count == U32_MAX) {
			ret = -EINVAL;
			goto config_unlock;
		}
		if (attached_count !=
		    ((config.workload == CPU_ACCEL_WORKLOAD_USER_REGION ||
		      config.workload == CPU_ACCEL_WORKLOAD_USER_CYCLICTEST ||
		      config.workload == CPU_ACCEL_WORKLOAD_USER_OSLAT) ? 1 : 0)) {
			ret = -EINVAL;
			goto config_unlock;
		}
		dev->config = config;
		dev->configured = true;
		mutex_unlock(&dev->region_lock);

#ifdef CONFIG_X86
		if (cpu_accel_user_workload(config.workload)) {
			struct mm_struct *mm = current->mm;

			mmap_read_lock(current->mm);
			ret = cpu_accel_validate_control_and_regions(dev, mm);
			mmap_read_unlock(current->mm);
			if (ret)
				goto config_fail;
		}
#endif
		ret = cpu_accel_prepare_workload(dev, &config);
		if (ret)
			goto config_fail;

		mutex_lock(&dev->region_lock);
		cpu_accel_region_activate_snapshot(dev);
		mutex_unlock(&dev->region_lock);
		dev->region_set_epoch =
			cpu_accel_region_next_epoch(dev->region_set_epoch);
		cpu_accel_reset_shared(dev);
		dev->shared->cpu = config.cpu;
		dev->shared->flags = config.flags;
		dev->shared->workload = config.workload;
		dev->shared->backend =
			cpu_accel_backend_id_for_workload(config.workload);
		dev->shared->backend_flags =
			cpu_accel_backend_flags_for_workload(config.workload);
		dev->shared->region_count = attached_count;
		dev->shared->region_set_epoch = dev->region_set_epoch;
		dev->shared->duration_ns = config.duration_ns;
		dev->shared->period_ns = config.period_ns;
		break;

config_unlock:
		cpu_accel_region_clear_snapshot(dev, false);
		mutex_unlock(&dev->region_lock);
		break;

config_fail:
		mutex_lock(&dev->region_lock);
		cpu_accel_region_clear_snapshot(dev, false);
		mutex_unlock(&dev->region_lock);
		dev->configured = false;
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

	case CPU_ACCEL_IOC_USER_EXIT:
		ret = cpu_accel_user_exit_locked(dev);
		break;

	case CPU_ACCEL_IOC_GET_CAPABILITIES:
		capabilities = (struct cpu_accel_capabilities){
			.max_region_count = cpu_accel_region_count_limit(),
			.max_region_bytes = cpu_accel_max_region_bytes,
			.max_total_region_bytes =
				cpu_accel_max_total_region_bytes,
			.supported_region_permissions =
				CPU_ACCEL_REGION_PERM_BIDIRECTIONAL |
				CPU_ACCEL_REGION_PERM_MANAGER_TO_OWNER |
				CPU_ACCEL_REGION_PERM_OWNER_TO_MANAGER,
		};
		if (copy_to_user((void __user *)argument, &capabilities,
				 sizeof(capabilities)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_REGION_CREATE:
		if (copy_from_user(&region_create, (void __user *)argument,
				   sizeof(region_create))) {
			ret = -EFAULT;
			break;
		}
		ret = cpu_accel_region_create_locked(dev, &region_create);
		if (!ret && copy_to_user((void __user *)argument,
					 &region_create,
					 sizeof(region_create)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_REGION_ATTACH:
	case CPU_ACCEL_IOC_REGION_DETACH:
		if (copy_from_user(&region_ref, (void __user *)argument,
				   sizeof(region_ref))) {
			ret = -EFAULT;
			break;
		}
		if (command == CPU_ACCEL_IOC_REGION_ATTACH)
			ret = cpu_accel_region_attach_locked(dev, &region_ref);
		else
			ret = cpu_accel_region_detach_locked(dev, &region_ref);
		if (!ret && copy_to_user((void __user *)argument,
					 &region_ref, sizeof(region_ref)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_REGION_DESTROY:
		if (copy_from_user(&region_ref, (void __user *)argument,
				   sizeof(region_ref))) {
			ret = -EFAULT;
			break;
		}
		ret = cpu_accel_region_destroy_locked(dev, &region_ref);
		break;

	case CPU_ACCEL_IOC_REGION_GET_INFO:
		if (copy_from_user(&region_info, (void __user *)argument,
				   sizeof(region_info))) {
			ret = -EFAULT;
			break;
		}
		ret = cpu_accel_region_info_locked(dev, &region_info);
		if (!ret && copy_to_user((void __user *)argument,
					 &region_info,
					 sizeof(region_info)))
			ret = -EFAULT;
		break;

	case CPU_ACCEL_IOC_RESET:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread || dev->user_image.active) {
			ret = -EBUSY;
			break;
		}
		ret = cpu_accel_free_workload(dev);
		if (ret)
			break;
		cpu_accel_region_detach_all(dev);
		dev->configured = false;
		dev->sequence = 0;
		dev->region_set_epoch =
			cpu_accel_region_next_epoch(dev->region_set_epoch);
		cpu_accel_reset_shared(dev);
		break;

	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&dev->lock);
	return ret;
}

static __poll_t cpu_accel_poll(struct file *file, poll_table *wait)
{
	struct cpu_accel_device *dev = file->private_data;
	__poll_t events = 0;

	poll_wait(file, &dev->poll_wait, wait);
	if (cpu_accel_state_terminal(READ_ONCE(dev->shared->state)))
		events |= EPOLLIN | EPOLLRDNORM;
	return events;
}

static const struct file_operations cpu_accel_fops = {
	.owner		= THIS_MODULE,
	.open		= cpu_accel_open,
	.release	= cpu_accel_release,
	.mmap		= cpu_accel_mmap,
	.unlocked_ioctl	= cpu_accel_ioctl,
	.poll		= cpu_accel_poll,
};

static int __init cpu_accel_init(void)
{
	int ret;

#ifdef CONFIG_X86
	/* Xen uses different IPI vectors and TLB-flush paths. */
	if (xen_domain()) {
		pr_err("cpu_accel: Xen guests are unsupported by the native x86 "
		       "backend\n");
		return -EOPNOTSUPP;
	}
#endif

	BUILD_BUG_ON(sizeof(struct cpu_accel_shared) > CPU_ACCEL_MAP_SIZE);

	mutex_init(&cpu_accel.lock);
	mutex_init(&cpu_accel.region_lock);
	init_waitqueue_head(&cpu_accel.poll_wait);
	cpu_accel.selected_abi = CPU_ACCEL_ABI_MIN_VERSION;
	init_completion(&cpu_accel.lifecycle_done);
	atomic_set(&cpu_accel.opened, 0);
	atomic_set(&cpu_accel.enter_requested, 0);
	atomic_set(&cpu_accel.stop_requested, 0);
	atomic_set(&cpu_accel.watchdog_fired, 0);
	atomic_set(&cpu_accel.user_escape_requested, 0);
	atomic_set(&cpu_accel.user_escape_seen, 0);
	atomic_set(&cpu_accel.running, 0);
	atomic64_set(&cpu_accel.workqueue_queued, 0);
	atomic64_set(&cpu_accel.workqueue_executed, 0);
	atomic64_set(&cpu_accel.user_escape_count, 0);
	timer_setup(&cpu_accel.watchdog_timer, cpu_accel_watchdog, 0);
#ifdef CONFIG_X86_LOCAL_APIC
	timer_setup(&cpu_accel.stop_escalation_timer, cpu_accel_stop_escalation,
		   0);
#endif
#ifdef CONFIG_X86
	INIT_WORK(&cpu_accel.owner_death_work, cpu_accel_owner_death_work);
#endif
	cpu_accel.shared = vmalloc_user(CPU_ACCEL_MAP_SIZE);
	if (!cpu_accel.shared)
		return -ENOMEM;
	cpu_accel_reset_shared(&cpu_accel);
#ifdef CONFIG_X86_LOCAL_APIC
	ret = register_nmi_handler(NMI_LOCAL, cpu_accel_user_nmi,
				   NMI_FLAG_FIRST, "cpu_accel");
	if (ret) {
		vfree(cpu_accel.shared);
		return ret;
	}
#endif
	ret = cpu_accel_register_tracepoints();
	if (ret) {
#ifdef CONFIG_X86_LOCAL_APIC
		unregister_nmi_handler(NMI_LOCAL, "cpu_accel");
		timer_delete(&cpu_accel.stop_escalation_timer);
#endif
		vfree(cpu_accel.shared);
		return ret;
	}

	cpu_accel.misc.minor = MISC_DYNAMIC_MINOR;
	cpu_accel.misc.name = "cpu_accel";
	cpu_accel.misc.fops = &cpu_accel_fops;
	cpu_accel.misc.mode = 0600;
	ret = misc_register(&cpu_accel.misc);
	if (ret) {
		cpu_accel_unregister_tracepoints();
#ifdef CONFIG_X86_LOCAL_APIC
		unregister_nmi_handler(NMI_LOCAL, "cpu_accel");
		timer_delete(&cpu_accel.stop_escalation_timer);
#endif
		vfree(cpu_accel.shared);
		return ret;
	}

	pr_info("single-CPU accelerator lifecycle prototype loaded\n");
	return 0;
}

static void __exit cpu_accel_exit(void)
{
	/*
	 * Stop the death probe before draining the death work: the work
	 * takes the device lock and must not outlive the module data.
	 */
	cpu_accel_unregister_tracepoints();
#ifdef CONFIG_X86
	flush_work(&cpu_accel.owner_death_work);
#endif
	mutex_lock(&cpu_accel.lock);
	if (READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&cpu_accel.stop_requested, 1);
		WRITE_ONCE(cpu_accel.shared->stop_requested, 1);
	}
	if (cpu_accel.lifecycle_thread)
		cpu_accel_lifecycle_exit(&cpu_accel);
	timer_delete_sync(&cpu_accel.watchdog_timer);
#ifdef CONFIG_X86_LOCAL_APIC
	timer_delete_sync(&cpu_accel.stop_escalation_timer);
#endif
	cpu_accel_free_workload(&cpu_accel);
	cpu_accel_region_detach_all(&cpu_accel);
	cpu_accel_regions_release(&cpu_accel);
	mutex_unlock(&cpu_accel.lock);

	misc_deregister(&cpu_accel.misc);
#ifdef CONFIG_X86_LOCAL_APIC
	unregister_nmi_handler(NMI_LOCAL, "cpu_accel");
#endif
	vfree(cpu_accel.shared);
	cpu_accel.shared = NULL;
}

module_init(cpu_accel_init);
module_exit(cpu_accel_exit);

MODULE_DESCRIPTION("Single-CPU accelerator lifecycle foundation prototype");
MODULE_LICENSE("GPL");
