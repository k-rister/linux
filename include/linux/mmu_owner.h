/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MMU_OWNER_H
#define _LINUX_MMU_OWNER_H

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct mm_struct;

/* An execution context using an mm outside the ordinary task-mm tracking. */
struct mmu_owner {
	raw_spinlock_t lock;
	struct list_head link;
	struct mm_struct *mm;
	void *data;
	u64 generation;
	u64 pending_tlb_gen;
};

struct mmu_owner_snapshot {
	void *data;
	u64 generation;
};

/* Snapshot data is borrowed; callers must revalidate the generation. */

void mmu_owner_init(struct mmu_owner *owner);
int mmu_owner_register(struct mm_struct *mm, struct mmu_owner *owner,
		       void *data, u64 generation);
u64 mmu_owner_unregister(struct mmu_owner *owner);
void mmu_owner_update_tlb_gen(struct mm_struct *mm, u64 tlb_gen);
void mmu_owner_update_one_tlb_gen(struct mmu_owner *owner, u64 tlb_gen);
int mmu_owner_snapshot(struct mm_struct *mm, u64 tlb_gen,
		       struct mmu_owner_snapshot *snapshot, unsigned int capacity);
void mmu_owner_update_begin(struct mm_struct *mm);
void mmu_owner_update_end(struct mm_struct *mm);
bool mmu_owner_update_inflight(struct mm_struct *mm);

#endif /* _LINUX_MMU_OWNER_H */
