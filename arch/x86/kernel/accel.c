// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/apic.h>
#include <asm/cpu_accel.h>
#include <asm/idtentry.h>
#include <asm/irq_vectors.h>
#include <asm/smp.h>

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
};

static DEFINE_PER_CPU(struct x86_cpu_accel_request, x86_cpu_accel_request) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(x86_cpu_accel_request.lock),
};
static atomic_t x86_cpu_accel_active_count = ATOMIC_INIT(0);

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry, void *data)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;

	if (!entry || cpu >= nr_cpu_ids || !cpu_online(cpu) ||
	    cpu == raw_smp_processor_id())
		return -EINVAL;

	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_read(&request->active)) {
		raw_spin_unlock_irqrestore(&request->lock, flags);
		return -EBUSY;
	}
	atomic_set(&request->reschedule_pending, 0);
	atomic_set(&request->call_function_pending, 0);
	atomic_inc(&x86_cpu_accel_active_count);
	atomic_set(&request->active, 1);
	raw_spin_unlock_irqrestore(&request->lock, flags);

	atomic_set(&request->done, 0);
	WRITE_ONCE(request->data, data);
	/* Publish the callback before the target can take the vector. */
	smp_store_release(&request->entry, entry);
	__apic_send_IPI(cpu, CPU_ACCEL_VECTOR);

	/* The lifecycle callback owns the target until it returns. */
	while (!atomic_read_acquire(&request->done))
		cpu_relax();

	raw_spin_lock_irqsave(&request->lock, flags);
	atomic_set(&request->active, 0);
	atomic_dec(&x86_cpu_accel_active_count);
	if (atomic_xchg(&request->call_function_pending, 0))
		__apic_send_IPI(cpu, CALL_FUNCTION_SINGLE_VECTOR);
	if (atomic_xchg(&request->reschedule_pending, 0))
		__apic_send_IPI(cpu, RESCHEDULE_VECTOR);
	raw_spin_unlock_irqrestore(&request->lock, flags);
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

bool x86_cpu_accel_note_tlb_shootdown(unsigned int cpu)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;
	bool active;

	if (cpu >= nr_cpu_ids || !x86_cpu_accel_any_active())
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	active = atomic_read(&request->active);
	if (active)
		atomic64_inc(&request->tlb_shootdown_targets);
	raw_spin_unlock_irqrestore(&request->lock, flags);

	return active;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_note_tlb_shootdown);

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
