/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPU_ACCEL_H
#define _ASM_X86_CPU_ACCEL_H

#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/mutex.h>

typedef void (*x86_cpu_accel_entry_fn)(void *data);

struct x86_cpu_accel_tlb_flush {
	cpumask_t targets;
	bool no_owners;
};

struct mm_struct;

#define X86_CPU_ACCEL_TLB_RECLAIM_MAX_MMS	16
#define X86_CPU_ACCEL_TLB_RECLAIM_MAX_ACKS	16

typedef void (*x86_cpu_accel_reclaim_fn)(void *data);

struct x86_cpu_accel_tlb_reclaim_completion;
struct x86_cpu_accel_tlb_reclaim_ack {
	struct list_head link;
	struct x86_cpu_accel_tlb_reclaim_completion *completion;
	struct mm_struct *mm;
	void *data;
	x86_cpu_accel_reclaim_fn ack;
	u64 tlb_gen;
};

struct x86_cpu_accel_tlb_reclaim_completion {
	struct {
		struct mm_struct *mm;
		u64 tlb_gen;
	} mms[X86_CPU_ACCEL_TLB_RECLAIM_MAX_MMS];
	struct x86_cpu_accel_tlb_reclaim_ack
		acks[X86_CPU_ACCEL_TLB_RECLAIM_MAX_ACKS];
	unsigned int nr_mms;
	unsigned int nr_acks;
	bool overflow;
};

extern struct mutex text_mutex;

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry, void *data);
bool x86_cpu_accel_defer_reschedule(unsigned int cpu);
u64 x86_cpu_accel_reschedule_deferred(unsigned int cpu);
bool x86_cpu_accel_defer_call_function(unsigned int cpu);
u64 x86_cpu_accel_call_function_deferred(unsigned int cpu);
#ifdef CONFIG_X86_LOCAL_APIC
/* Keep mm write-locked until user_exit() has reconciled its TLB state. */
int x86_cpu_accel_user_enter(unsigned int cpu, struct mm_struct *mm,
			     u64 *tlb_targets);
u64 x86_cpu_accel_user_exit(unsigned int cpu);
void x86_cpu_accel_tlb_unmap_begin(struct mm_struct *mm);
void x86_cpu_accel_tlb_unmap_end(struct mm_struct *mm);
bool x86_cpu_accel_any_active(void);
/* Drain owners, block admission, and allow same-task nesting. */
void x86_cpu_accel_maintenance_begin(void);
void x86_cpu_accel_maintenance_end(void);
/* Succeed only with no owners; block admission until try_end(). */
bool x86_cpu_accel_maintenance_try_begin(void);
void x86_cpu_accel_maintenance_try_end(void);
/*
 * Call before recording owners; reserve admission until end.
 * no_owners permits a broadcast.
 */
void x86_cpu_accel_tlb_flush_begin(struct x86_cpu_accel_tlb_flush *flush,
				   const struct cpumask *targets);
void x86_cpu_accel_tlb_flush_end(struct x86_cpu_accel_tlb_flush *flush);
bool x86_cpu_accel_note_tlb_shootdown(unsigned int cpu,
				      const struct mm_struct *mm, u64 tlb_gen);
void x86_cpu_accel_note_tlb_unmap(struct mm_struct *mm, u64 tlb_gen);
bool x86_cpu_accel_reclaim_record(struct x86_cpu_accel_tlb_reclaim_completion *comp,
				  struct mm_struct *mm, u64 tlb_gen);
int x86_cpu_accel_reclaim_register(struct x86_cpu_accel_tlb_reclaim_completion *comp,
				   void *data, x86_cpu_accel_reclaim_fn get,
				   x86_cpu_accel_reclaim_fn ack);
void x86_cpu_accel_reclaim_release(struct x86_cpu_accel_tlb_reclaim_completion *comp);
/* Record an mm-scoped target and filter ring-3 owners from its IPI mask. */
bool x86_cpu_accel_filter_mm_tlb_shootdown(unsigned int cpu,
					   const struct mm_struct *mm,
					   u64 tlb_gen);
/* Filter unchanged-mm owners or owners registered to this reclaim completion. */
bool x86_cpu_accel_filter_tlb_unmap(unsigned int cpu,
				    const void *completion);
u64 x86_cpu_accel_tlb_shootdown_targets(unsigned int cpu);
#else
static inline int x86_cpu_accel_user_enter(unsigned int cpu,
					   struct mm_struct *mm,
					   u64 *tlb_targets)
{
	(void)cpu;
	(void)mm;
	if (tlb_targets)
		*tlb_targets = 0;
	return 0;
}

static inline u64 x86_cpu_accel_user_exit(unsigned int cpu)
{
	(void)cpu;
	return 0;
}

static inline void x86_cpu_accel_tlb_unmap_begin(struct mm_struct *mm)
{
	(void)mm;
}

static inline void x86_cpu_accel_tlb_unmap_end(struct mm_struct *mm)
{
	(void)mm;
}

static inline bool
x86_cpu_accel_reclaim_record(struct x86_cpu_accel_tlb_reclaim_completion *comp,
			     struct mm_struct *mm, u64 tlb_gen)
{
	(void)comp;
	(void)mm;
	(void)tlb_gen;
	return false;
}

static inline int
x86_cpu_accel_reclaim_register(struct x86_cpu_accel_tlb_reclaim_completion *comp,
			       void *data, x86_cpu_accel_reclaim_fn get,
			       x86_cpu_accel_reclaim_fn ack)
{
	(void)comp;
	(void)data;
	(void)get;
	(void)ack;
	return 0;
}

static inline void
x86_cpu_accel_reclaim_release(struct x86_cpu_accel_tlb_reclaim_completion *comp)
{
	(void)comp;
}

static inline bool x86_cpu_accel_any_active(void)
{
	return false;
}

static inline void x86_cpu_accel_maintenance_begin(void)
{
}

static inline void x86_cpu_accel_maintenance_end(void)
{
}

static inline bool x86_cpu_accel_maintenance_try_begin(void)
{
	return true;
}

static inline void x86_cpu_accel_maintenance_try_end(void)
{
}

static inline void
x86_cpu_accel_tlb_flush_begin(struct x86_cpu_accel_tlb_flush *flush,
			      const struct cpumask *targets)
{
	cpumask_copy(&flush->targets, targets);
	flush->no_owners = true;
}

static inline void
x86_cpu_accel_tlb_flush_end(struct x86_cpu_accel_tlb_flush *flush)
{
	(void)flush;
}

static inline bool x86_cpu_accel_note_tlb_shootdown(unsigned int cpu,
						    const struct mm_struct *mm,
						    u64 tlb_gen)
{
	(void)cpu;
	(void)mm;
	(void)tlb_gen;
	return false;
}

static inline void x86_cpu_accel_note_tlb_unmap(struct mm_struct *mm,
						u64 tlb_gen)
{
	(void)mm;
	(void)tlb_gen;
}

static inline bool
x86_cpu_accel_filter_mm_tlb_shootdown(unsigned int cpu,
				      const struct mm_struct *mm,
				      u64 tlb_gen)
{
	(void)cpu;
	(void)mm;
	(void)tlb_gen;
	return false;
}

static inline bool x86_cpu_accel_filter_tlb_unmap(unsigned int cpu,
						  const void *completion)
{
	(void)cpu;
	(void)completion;
	return false;
}

static inline u64 x86_cpu_accel_tlb_shootdown_targets(unsigned int cpu)
{
	(void)cpu;
	return 0;
}
#endif

static inline void x86_cpu_accel_text_mutex_lock(void)
	__acquires(&text_mutex)
{
	x86_cpu_accel_maintenance_begin();
	mutex_lock(&text_mutex);
}

static inline void x86_cpu_accel_text_mutex_unlock(void)
	__releases(&text_mutex)
{
	mutex_unlock(&text_mutex);
	x86_cpu_accel_maintenance_end();
}

#endif
