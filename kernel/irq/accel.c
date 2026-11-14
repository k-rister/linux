// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irq_accel.h>
#include <linux/list.h>
#include <linux/sched/isolation.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include "internals.h"

/* CPUs owned by an accelerator must not receive new IRQ affinity requests. */
static DEFINE_PER_CPU(atomic_t, irq_accel_reserved);

bool irq_accel_cpu_reserved(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return false;
	return atomic_read(per_cpu_ptr(&irq_accel_reserved, cpu));
}

bool irq_accel_affinity_allowed(const struct cpumask *mask)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask) {
		if (irq_accel_cpu_reserved(cpu))
			return false;
	}
	return true;
}

int irq_accel_reserve_cpu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return -EINVAL;
	if (atomic_cmpxchg(per_cpu_ptr(&irq_accel_reserved, cpu), 0, 1))
		return -EBUSY;
	return 0;
}

void irq_accel_release_cpu(unsigned int cpu)
{
	if (cpu >= nr_cpu_ids)
		return;

	atomic_set(per_cpu_ptr(&irq_accel_reserved, cpu), 0);
	/* Restart managed IRQs that had no unreserved target while owned. */
	irq_affinity_online_cpu(cpu);
}

struct irq_accel_quarantine_entry {
	struct list_head node;
	unsigned int irq;
	cpumask_var_t affinity;
};

struct irq_accel_quarantine {
	unsigned int cpu;
	cpumask_var_t destination;
	struct list_head entries;
};

static void irq_accel_free_quarantine(struct irq_accel_quarantine *quarantine)
{
	struct irq_accel_quarantine_entry *entry, *next;

	list_for_each_entry_safe(entry, next, &quarantine->entries, node) {
		list_del(&entry->node);
		free_cpumask_var(entry->affinity);
		kfree(entry);
	}
	free_cpumask_var(quarantine->destination);
	kfree(quarantine);
}

struct irq_accel_affinity_request {
	struct irq_desc *desc;
	const struct cpumask *mask;
	bool force;
	int ret;
};

static void irq_accel_set_affinity_on_cpu(void *arg)
{
	struct irq_accel_affinity_request *request = arg;
	struct irq_desc *desc = request->desc;
	struct irq_data *data = irq_desc_get_irq_data(desc);
	const struct cpumask *effective;
	unsigned int owner;
	unsigned long flags;

	/*
	 * x86 MSI vector moves which change the destination must run on the
	 * CPU currently owning the vector.  Use the same pending-affinity
	 * protocol as the generic IRQ core, then complete the deferred move
	 * while executing on that CPU.
	 */
	raw_spin_lock_irqsave(&desc->lock, flags);
	irq_force_complete_move(desc);
	if (irqd_is_setaffinity_pending(data))
		irq_move_irq(data);
	effective = irq_data_get_effective_affinity_mask(data);
	owner = cpumask_first_and(effective, cpu_online_mask);
	if (owner != smp_processor_id()) {
		request->ret = -EAGAIN;
		goto out;
	}
	if (irqd_is_setaffinity_pending(data)) {
		request->ret = -EBUSY;
		goto out;
	}
	request->ret = irq_set_affinity_locked(data, request->mask,
					       request->force);
	if (!request->ret)
		irq_move_irq(data);
out:
	raw_spin_unlock_irqrestore(&desc->lock, flags);
}

static int irq_accel_set_affinity(struct irq_desc *desc,
				  const struct cpumask *mask, bool force)
{
	struct irq_accel_affinity_request request = {
		.desc = desc,
		.mask = mask,
		.force = force,
		.ret = -EAGAIN,
	};
	struct irq_data *data;
	const struct cpumask *effective;
	unsigned int cpu;
	unsigned int attempt;

	if (!desc)
		return -EINVAL;

	data = irq_desc_get_irq_data(desc);
	for (attempt = 0; attempt <= nr_cpu_ids; attempt++) {
		effective = irq_data_get_effective_affinity_mask(data);
		cpu = cpumask_first_and(effective, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			return -ENODEV;
		request.ret = -EAGAIN;

		if (cpu == smp_processor_id())
			irq_accel_set_affinity_on_cpu(&request);
		else if (smp_call_function_single(cpu, irq_accel_set_affinity_on_cpu,
						  &request, true))
			return -ENODEV;

		if (request.ret != -EAGAIN)
			return request.ret;
	}

	return -EBUSY;
}

static int irq_accel_restore_entries(struct irq_accel_quarantine *quarantine)
{
	struct irq_accel_quarantine_entry *entry;
	cpumask_var_t restore_mask;
	int first_error = 0;

	if (!zalloc_cpumask_var(&restore_mask, GFP_KERNEL))
		return -ENOMEM;

	list_for_each_entry(entry, &quarantine->entries, node) {
		struct irq_desc *desc = irq_to_desc(entry->irq);
		int ret;

		/* Do not restore an IRQ while an earlier handler is still active. */
		synchronize_irq(entry->irq);
		/*
		 * Force is needed to restore offline affinity bits. The reserved
		 * target is safe here: admission has not completed, or its owner
		 * has already stopped during teardown.
		 */
		irq_accel_filter_restore_affinity(restore_mask,
						  entry->affinity,
						  quarantine->cpu);
		if (cpumask_empty(restore_mask))
			ret = -ENOSPC;
		else if (desc)
			ret = irq_accel_set_affinity(desc, restore_mask, true);
		else
			ret = -EINVAL;
		if (ret && !first_error)
			first_error = ret;
	}

	free_cpumask_var(restore_mask);
	return first_error;
}

static bool irq_accel_effective_targets_cpu(struct irq_data *data,
						unsigned int cpu,
						const struct cpumask *affinity)
{
	const struct cpumask *effective =
		irq_data_get_effective_affinity_mask(data);

	/* Some interrupt chips do not provide an effective affinity mask. */
	if (cpumask_empty(effective))
		return cpumask_test_cpu(cpu, affinity);
	return cpumask_test_cpu(cpu, effective);
}

/**
 * irq_accel_quarantine_cpu - move active migratable IRQs away from a CPU
 * @cpu: CPU to reserve
 * @quarantine: returned rollback state
 * @migrated: returned number of moved IRQs
 * @blocked: returned number of non-migratable IRQ blockers
 *
 * This is an online-CPU ownership transition helper.  It deliberately fails
 * closed when an active IRQ targets @cpu but cannot be moved through the IRQ
 * core.  The caller must hold the CPU hotplug read lock while using the
 * returned state.
 */
int irq_accel_quarantine_cpu(unsigned int cpu,
				     struct irq_accel_quarantine **quarantine,
				     unsigned int *migrated,
				     unsigned int *blocked)
{
	struct irq_accel_quarantine *state;
	struct irq_accel_quarantine_entry *entry;
	struct irq_desc *desc;
	unsigned int irq;
	unsigned int other;
	int ret;

	if (!quarantine)
		return -EINVAL;
	*quarantine = NULL;
	if (migrated)
		*migrated = 0;
	if (blocked)
		*blocked = 0;
	if (!cpu_online(cpu))
		return -ENODEV;

	ret = irq_accel_reserve_cpu(cpu);
	if (ret)
		return ret;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state) {
		ret = -ENOMEM;
		goto fail_release;
	}
	state->cpu = cpu;
	INIT_LIST_HEAD(&state->entries);
	if (!zalloc_cpumask_var(&state->destination, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto fail_free;
	}

	cpumask_copy(state->destination, cpu_online_mask);
	cpumask_clear_cpu(cpu, state->destination);
	for (other = 0; other < nr_cpu_ids; other++)
		if (irq_accel_cpu_reserved(other))
			cpumask_clear_cpu(other, state->destination);
	if (cpumask_empty(state->destination)) {
		ret = -ENOSPC;
		goto fail_free;
	}

	irq_lock_sparse();
	for_each_active_irq(irq) {
		struct irq_data *data;
		struct irq_chip *chip;
		const struct cpumask *affinity;
		const struct cpumask *effective;
		struct cpumask effective_mask;
		unsigned long flags;
		bool pending;

		desc = irq_to_desc(irq);
		raw_spin_lock_irqsave(&desc->lock, flags);
		data = irq_desc_get_irq_data(desc);
		affinity = irq_data_get_affinity_mask(data);
		if (!irqd_is_started(data) ||
		    !irq_accel_effective_targets_cpu(data, cpu, affinity)) {
			raw_spin_unlock_irqrestore(&desc->lock, flags);
			continue;
		}

		chip = irq_data_get_irq_chip(data);
		if (irqd_affinity_is_managed(data) ||
		    !irqd_can_balance(data) || !chip || !chip->irq_set_affinity) {
			if (blocked)
				(*blocked)++;
			pr_warn_ratelimited(
				"cpu_accel: IRQ %u blocks CPU %u quarantine "
				"requested=%*pbl effective=%*pbl managed=%u balance=%u "
				"chip=%s set_affinity=%ps pending=%u\n", irq, cpu,
				cpumask_pr_args(affinity),
				cpumask_pr_args(irq_data_get_effective_affinity_mask(data)),
				irqd_affinity_is_managed(data), irqd_can_balance(data),
				chip ? chip->name : "none",
				chip ? chip->irq_set_affinity : NULL,
				irqd_is_setaffinity_pending(data));
			raw_spin_unlock_irqrestore(&desc->lock, flags);
			ret = -EBUSY;
			break;
		}

		entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
		if (!entry || !zalloc_cpumask_var(&entry->affinity, GFP_ATOMIC)) {
			if (entry) {
				free_cpumask_var(entry->affinity);
				kfree(entry);
			}
			raw_spin_unlock_irqrestore(&desc->lock, flags);
			ret = -ENOMEM;
			break;
		}
		entry->irq = irq;
		cpumask_copy(entry->affinity, affinity);

		list_add_tail(&entry->node, &state->entries);
		raw_spin_unlock_irqrestore(&desc->lock, flags);
		ret = irq_accel_set_affinity(desc, state->destination, false);
		raw_spin_lock_irqsave(&desc->lock, flags);
		effective = irq_data_get_effective_affinity_mask(data);
		cpumask_copy(&effective_mask, effective);
		pending = irqd_is_setaffinity_pending(data);
		raw_spin_unlock_irqrestore(&desc->lock, flags);
		if (ret || pending || cpumask_test_cpu(cpu, &effective_mask)) {
			pr_warn_ratelimited(
				"cpu_accel: IRQ %u could not leave CPU %u (ret=%d "
				"requested=%*pbl effective=%*pbl pending=%u)\n", irq,
				cpu, ret, cpumask_pr_args(entry->affinity),
				cpumask_pr_args(&effective_mask), pending);
			if (!ret)
				ret = -EBUSY;
			break;
		}

		if (migrated)
			(*migrated)++;
	}
	irq_unlock_sparse();
	if (ret)
		goto fail_restore;

	/* Drain handlers that were already in flight before entry ownership. */
	list_for_each_entry(entry, &state->entries, node)
		synchronize_irq(entry->irq);

	*quarantine = state;
	return 0;

fail_restore:
	{
		int rollback_ret = irq_accel_restore_entries(state);

		if (rollback_ret) {
			pr_err("cpu_accel: IRQ quarantine rollback for CPU %u was incomplete\n",
			       cpu);
			*quarantine = state;
			return rollback_ret;
		}
	}
fail_free:
	irq_accel_free_quarantine(state);
fail_release:
	irq_accel_release_cpu(cpu);
	return ret;
}
EXPORT_SYMBOL_GPL(irq_accel_quarantine_cpu);

/**
 * irq_accel_restore_cpu - restore IRQ affinity and release CPU ownership
 * @quarantine: state returned by irq_accel_quarantine_cpu()
 *
 * The caller must ensure that the owner of @quarantine->cpu has stopped or
 * that admission failed. Other accelerator-owned CPUs are filtered from the
 * restored affinity masks because they may still have active owners.
 */
int irq_accel_restore_cpu(struct irq_accel_quarantine *quarantine)
{
	int ret;

	if (!quarantine)
		return 0;
	ret = irq_accel_restore_entries(quarantine);
	if (ret)
		return ret;
	irq_accel_release_cpu(quarantine->cpu);
	irq_accel_free_quarantine(quarantine);
	return ret;
}
EXPORT_SYMBOL_GPL(irq_accel_restore_cpu);
