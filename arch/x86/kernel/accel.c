// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/apic.h>
#include <asm/cpu_accel.h>
#include <asm/idtentry.h>
#include <asm/irq_vectors.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

struct x86_cpu_accel_request {
	raw_spinlock_t lock;
	x86_cpu_accel_entry_fn entry;
	void *data;
	atomic_t active;
	atomic_t done;
	atomic_t reschedule_pending;
	atomic64_t reschedule_deferred;
	atomic_t call_function_pending;
	atomic64_t call_function_deferred;
	atomic64_t tlb_shootdown_targets;
	struct mm_struct *owner_mm;
	/* Highest native TLB generation targeting owner_mm during ownership. */
	u64 pending_tlb_gen;
	/* Flushes with no single address-space owner (e.g. kernel/global). */
	bool pending_unscoped_tlb_flush;
};

static DEFINE_PER_CPU(struct x86_cpu_accel_request, x86_cpu_accel_request) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(x86_cpu_accel_request.lock),
};

static DEFINE_PER_CPU(unsigned int, x86_cpu_accel_tlb_flush_count);
static atomic_t x86_cpu_accel_active_count = ATOMIC_INIT(0);
static DEFINE_RAW_SPINLOCK(x86_cpu_accel_ownership_lock);

static int x86_cpu_accel_owner_enter(unsigned int cpu, u64 *tlb_targets,
				     struct mm_struct *owner_mm)
{
	struct x86_cpu_accel_request *request;
	unsigned long ownership_flags;
	unsigned long request_flags;
	bool wrong_cpu;

	preempt_disable();
	wrong_cpu = owner_mm && cpu != raw_smp_processor_id();
	if (wrong_cpu || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		preempt_enable();
		return wrong_cpu ? -EXDEV : -EINVAL;
	}

	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, ownership_flags);
	/* Do not enter a CPU while a TLB flush targets it. */
	if (per_cpu(x86_cpu_accel_tlb_flush_count, cpu)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EBUSY;
	}
	raw_spin_lock_irqsave(&request->lock, request_flags);
	if (atomic_read(&request->active)) {
		raw_spin_unlock_irqrestore(&request->lock, request_flags);
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EBUSY;
	}
	atomic_set(&request->reschedule_pending, 0);
	atomic_set(&request->call_function_pending, 0);
	request->owner_mm = owner_mm;
	request->pending_tlb_gen = 0;
	request->pending_unscoped_tlb_flush = false;
	atomic_inc(&x86_cpu_accel_active_count);
	atomic_set(&request->active, 1);
	if (tlb_targets)
		*tlb_targets = atomic64_read(&request->tlb_shootdown_targets);
	raw_spin_unlock_irqrestore(&request->lock, request_flags);
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
				   ownership_flags);
	preempt_enable();
	return 0;
}

void x86_cpu_accel_tlb_flush_begin(struct x86_cpu_accel_tlb_flush *flush,
				   const struct cpumask *targets)
{
	unsigned int cpu;
	unsigned long flags;

	cpumask_copy(&flush->targets, targets);
	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	flush->no_owners = !atomic_read(&x86_cpu_accel_active_count);
	for_each_cpu(cpu, &flush->targets)
		per_cpu(x86_cpu_accel_tlb_flush_count, cpu)++;
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_tlb_flush_begin);

void x86_cpu_accel_tlb_flush_end(struct x86_cpu_accel_tlb_flush *flush)
{
	unsigned int cpu;
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	for_each_cpu(cpu, &flush->targets) {
		if (WARN_ON_ONCE(!per_cpu(x86_cpu_accel_tlb_flush_count, cpu)))
			continue;
		per_cpu(x86_cpu_accel_tlb_flush_count, cpu)--;
	}
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_tlb_flush_end);

static u64 x86_cpu_accel_owner_exit(unsigned int cpu,
				    bool *local_tlb_flush)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;
	u64 targets;

	if (local_tlb_flush)
		*local_tlb_flush = false;
	if (cpu >= nr_cpu_ids)
		return 0;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_xchg(&request->active, 0)) {
		atomic_dec(&x86_cpu_accel_active_count);
		if (local_tlb_flush)
			*local_tlb_flush = request->pending_unscoped_tlb_flush ||
				request->pending_tlb_gen != 0;
		request->owner_mm = NULL;
		request->pending_tlb_gen = 0;
		request->pending_unscoped_tlb_flush = false;
		if (atomic_xchg(&request->call_function_pending, 0))
			__apic_send_IPI(cpu, CALL_FUNCTION_SINGLE_VECTOR);
		if (atomic_xchg(&request->reschedule_pending, 0))
			__apic_send_IPI(cpu, RESCHEDULE_VECTOR);
	}
	targets = atomic64_read(&request->tlb_shootdown_targets);
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return targets;
}

int x86_cpu_accel_user_enter(unsigned int cpu, struct mm_struct *mm,
			     u64 *tlb_targets)
{
	if (!mm)
		return -EINVAL;
	if (cpu != raw_smp_processor_id())
		return -EXDEV;
	if (mm != current->mm)
		return -EXDEV;
	return x86_cpu_accel_owner_enter(cpu, tlb_targets, mm);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_user_enter);

u64 x86_cpu_accel_user_exit(unsigned int cpu)
{
	bool local_tlb_flush;
	u64 targets = x86_cpu_accel_owner_exit(cpu, &local_tlb_flush);

	/* Reconcile relevant local TLB state before pinned pages are released. */
	if (cpu == raw_smp_processor_id() && local_tlb_flush)
		__flush_tlb_all();
	return targets;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_user_exit);

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry, void *data)
{
	struct x86_cpu_accel_request *request;
	int ret;

	if (!entry || cpu == raw_smp_processor_id())
		return -EINVAL;
	ret = x86_cpu_accel_owner_enter(cpu, NULL, NULL);
	if (ret)
		return ret;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);

	atomic_set(&request->done, 0);
	WRITE_ONCE(request->data, data);
	/* Publish the callback before the target can take the vector. */
	smp_store_release(&request->entry, entry);
	__apic_send_IPI(cpu, CPU_ACCEL_VECTOR);

	/* The lifecycle callback owns the target until it returns. */
	while (!atomic_read_acquire(&request->done))
		cpu_relax();

	x86_cpu_accel_owner_exit(cpu, NULL);
	return 0;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_direct_enter);

bool x86_cpu_accel_defer_reschedule(unsigned int cpu)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;

	if (cpu >= nr_cpu_ids)
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (!atomic_read(&request->active))
		goto out;
	atomic64_inc(&request->reschedule_deferred);
	atomic_set(&request->reschedule_pending, 1);
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return true;

out:
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return false;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_defer_reschedule);

u64 x86_cpu_accel_reschedule_deferred(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return 0;
	return atomic64_read(&per_cpu(x86_cpu_accel_request,
					     cpu).reschedule_deferred);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_reschedule_deferred);

bool x86_cpu_accel_defer_call_function(unsigned int cpu)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;

	if (cpu >= nr_cpu_ids)
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (!atomic_read(&request->active))
		goto out;
	atomic64_inc(&request->call_function_deferred);
	atomic_set(&request->call_function_pending, 1);
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return true;

out:
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return false;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_defer_call_function);

u64 x86_cpu_accel_call_function_deferred(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return 0;
	return atomic64_read(&per_cpu(x86_cpu_accel_request,
					     cpu).call_function_deferred);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_call_function_deferred);

bool x86_cpu_accel_note_tlb_shootdown(unsigned int cpu,
				      const struct mm_struct *mm,
				      u64 tlb_gen)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;
	bool active;

	if (cpu >= nr_cpu_ids || !x86_cpu_accel_any_active())
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	active = atomic_read(&request->active);
	if (active) {
		atomic64_inc(&request->tlb_shootdown_targets);
		/* A NULL mm denotes an address-space-unscoped invalidation. */
		if (!mm)
			request->pending_unscoped_tlb_flush = true;
		else if (request->owner_mm == mm &&
			 tlb_gen > request->pending_tlb_gen)
			request->pending_tlb_gen = tlb_gen;
	}
	raw_spin_unlock_irqrestore(&request->lock, flags);

	return active;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_note_tlb_shootdown);

bool x86_cpu_accel_filter_mm_tlb_shootdown(unsigned int cpu,
					   const struct mm_struct *mm,
					   u64 tlb_gen)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;
	bool filter = false;

	if (cpu >= nr_cpu_ids || !mm || !x86_cpu_accel_any_active())
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_read(&request->active) && request->owner_mm) {
		atomic64_inc(&request->tlb_shootdown_targets);
		if (request->owner_mm == mm &&
		    tlb_gen > request->pending_tlb_gen)
			request->pending_tlb_gen = tlb_gen;
		/*
		 * The ring-3 owner cannot receive the synchronous call-function
		 * TLB callback with interrupts disabled. Its own mm is write-locked
		 * until exit and flushed before unlock; another mm's TLB generation
		 * is checked by switch_mm() before this CPU can use that mm again.
		 */
		filter = true;
	}
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return filter;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_filter_mm_tlb_shootdown);

bool x86_cpu_accel_any_active(void)
{
	return atomic_read(&x86_cpu_accel_active_count) != 0;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_any_active);

u64 x86_cpu_accel_tlb_shootdown_targets(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return 0;
	return atomic64_read(&per_cpu(x86_cpu_accel_request,
					     cpu).tlb_shootdown_targets);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_tlb_shootdown_targets);

DEFINE_IDTENTRY_SYSVEC(sysvec_cpu_accel)
{
	struct x86_cpu_accel_request *request = this_cpu_ptr(
		&x86_cpu_accel_request);
	x86_cpu_accel_entry_fn entry;
	void *data;

	apic_eoi();
	entry = smp_load_acquire(&request->entry);
	if (!entry)
		return;
	data = READ_ONCE(request->data);
	entry(data);
	WRITE_ONCE(request->data, NULL);
	smp_store_release(&request->entry, NULL);
	atomic_set_release(&request->done, 1);
}
