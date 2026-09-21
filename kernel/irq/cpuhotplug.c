// SPDX-License-Identifier: GPL-2.0
/*
 * Generic cpu hotunplug interrupt migration code copied from the
 * arch/arm implementation
 *
 * Copyright (C) Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/ratelimit.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/sched/isolation.h>
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
	if (cpu < nr_cpu_ids)
		atomic_set(per_cpu_ptr(&irq_accel_reserved, cpu), 0);
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

	if (!desc)
		return -EINVAL;

	data = irq_desc_get_irq_data(desc);
	effective = irq_data_get_effective_affinity_mask(data);
	cpu = cpumask_first_and(effective, cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return -ENODEV;

	if (cpu == smp_processor_id())
		irq_accel_set_affinity_on_cpu(&request);
	else if (smp_call_function_single(cpu, irq_accel_set_affinity_on_cpu,
						 &request, true))
		return -ENODEV;

	return request.ret;
}

static int irq_accel_restore_entries(struct irq_accel_quarantine *quarantine)
{
	struct irq_accel_quarantine_entry *entry;
	int first_error = 0;

	list_for_each_entry(entry, &quarantine->entries, node) {
		struct irq_desc *desc = irq_to_desc(entry->irq);
		int ret;

		/* Do not restore an IRQ while an earlier handler is still active. */
		synchronize_irq(entry->irq);
		ret = desc ? irq_accel_set_affinity(desc, entry->affinity, true) :
			-EINVAL;
		if (ret && !first_error)
			first_error = ret;
	}

	return first_error;
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

		desc = irq_to_desc(irq);
		data = irq_desc_get_irq_data(desc);
		affinity = irq_data_get_affinity_mask(data);
		if (!irqd_is_started(data) ||
		    !cpumask_test_cpu(cpu, affinity))
			continue;

		chip = irq_data_get_irq_chip(data);
		if (!irqd_can_balance(data) || !chip ||
		    !chip->irq_set_affinity ||
		    irqd_is_setaffinity_pending(data)) {
			if (blocked)
				(*blocked)++;
			pr_warn_ratelimited("cpu_accel: IRQ %u blocks CPU %u quarantine\n",
					    irq, cpu);
			ret = -EBUSY;
			break;
		}

		entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
		if (!entry || !zalloc_cpumask_var(&entry->affinity, GFP_ATOMIC)) {
			if (entry) {
				free_cpumask_var(entry->affinity);
				kfree(entry);
			}
			ret = -ENOMEM;
			break;
		}
		entry->irq = irq;
		cpumask_copy(entry->affinity, affinity);

		list_add_tail(&entry->node, &state->entries);
		ret = irq_accel_set_affinity(desc, state->destination, false);
		effective = irq_data_get_effective_affinity_mask(data);
		if (ret || cpumask_test_cpu(cpu, affinity) ||
		    cpumask_test_cpu(cpu, effective)) {
			pr_warn_ratelimited(
				"cpu_accel: IRQ %u could not leave CPU %u (ret=%d)\n",
				irq, cpu, ret);
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

/* For !GENERIC_IRQ_EFFECTIVE_AFF_MASK this looks at general affinity mask */
static inline bool irq_needs_fixup(struct irq_data *d)
{
	const struct cpumask *m = irq_data_get_effective_affinity_mask(d);
	unsigned int cpu = smp_processor_id();

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	/*
	 * The cpumask_empty() check is a workaround for interrupt chips,
	 * which do not implement effective affinity, but the architecture has
	 * enabled the config switch. Use the general affinity mask instead.
	 */
	if (cpumask_empty(m))
		m = irq_data_get_affinity_mask(d);

	/*
	 * Sanity check. If the mask is not empty when excluding the outgoing
	 * CPU then it must contain at least one online CPU. The outgoing CPU
	 * has been removed from the online mask already.
	 */
	if (cpumask_any_but(m, cpu) < nr_cpu_ids &&
	    !cpumask_intersects(m, cpu_online_mask)) {
		/*
		 * If this happens then there was a missed IRQ fixup at some
		 * point. Warn about it and enforce fixup.
		 */
		pr_warn("Eff. affinity %*pbl of IRQ %u contains only offline CPUs after offlining CPU %u\n",
			cpumask_pr_args(m), d->irq, cpu);
		return true;
	}
#endif
	return cpumask_test_cpu(cpu, m);
}

static bool migrate_one_irq(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(d);
	bool maskchip = !irq_can_move_pcntxt(d) && !irqd_irq_masked(d);
	const struct cpumask *affinity;
	bool brokeaff = false;
	int err;

	/*
	 * IRQ chip might be already torn down, but the irq descriptor is
	 * still in the radix tree. Also if the chip has no affinity setter,
	 * nothing can be done here.
	 */
	if (!chip || !chip->irq_set_affinity) {
		pr_debug("IRQ %u: Unable to migrate away\n", d->irq);
		return false;
	}

	/*
	 * Complete an eventually pending irq move cleanup. If this
	 * interrupt was moved in hard irq context, then the vectors need
	 * to be cleaned up. It can't wait until this interrupt actually
	 * happens and this CPU was involved.
	 */
	irq_force_complete_move(desc);

	/*
	 * No move required, if:
	 * - Interrupt is per cpu
	 * - Interrupt is not started
	 * - Affinity mask does not include this CPU.
	 *
	 * Note: Do not check desc->action as this might be a chained
	 * interrupt.
	 */
	if (irqd_is_per_cpu(d) || !irqd_is_started(d) || !irq_needs_fixup(d)) {
		/*
		 * If an irq move is pending, abort it if the dying CPU is
		 * the sole target.
		 */
		irq_fixup_move_pending(desc, false);
		return false;
	}

	/*
	 * If there is a setaffinity pending, then try to reuse the pending
	 * mask, so the last change of the affinity does not get lost. If
	 * there is no move pending or the pending mask does not contain
	 * any online CPU, use the current affinity mask.
	 */
	if (irq_fixup_move_pending(desc, true))
		affinity = irq_desc_get_pending_mask(desc);
	else
		affinity = irq_data_get_affinity_mask(d);

	/* Mask the chip for interrupts which cannot move in process context */
	if (maskchip && chip->irq_mask)
		chip->irq_mask(d);

	if (!cpumask_intersects(affinity, cpu_online_mask)) {
		/*
		 * If the interrupt is managed, then shut it down and leave
		 * the affinity untouched.
		 */
		if (irqd_affinity_is_managed(d)) {
			irqd_set_managed_shutdown(d);
			irq_shutdown_and_deactivate(desc);
			return false;
		}
		affinity = cpu_online_mask;
		brokeaff = true;
	}
	/*
	 * Do not set the force argument of irq_do_set_affinity() as this
	 * disables the masking of offline CPUs from the supplied affinity
	 * mask and therefore might keep/reassign the irq to the outgoing
	 * CPU.
	 */
	err = irq_do_set_affinity(d, affinity, false);

	/*
	 * If there are online CPUs in the affinity mask, but they have no
	 * vectors left to make the migration work, try to break the
	 * affinity by migrating to any online CPU.
	 */
	if (err == -ENOSPC && !irqd_affinity_is_managed(d) && affinity != cpu_online_mask) {
		pr_debug("IRQ%u: set affinity failed for %*pbl, re-try with online CPUs\n",
			 d->irq, cpumask_pr_args(affinity));

		affinity = cpu_online_mask;
		brokeaff = true;

		err = irq_do_set_affinity(d, affinity, false);
	}

	if (err) {
		pr_warn_ratelimited("IRQ%u: set affinity failed(%d).\n",
				    d->irq, err);
		brokeaff = false;
	}

	if (maskchip && chip->irq_unmask)
		chip->irq_unmask(d);

	return brokeaff;
}

/**
 * irq_migrate_all_off_this_cpu - Migrate irqs away from offline cpu
 *
 * The current CPU has been marked offline.  Migrate IRQs off this CPU.
 * If the affinity settings do not allow other CPUs, force them onto any
 * available CPU.
 *
 * Note: we must iterate over all IRQs, whether they have an attached
 * action structure or not, as we need to get chained interrupts too.
 */
void irq_migrate_all_off_this_cpu(void)
{
	struct irq_desc *desc;
	unsigned int irq;

	for_each_active_irq(irq) {
		bool affinity_broken;

		desc = irq_to_desc(irq);
		scoped_guard(raw_spinlock, &desc->lock) {
			affinity_broken = migrate_one_irq(desc);
			if (affinity_broken && desc->affinity_notify)
				irq_affinity_schedule_notify_work(desc);
		}
		if (affinity_broken) {
			pr_debug_ratelimited("IRQ %u: no longer affine to CPU%u\n",
					    irq, smp_processor_id());
		}
	}
}

static bool hk_should_isolate(struct irq_data *data, unsigned int cpu)
{
	const struct cpumask *hk_mask;

	if (!housekeeping_enabled(HK_TYPE_MANAGED_IRQ))
		return false;

	hk_mask = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ);
	if (cpumask_subset(irq_data_get_effective_affinity_mask(data), hk_mask))
		return false;

	return cpumask_test_cpu(cpu, hk_mask);
}

static void irq_restore_affinity_of_irq(struct irq_desc *desc, unsigned int cpu)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	const struct cpumask *affinity = irq_data_get_affinity_mask(data);

	if (!irqd_affinity_is_managed(data) || !desc->action ||
	    !irq_data_get_irq_chip(data) || !cpumask_test_cpu(cpu, affinity))
		return;

	if (irqd_is_managed_and_shutdown(data))
		irq_startup_managed(desc);

	/*
	 * If the interrupt can only be directed to a single target
	 * CPU then it is already assigned to a CPU in the affinity
	 * mask. No point in trying to move it around unless the
	 * isolation mechanism requests to move it to an upcoming
	 * housekeeping CPU.
	 */
	if (!irqd_is_single_target(data) || hk_should_isolate(data, cpu))
		irq_set_affinity_locked(data, affinity, false);
}

/**
 * irq_affinity_online_cpu - Restore affinity for managed interrupts
 * @cpu:	Upcoming CPU for which interrupts should be restored
 */
int irq_affinity_online_cpu(unsigned int cpu)
{
	struct irq_desc *desc;
	unsigned int irq;

	irq_lock_sparse();
	for_each_active_irq(irq) {
		desc = irq_to_desc(irq);
		scoped_guard(raw_spinlock_irq, &desc->lock)
			irq_restore_affinity_of_irq(desc, cpu);
	}
	irq_unlock_sparse();

	return 0;
}
