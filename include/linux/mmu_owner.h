/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MMU_OWNER_H
#define _LINUX_MMU_OWNER_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct mm_struct;

/* The ops table and callback text must outlive owners, snapshots, and subs. */
struct mmu_owner_ops {
	/*
	 * Called with mm->execution_owner_lock held. Must be atomic, must not
	 * take owner->lock, and must not re-enter the owner registry. Returning
	 * true means one reference was acquired; false declines the snapshot.
	 */
	bool (*get)(void *data);
	/* Must be atomic-safe; may run in atomic context and after unregister. */
	void (*put)(void *data);
	/* Optional nonblocking stop request, called with no MM/owner locks held. */
	void (*request_stop)(void *data, u64 generation);
};

/* Callbacks for a preallocated completion record attached to one owner. */
struct mmu_owner_subscription_ops {
	/* Called under owner and registry locks; atomic, nonblocking, takes neither. */
	bool (*get)(void *data);
	/* Atomic-safe; called only after owner exit and local TLB reconciliation. */
	void (*complete)(void *data);
	/* Atomic-safe; releases the get() reference and may free data. */
	void (*put)(void *data);
};

/* An execution context using an mm outside the ordinary task-mm tracking. */
struct mmu_owner {
	raw_spinlock_t lock;
	struct list_head link;
	struct list_head subscriptions;
	struct mm_struct *mm;
	void *data;
	const struct mmu_owner_ops *ops;
	u64 generation;
	u64 pending_tlb_gen;
};

/* Caller-owned storage remains pinned until complete/put have both run. */
struct mmu_owner_subscription {
	struct list_head link;
	void *owner_data;
	void (*owner_put)(void *data);
	void *data;
	const struct mmu_owner_subscription_ops *ops;
	u64 generation;
};

struct mmu_owner_snapshot {
	void *data;
	void (*put)(void *data);
	u64 generation;
};

#ifdef CONFIG_X86
extern const struct mmu_owner_subscription_ops
	mmu_gather_completion_owner_ops;
#endif

/*
 * A registered owner pins the live address space on MMU builds. A snapshot
 * takes an owner-data reference under the registry lock; callers must release
 * it with mmu_owner_snapshot_put().
 */

void mmu_owner_init(struct mmu_owner *owner);
/* Call mmu_owner_init() before the first registration. */
int mmu_owner_register(struct mm_struct *mm, struct mmu_owner *owner,
		       void *data, u64 generation,
		       const struct mmu_owner_ops *ops);
/* Detach subscriptions for completion after the owner's local TLB flush. */
u64 mmu_owner_unregister(struct mmu_owner *owner,
			 struct list_head *subscriptions);
void mmu_owner_update_tlb_gen(struct mm_struct *mm, u64 tlb_gen);
void mmu_owner_update_one_tlb_gen(struct mmu_owner *owner, u64 tlb_gen);
int mmu_owner_snapshot(struct mm_struct *mm, u64 tlb_gen,
		       struct mmu_owner_snapshot *snapshot, unsigned int capacity);
void mmu_owner_snapshot_put(struct mmu_owner_snapshot *snapshot);
/*
 * Attach to this exact generation and request stop after registry locks are
 * dropped. Call only after page-table locks have been released.
 */
int mmu_owner_subscribe(struct mmu_owner *owner, u64 generation, u64 tlb_gen,
			struct mmu_owner_subscription *subscription,
			void *data,
			const struct mmu_owner_subscription_ops *ops);
bool mmu_owner_has_subscription(struct mmu_owner *owner, const void *data);
/* Call only after the owner has reconciled its local TLB state. */
void mmu_owner_subscription_complete_all(struct list_head *subscriptions);
void mmu_owner_update_begin(struct mm_struct *mm);
void mmu_owner_update_end(struct mm_struct *mm);
bool mmu_owner_update_inflight(struct mm_struct *mm);

#endif /* _LINUX_MMU_OWNER_H */
