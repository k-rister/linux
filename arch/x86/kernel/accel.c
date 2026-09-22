// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/smp.h>

#include <asm/apic.h>
#include <asm/cpu_accel.h>
#include <asm/idtentry.h>
#include <asm/irq_vectors.h>

struct x86_cpu_accel_request {
	x86_cpu_accel_entry_fn entry;
	void *data;
	atomic_t active;
	atomic_t done;
};

static DEFINE_PER_CPU(struct x86_cpu_accel_request, x86_cpu_accel_request);

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry, void *data)
{
	struct x86_cpu_accel_request *request;

	if (!entry || cpu >= nr_cpu_ids || !cpu_online(cpu) ||
	    cpu == raw_smp_processor_id())
		return -EINVAL;

	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	if (atomic_cmpxchg(&request->active, 0, 1))
		return -EBUSY;

	atomic_set(&request->done, 0);
	WRITE_ONCE(request->data, data);
	/* Publish the callback before the target can take the vector. */
	smp_store_release(&request->entry, entry);
	__apic_send_IPI(cpu, CPU_ACCEL_VECTOR);

	/* The lifecycle callback owns the target until it returns. */
	while (!atomic_read_acquire(&request->done))
		cpu_relax();

	atomic_set(&request->active, 0);
	return 0;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_direct_enter);

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
