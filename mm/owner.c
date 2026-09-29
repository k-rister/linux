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

/*
 * Lock order: owner->lock before mm->execution_owner_lock. A caller may hold
 * an architecture lock outside these, but must not acquire owner->lock while
 * holding execution_owner_lock.
 */

static bool mmu_owner_get_mm(struct mm_struct *mm)
{
#ifdef CONFIG_MMU
	/* Keep the address space alive, not only the mm_struct allocation. */
	return mmget_not_zero(mm);
#else
	/* NOMMU has no page tables whose lifetime needs an mm_users reference. */
	mmgrab(mm);
	return true;
#endif
}

static void mmu_owner_put_mm(struct mm_struct *mm)
{
#ifdef CONFIG_MMU
	/* Owner exit may run in atomic context; final teardown must be deferred. */
	mmput_async(mm);
#else
	mmdrop(mm);
#endif
}

void mmu_owner_init(struct mmu_owner *owner)
{
	raw_spin_lock_init(&owner->lock);
	INIT_LIST_HEAD(&owner->link);
	INIT_LIST_HEAD(&owner->subscriptions);
	owner->mm = NULL;
	owner->data = NULL;
	owner->ops = NULL;
	owner->generation = 0;
	owner->pending_tlb_gen = 0;
}

int mmu_owner_register(struct mm_struct *mm, struct mmu_owner *owner,
		       void *data, u64 generation,
		       const struct mmu_owner_ops *ops)
{
	unsigned long flags;
	unsigned long owner_flags;
	int ret = 0;

	if (WARN_ON_ONCE(!mm || !owner || !ops || !ops->get ||
			 !ops->put))
		return -EINVAL;
	if (!mmu_owner_get_mm(mm))
		return -ESRCH;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	if (owner->mm) {
		ret = -EBUSY;
		goto out_owner;
	}
	if (WARN_ON_ONCE(!list_empty(&owner->subscriptions))) {
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
		owner->ops = ops;
		owner->generation = generation;
		owner->pending_tlb_gen = 0;
		list_add_tail(&owner->link, &mm->execution_owners);
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	if (ret)
		mmu_owner_put_mm(mm);
	return ret;
}

u64 mmu_owner_unregister(struct mmu_owner *owner,
			 struct list_head *subscriptions)
{
	struct mm_struct *mm;
	unsigned long flags;
	unsigned long owner_flags;
	u64 tlb_gen = 0;

	if (WARN_ON_ONCE(!owner || !subscriptions ||
			 !list_empty(subscriptions)))
		return 0;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	mm = owner->mm;
	if (!mm)
		goto out_owner;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	tlb_gen = owner->pending_tlb_gen;
	list_splice_init(&owner->subscriptions, subscriptions);
	list_del_init(&owner->link);
	owner->mm = NULL;
	owner->data = NULL;
	owner->ops = NULL;
	owner->generation = 0;
	owner->pending_tlb_gen = 0;
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	mmu_owner_put_mm(mm);
	return tlb_gen;

out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	return 0;
}

int mmu_owner_subscribe(struct mmu_owner *owner, u64 generation, u64 tlb_gen,
			struct mmu_owner_subscription *subscription,
			void *data,
			const struct mmu_owner_subscription_ops *ops)
{
	struct mm_struct *mm;
	const struct mmu_owner_ops *owner_ops = NULL;
	void *owner_data = NULL, *stop_data = NULL;
	unsigned long flags;
	unsigned long owner_flags;
	bool owner_pinned = false, stop_pinned = false, data_pinned = false;
	int ret = 0;

	if (WARN_ON_ONCE(!owner || !subscription || !ops || !ops->get ||
			 !ops->complete || !ops->put))
		return -EINVAL;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	mm = owner->mm;
	if (!mm) {
		ret = -ENOENT;
		goto out_owner;
	}

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	if (owner->generation != generation) {
		ret = -ESTALE;
	} else if (!owner->ops || !owner->ops->request_stop) {
		ret = -EOPNOTSUPP;
	} else if (owner->pending_tlb_gen < tlb_gen) {
		ret = -EAGAIN;
	} else {
		owner_ops = owner->ops;
		owner_data = owner->data;
		if (!owner_ops->get(owner_data)) {
			ret = -EAGAIN;
		} else {
			owner_pinned = true;
			if (!owner_ops->get(owner_data)) {
				ret = -EAGAIN;
			} else {
				stop_data = owner_data;
				stop_pinned = true;
			}
		}
		if (!ret) {
			if (!ops->get(data)) {
				ret = -EAGAIN;
			} else {
				data_pinned = true;
			}
		}
		if (!ret) {
			INIT_LIST_HEAD(&subscription->link);
			subscription->owner_data = owner_data;
			subscription->owner_put = owner_ops->put;
			subscription->data = data;
			subscription->ops = ops;
			subscription->generation = generation;
			list_add_tail(&subscription->link,
				      &owner->subscriptions);
		}
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);

	if (ret) {
		if (data_pinned)
			ops->put(data);
		if (stop_pinned)
			owner_ops->put(stop_data);
		if (owner_pinned)
			owner_ops->put(owner_data);
		return ret;
	}

	/* A concurrent unregister may complete the subscription before this call. */
	owner_ops->request_stop(stop_data, generation);
	owner_ops->put(stop_data);
	return 0;
}

bool mmu_owner_has_subscription(struct mmu_owner *owner, const void *data)
{
	struct mmu_owner_subscription *subscription;
	struct mm_struct *mm;
	unsigned long flags;
	unsigned long owner_flags;
	bool found = false;

	if (!owner || !data)
		return false;

	raw_spin_lock_irqsave(&owner->lock, owner_flags);
	mm = owner->mm;
	if (!mm)
		goto out_owner;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	list_for_each_entry(subscription, &owner->subscriptions, link) {
		if (subscription->data == data) {
			found = true;
			break;
		}
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);
out_owner:
	raw_spin_unlock_irqrestore(&owner->lock, owner_flags);
	return found;
}

void mmu_owner_subscription_complete_all(struct list_head *subscriptions)
{
	struct mmu_owner_subscription *subscription, *next;

	list_for_each_entry_safe(subscription, next, subscriptions, link) {
		const struct mmu_owner_subscription_ops *ops = subscription->ops;
		void *owner_data = subscription->owner_data;
		void (*owner_put)(void *data) = subscription->owner_put;
		void *data = subscription->data;

		list_del_init(&subscription->link);
		subscription->owner_data = NULL;
		subscription->owner_put = NULL;
		subscription->data = NULL;
		subscription->ops = NULL;
		subscription->generation = 0;

		/* The completion callback may release the last reference to data. */
		owner_put(owner_data);
		ops->complete(data);
		ops->put(data);
	}
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

	if (!mm || (!snapshot && capacity))
		return -EINVAL;

	raw_spin_lock_irqsave(&mm->execution_owner_lock, flags);
	list_for_each_entry(owner, &mm->execution_owners, link) {
		if (owner->pending_tlb_gen < tlb_gen)
			continue;
		if (nr == capacity) {
			ret = -EOVERFLOW;
			break;
		}
		if (WARN_ON_ONCE(!owner->ops ||
				 !owner->ops->get || !owner->ops->put) ||
		    !owner->ops->get(owner->data)) {
			ret = -EAGAIN;
			break;
		}
		snapshot[nr].data = owner->data;
		snapshot[nr].put = owner->ops->put;
		snapshot[nr].generation = owner->generation;
		nr++;
	}
	raw_spin_unlock_irqrestore(&mm->execution_owner_lock, flags);

	if (ret) {
		while (nr)
			mmu_owner_snapshot_put(&snapshot[--nr]);
		return ret;
	}
	return nr;
}

void mmu_owner_snapshot_put(struct mmu_owner_snapshot *snapshot)
{
	if (!snapshot)
		return;

	if (snapshot->put)
		snapshot->put(snapshot->data);
	else
		WARN_ON_ONCE(snapshot->data);
	snapshot->data = NULL;
	snapshot->put = NULL;
	snapshot->generation = 0;
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
	/* Admission rechecks under the ownership locks; this is a wait hint. */
	return READ_ONCE(mm->execution_owner_update_depth) != 0;
}
