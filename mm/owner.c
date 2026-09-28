// SPDX-License-Identifier: GPL-2.0
/*
 * MM execution-owner registry.
 *
 * Some architectures can execute an address space outside the task that
 * normally owns it.  Keep those owners separate from mm_cpumask(): the latter
 * describes CPUs that may cache translations, not execution ownership.
 */

#include <linux/mm.h>
#include <linux/mmu_owner.h>
#include <linux/sched/mm.h>

void mmu_owner_init(struct mmu_owner *owner)
{
	raw_spin_lock_init(&owner->lock);
	INIT_LIST_HEAD(&owner->link);
	owner->mm = NULL;
	owner->data = NULL;
	owner->generation = 0;
	owner->pending_tlb_gen = 0;
}

int mmu_owner_register(struct mm_struct *mm, struct mmu_owner *owner,
		       void *data, u64 generation)
{
	unsigned long flags;
	unsigned long owner_flags;
	int ret = 0;

	if (WARN_ON_ONCE(!mm || !owner))
		return -EINVAL;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	if (owner->mm) {
		ret = -EBUSY;
		goto out_owner;
	}

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	if (mm->execution_owner_update_depth) {
		ret = -EAGAIN;
	} else {
		INIT_LIST_HEAD(&owner->link);
		owner->mm = mm;
		owner->data = data;
		owner->generation = generation;
		owner->pending_tlb_gen = 0;
		mmgrab(mm);
		list_add_tail(&owner->link, &mm->execution_owners);
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	return ret;
}

u64 mmu_owner_unregister(struct mmu_owner *owner)
{
	struct mm_struct *mm;
	unsigned long flags;
	unsigned long owner_flags;
	u64 tlb_gen = 0;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	mm = owner->mm;
	if (!mm)
		goto out_owner;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	tlb_gen = owner->pending_tlb_gen;
	list_del_init(&owner->link);
	owner->mm = NULL;
	owner->data = NULL;
	owner->generation = 0;
	owner->pending_tlb_gen = 0;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	mmdrop(mm);
	return tlb_gen;

out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	return 0;
}

void mmu_owner_update_tlb_gen(struct mm_struct *mm, u64 tlb_gen)
{
	struct mmu_owner *owner;
	unsigned long flags;

	if (WARN_ON_ONCE(!mm))
		return;
	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	list_for_each_entry(owner, &mm->execution_owners, link)
		if (tlb_gen > owner->pending_tlb_gen)
			owner->pending_tlb_gen = tlb_gen;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
}

void mmu_owner_update_one_tlb_gen(struct mmu_owner *owner, u64 tlb_gen)
{
	struct mm_struct *mm;
	unsigned long flags;
	unsigned long owner_flags;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	mm = owner->mm;
	if (!mm)
		goto out_owner;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	if (tlb_gen > owner->pending_tlb_gen)
		owner->pending_tlb_gen = tlb_gen;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
}

int mmu_owner_snapshot(struct mm_struct *mm, u64 tlb_gen,
		       struct mmu_owner_snapshot *snapshot, unsigned int capacity)
{
	struct mmu_owner *owner;
	unsigned int nr = 0;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	list_for_each_entry(owner, &mm->execution_owners, link) {
		if (owner->pending_tlb_gen < tlb_gen)
			continue;
		if (nr == capacity) {
			ret = -EOVERFLOW;
			break;
		}
		snapshot[nr].data = owner->data;
		snapshot[nr].generation = owner->generation;
		nr++;
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);

	return ret ?: nr;
}

void mmu_owner_update_begin(struct mm_struct *mm)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	mm->execution_owner_update_depth++;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
}

void mmu_owner_update_end(struct mm_struct *mm)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	if (WARN_ON_ONCE(!mm->execution_owner_update_depth)) {
		raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
		return;
	}
	mm->execution_owner_update_depth--;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
}

bool mmu_owner_update_inflight(struct mm_struct *mm)
{
	return READ_ONCE(mm->execution_owner_update_depth) != 0;
}
