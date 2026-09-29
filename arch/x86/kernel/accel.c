// SPDX-License-Identifier: GPL-2.0-only
#define CREATE_TRACE_POINTS
#include <trace/events/cpu_accel.h>

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/list.h>
#include <linux/mmap_lock.h>
#include <linux/mutex.h>
#include <linux/sched/mm.h>
#include <linux/types.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include <asm/apic.h>
#include <asm/cpu_accel.h>
#include <asm/idtentry.h>
#include <asm/irq_vectors.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

struct x86_cpu_accel_request {
	raw_spinlock_t lock;
	unsigned int cpu;
	x86_cpu_accel_entry_fn entry;
	void *data;
	x86_cpu_accel_stop_fn owner_stop;
	void *owner_stop_data;
	bool owner_stop_requested;
	atomic_t stop_inflight;
	atomic_t active;
	bool exiting;
	atomic_t done;
	atomic_t reschedule_pending;
	atomic64_t reschedule_deferred;
	atomic_t call_function_pending;
	atomic64_t call_function_deferred;
	atomic64_t tlb_shootdown_targets;
	struct mm_struct *owner_mm;
	struct mmu_owner mmu_owner;
	u64 owner_generation;
	bool owner_user_mm;
	/* Highest native TLB generation targeting owner_mm during ownership. */
	u64 pending_tlb_gen;
	u64 exit_tlb_gen;
	bool exit_unscoped_tlb_flush;
	/* Flushes with no single address-space owner (e.g. kernel/global). */
	bool pending_unscoped_tlb_flush;
	bool mmu_owner_initialized;
};

static DEFINE_PER_CPU(struct x86_cpu_accel_request, x86_cpu_accel_request) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(x86_cpu_accel_request.lock),
	.mmu_owner = {
		.lock = __RAW_SPIN_LOCK_UNLOCKED(x86_cpu_accel_request.mmu_owner.lock),
	},
};

static void x86_cpu_accel_request_stop_owner_gen(unsigned int cpu,
						u64 owner_generation);

/* Per-CPU request storage is permanent, so snapshot pinning is a no-op. */
static bool x86_cpu_accel_owner_data_get(void *data)
{
	(void)data;
	return true;
}

static void x86_cpu_accel_owner_data_put(void *data)
{
	(void)data;
}

static void x86_cpu_accel_owner_request_stop(void *data, u64 generation)
{
	struct x86_cpu_accel_request *request = data;

	x86_cpu_accel_request_stop_owner_gen(request->cpu, generation);
}

static const struct mmu_owner_ops x86_cpu_accel_owner_ops = {
	.get = x86_cpu_accel_owner_data_get,
	.put = x86_cpu_accel_owner_data_put,
	.request_stop = x86_cpu_accel_owner_request_stop,
};

static const struct mmu_owner_ops x86_cpu_accel_owner_no_stop_ops = {
	.get = x86_cpu_accel_owner_data_get,
	.put = x86_cpu_accel_owner_data_put,
};

static DEFINE_PER_CPU(unsigned int, x86_cpu_accel_tlb_flush_count);
static atomic_t x86_cpu_accel_active_count = ATOMIC_INIT(0);
static atomic64_t x86_cpu_accel_tlb_flush_id = ATOMIC64_INIT(0);
static DEFINE_RAW_SPINLOCK(x86_cpu_accel_ownership_lock);
static DEFINE_MUTEX(x86_cpu_accel_maintenance_mutex);
static DECLARE_WAIT_QUEUE_HEAD(x86_cpu_accel_owner_wait);
static struct task_struct *x86_cpu_accel_maintenance_owner;
static unsigned int x86_cpu_accel_maintenance_depth;
static bool x86_cpu_accel_maintenance;
static bool x86_cpu_accel_maintenance_try;
static unsigned int x86_cpu_accel_kgdb_breakpoints;

static int x86_cpu_accel_owner_enter(unsigned int cpu, u64 *tlb_targets,
				     struct mm_struct *owner_mm,
				     x86_cpu_accel_stop_fn owner_stop,
				     void *owner_stop_data)
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
	if (x86_cpu_accel_maintenance || x86_cpu_accel_maintenance_try ||
	    x86_cpu_accel_kgdb_breakpoints) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EBUSY;
	}
	if (owner_mm && mmu_owner_update_inflight(owner_mm)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EAGAIN;
	}
	/* Do not enter a CPU while a TLB flush targets it. */
	if (per_cpu(x86_cpu_accel_tlb_flush_count, cpu)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EBUSY;
	}
	raw_spin_lock_irqsave(&request->lock, request_flags);
	if (!request->mmu_owner_initialized) {
		mmu_owner_init(&request->mmu_owner);
		request->mmu_owner_initialized = true;
	}
	if (atomic_read(&request->active) || request->exiting ||
	    atomic_read(&request->stop_inflight)) {
		raw_spin_unlock_irqrestore(&request->lock, request_flags);
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   ownership_flags);
		preempt_enable();
		return -EBUSY;
	}
	atomic_set(&request->reschedule_pending, 0);
	atomic_set(&request->call_function_pending, 0);
	request->cpu = cpu;
	request->owner_mm = owner_mm;
	if (!++request->owner_generation)
		request->owner_generation++;
	request->owner_user_mm = !!owner_mm;
	request->owner_stop = owner_stop;
	request->owner_stop_data = owner_stop_data;
	request->owner_stop_requested = false;
	request->pending_tlb_gen = 0;
	request->pending_unscoped_tlb_flush = false;
	request->exit_tlb_gen = 0;
	request->exit_unscoped_tlb_flush = false;
	if (owner_mm) {
		int ret = mmu_owner_register(owner_mm, &request->mmu_owner,
					     request,
					     request->owner_generation,
					     owner_stop ?
						     &x86_cpu_accel_owner_ops :
						     &x86_cpu_accel_owner_no_stop_ops);

		if (ret) {
			request->owner_mm = NULL;
			request->owner_stop = NULL;
			request->owner_stop_data = NULL;
			request->owner_user_mm = false;
			raw_spin_unlock_irqrestore(&request->lock, request_flags);
			raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
						   ownership_flags);
			preempt_enable();
			return ret;
		}
	}
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
	unsigned int nr_targets;
	unsigned int cpu;
	unsigned long flags;

	cpumask_copy(&flush->targets, targets);
	flush->id = 0;
	flush->caller = _RET_IP_;
	flush->owner_targets = 0;
	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	flush->no_owners = !atomic_read(&x86_cpu_accel_active_count);
	for_each_cpu(cpu, &flush->targets) {
		per_cpu(x86_cpu_accel_tlb_flush_count, cpu)++;
		if (!flush->no_owners &&
		    atomic_read(&per_cpu_ptr(&x86_cpu_accel_request,
					     cpu)->active))
			flush->owner_targets++;
	}
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);

	if (!flush->owner_targets)
		return;

	flush->id = atomic64_inc_return(&x86_cpu_accel_tlb_flush_id);
	nr_targets = cpumask_weight(&flush->targets);
	trace_tlb_flush_wait(flush->id, false, nr_targets,
			     flush->owner_targets, flush->caller);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_tlb_flush_begin);

void x86_cpu_accel_tlb_flush_end(struct x86_cpu_accel_tlb_flush *flush)
{
	unsigned int cpu;
	unsigned long flags;

	if (flush->id)
		trace_tlb_flush_wait(flush->id, true,
				     cpumask_weight(&flush->targets),
				     flush->owner_targets, flush->caller);

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	for_each_cpu(cpu, &flush->targets) {
		if (WARN_ON_ONCE(!per_cpu(x86_cpu_accel_tlb_flush_count, cpu)))
			continue;
		per_cpu(x86_cpu_accel_tlb_flush_count, cpu)--;
	}
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_tlb_flush_end);

/* Generation zero selects the current active owner without an identity check. */
static void
x86_cpu_accel_request_stop_owner_gen(unsigned int cpu, u64 owner_generation)
{
	struct x86_cpu_accel_request *request;
	x86_cpu_accel_stop_fn stop = NULL;
	void *data = NULL;
	unsigned long caller = _RET_IP_;
	unsigned long flags;
	u64 owner_id = 0;
	bool active = false;
	bool user_mm = false;
	bool callback_registered = false;
	bool callback_sent = false;

	if (cpu >= nr_cpu_ids)
		return;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_read(&request->active) &&
	    (!owner_generation ||
	     request->owner_generation == owner_generation)) {
		active = true;
		atomic_inc(&request->stop_inflight);
		owner_id = request->owner_generation;
		user_mm = request->owner_user_mm;
		callback_registered = !!request->owner_stop;
		if (request->owner_stop && !request->owner_stop_requested) {
			request->owner_stop_requested = true;
			stop = request->owner_stop;
			data = request->owner_stop_data;
			callback_sent = true;
		}
	}
	raw_spin_unlock_irqrestore(&request->lock, flags);
	if (active) {
		trace_owner_stop_request(cpu, owner_id, user_mm,
					 callback_registered, callback_sent,
					 caller);
		if (stop) {
			/* This callback only publishes a stop request; it must not wait. */
			stop(data);
		}
		atomic_dec_return_release(&request->stop_inflight);
	}
}

void x86_cpu_accel_request_stop_owner(unsigned int cpu)
{
	x86_cpu_accel_request_stop_owner_gen(cpu, 0);
}

static u64 x86_cpu_accel_owner_exit(unsigned int cpu,
				    bool *local_tlb_flush,
				    struct list_head *subscriptions,
				    bool *owner_exited)
{
	struct x86_cpu_accel_request *request;
	struct mm_struct *owner_mm = NULL;
	unsigned long flags;
	u64 targets, owner_id = 0;
	bool exited = false;

	if (local_tlb_flush)
		*local_tlb_flush = false;
	if (owner_exited)
		*owner_exited = false;
	if (cpu >= nr_cpu_ids)
		return 0;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_xchg(&request->active, 0)) {
		WARN_ON_ONCE(request->exiting);
		request->exiting = true;
		exited = true;
		owner_mm = request->owner_mm;
		owner_id = request->owner_generation;
		request->exit_tlb_gen = request->pending_tlb_gen;
		request->exit_unscoped_tlb_flush =
			request->pending_unscoped_tlb_flush;
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
	if (exited && owner_mm) {
		u64 tlb_gen = mmu_owner_unregister(&request->mmu_owner,
							    subscriptions);

		raw_spin_lock_irqsave(&request->lock, flags);
		if (WARN_ON_ONCE(!request->exiting ||
				 request->owner_generation != owner_id)) {
			raw_spin_unlock_irqrestore(&request->lock, flags);
			return targets;
		}
		if (tlb_gen > request->exit_tlb_gen)
			request->exit_tlb_gen = tlb_gen;
		if (local_tlb_flush && tlb_gen)
			*local_tlb_flush = true;
		raw_spin_unlock_irqrestore(&request->lock, flags);
	}
	/* Keep callback storage alive until every request made under the lock ran. */
	while (atomic_read_acquire(&request->stop_inflight))
		cpu_relax();
	if (owner_exited)
		*owner_exited = exited;
	return targets;
}

static void x86_cpu_accel_owner_exit_finish(unsigned int cpu, bool owner_exited)
{
	struct x86_cpu_accel_request *request;
	u64 owner_id, tlb_gen;
	bool user_mm, stop_requested, unscoped_flush;
	unsigned long flags;

	if (!owner_exited)
		return;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (WARN_ON_ONCE(!request->exiting)) {
		raw_spin_unlock_irqrestore(&request->lock, flags);
		return;
	}
	owner_id = request->owner_generation;
	user_mm = request->owner_user_mm;
	stop_requested = request->owner_stop_requested;
	tlb_gen = request->exit_tlb_gen;
	unscoped_flush = request->exit_unscoped_tlb_flush;
	raw_spin_unlock_irqrestore(&request->lock, flags);

	/* Keep this owner slot reserved until its completion is observable. */
	trace_owner_exit_complete(cpu, owner_id, user_mm, stop_requested,
				  tlb_gen, unscoped_flush);

	raw_spin_lock_irqsave(&request->lock, flags);
	if (WARN_ON_ONCE(!request->exiting ||
			 request->owner_generation != owner_id)) {
		raw_spin_unlock_irqrestore(&request->lock, flags);
		return;
	}
	request->owner_stop = NULL;
	request->owner_stop_data = NULL;
	request->owner_stop_requested = false;
	request->owner_user_mm = false;
	request->exit_tlb_gen = 0;
	request->exit_unscoped_tlb_flush = false;
	request->exiting = false;
	atomic_dec(&x86_cpu_accel_active_count);
	raw_spin_unlock_irqrestore(&request->lock, flags);
	wake_up_all(&x86_cpu_accel_owner_wait);
}

static void x86_cpu_accel_tlb_reconcile_and_ack(void *data)
{
	struct list_head *subscriptions = data;

	__flush_tlb_all();
	mmu_owner_subscription_complete_all(subscriptions);
}

static void x86_cpu_accel_tlb_reconcile(unsigned int cpu,
					bool local_tlb_flush,
					struct list_head *subscriptions)
{
	int ret;

	if (!local_tlb_flush && list_empty(subscriptions))
		return;
	if (cpu == raw_smp_processor_id()) {
		__flush_tlb_all();
		mmu_owner_subscription_complete_all(subscriptions);
		return;
	}

	/* A void owner-exit API cannot report a missing TLB acknowledgement. */
	do {
		ret = smp_call_function_single(cpu,
					       x86_cpu_accel_tlb_reconcile_and_ack,
					       subscriptions, 1);
		if (ret) {
			WARN_ON_ONCE(ret);
			cpu_relax();
		}
	} while (ret);
}

static void x86_cpu_accel_direct_owner_exit(unsigned int cpu)
{
	LIST_HEAD(subscriptions);
	bool local_tlb_flush;
	bool owner_exited;

	x86_cpu_accel_owner_exit(cpu, &local_tlb_flush, &subscriptions,
				&owner_exited);
	x86_cpu_accel_tlb_reconcile(cpu, local_tlb_flush, &subscriptions);
	x86_cpu_accel_owner_exit_finish(cpu, owner_exited);
}

void x86_cpu_accel_maintenance_begin(void)
{
	unsigned int cpu;
	unsigned long flags;

	might_sleep();
	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	if (x86_cpu_accel_maintenance_owner == current) {
		x86_cpu_accel_maintenance_depth++;
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
		return;
	}
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);

	mutex_lock(&x86_cpu_accel_maintenance_mutex);
	for (;;) {
		/* Serialize admission with owner entry and try-only callers. */
		raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
		if (!x86_cpu_accel_maintenance_try) {
			WARN_ON_ONCE(x86_cpu_accel_maintenance_owner);
			x86_cpu_accel_maintenance_owner = current;
			x86_cpu_accel_maintenance_depth = 1;
			x86_cpu_accel_maintenance = true;
			raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
						   flags);
			break;
		}
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
		wait_event(x86_cpu_accel_owner_wait,
			   !READ_ONCE(x86_cpu_accel_maintenance_try));
	}
	/*
	 * Admission is closed, so this set cannot grow. Ask owners with a
	 * registered stop callback to leave; the callback only requests a stop
	 * and does not acknowledge owner exit or TLB completion.
	 */
	for_each_possible_cpu(cpu)
		x86_cpu_accel_request_stop_owner(cpu);

	/*
	 * owner_exit_finish() drops active_count only after local TLB
	 * reconciliation and any reclaim acknowledgements have completed.
	 * Owners without a stop callback, or which do not honor the request,
	 * retain the existing synchronous wait.
	 */
	wait_event(x86_cpu_accel_owner_wait,
		   !atomic_read(&x86_cpu_accel_active_count));
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_maintenance_begin);

void x86_cpu_accel_maintenance_end(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	if (WARN_ON_ONCE(x86_cpu_accel_maintenance_owner != current ||
			 !x86_cpu_accel_maintenance_depth)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock,
					   flags);
		return;
	}
	if (--x86_cpu_accel_maintenance_depth) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
		return;
	}
	x86_cpu_accel_maintenance_owner = NULL;
	x86_cpu_accel_maintenance = false;
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
	mutex_unlock(&x86_cpu_accel_maintenance_mutex);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_maintenance_end);

bool x86_cpu_accel_maintenance_try_begin(void)
{
	unsigned long flags;
	bool acquired = false;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	if (!x86_cpu_accel_maintenance && !x86_cpu_accel_maintenance_try &&
	    !atomic_read(&x86_cpu_accel_active_count)) {
		x86_cpu_accel_maintenance_try = true;
		acquired = true;
	}
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);

	return acquired;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_maintenance_try_begin);

void x86_cpu_accel_maintenance_try_end(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	if (WARN_ON_ONCE(!x86_cpu_accel_maintenance_try)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
		return;
	}
	x86_cpu_accel_maintenance_try = false;
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
	wake_up_all(&x86_cpu_accel_owner_wait);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_maintenance_try_end);

void x86_cpu_accel_kgdb_breakpoint_get(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	x86_cpu_accel_kgdb_breakpoints++;
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_kgdb_breakpoint_get);

void x86_cpu_accel_kgdb_breakpoint_put(void)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	if (WARN_ON_ONCE(!x86_cpu_accel_kgdb_breakpoints)) {
		raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
		return;
	}
	x86_cpu_accel_kgdb_breakpoints--;
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_kgdb_breakpoint_put);

int x86_cpu_accel_user_enter(unsigned int cpu, struct mm_struct *mm,
			     x86_cpu_accel_stop_fn stop, void *data,
			     u64 *tlb_targets)
{
	int ret;

	if (!mm)
		return -EINVAL;
	if (cpu != raw_smp_processor_id())
		return -EXDEV;
	if (mm != current->mm)
		return -EXDEV;
	mmap_assert_write_locked(mm);

	for (;;) {
		wait_event(x86_cpu_accel_owner_wait,
			   !mmu_owner_update_inflight(mm));

		/*
		 * A batched unmap may have published a new generation without
		 * flushing this CPU yet. Reconcile its local translations before
		 * making the address space available to the accelerator.
		 */
		preempt_disable();
		if (cpu != raw_smp_processor_id()) {
			preempt_enable();
			return -EXDEV;
		}
		__flush_tlb_all();
		ret = x86_cpu_accel_owner_enter(cpu, tlb_targets, mm,
						stop, data);
		preempt_enable();
		if (ret != -EAGAIN)
			return ret;
	}
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_user_enter);

u64 x86_cpu_accel_user_exit(unsigned int cpu)
{
	LIST_HEAD(subscriptions);
	bool local_tlb_flush;
	bool owner_exited;
	u64 targets = x86_cpu_accel_owner_exit(cpu, &local_tlb_flush,
						      &subscriptions,
						      &owner_exited);

	/* Flush and complete subscriptions before publishing owner exit. */
	x86_cpu_accel_tlb_reconcile(cpu, local_tlb_flush, &subscriptions);
	x86_cpu_accel_owner_exit_finish(cpu, owner_exited);
	return targets;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_user_exit);

void x86_cpu_accel_tlb_unmap_begin(struct mm_struct *mm)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	mmu_owner_update_begin(mm);
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
}

void x86_cpu_accel_tlb_unmap_end(struct mm_struct *mm)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&x86_cpu_accel_ownership_lock, flags);
	mmu_owner_update_end(mm);
	raw_spin_unlock_irqrestore(&x86_cpu_accel_ownership_lock, flags);
	wake_up_all(&x86_cpu_accel_owner_wait);
}

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry,
			       x86_cpu_accel_stop_fn stop, void *data)
{
	struct x86_cpu_accel_request *request;
	int ret;

	if (!entry || cpu == raw_smp_processor_id())
		return -EINVAL;
	ret = x86_cpu_accel_owner_enter(cpu, NULL, NULL, stop, data);
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
			 tlb_gen > request->pending_tlb_gen) {
			request->pending_tlb_gen = tlb_gen;
			mmu_owner_update_one_tlb_gen(&request->mmu_owner, tlb_gen);
		}
	}
	raw_spin_unlock_irqrestore(&request->lock, flags);

	return active;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_note_tlb_shootdown);

void x86_cpu_accel_note_tlb_unmap(struct mm_struct *mm, u64 tlb_gen)
{
	struct x86_cpu_accel_request *request;
	unsigned int cpu;
	unsigned long flags;

	if (!mm)
		return;
	mmu_owner_update_tlb_gen(mm, tlb_gen);
	if (!x86_cpu_accel_any_active())
		return;

	for_each_cpu(cpu, mm_cpumask(mm)) {
		request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
		raw_spin_lock_irqsave(&request->lock, flags);
		if (atomic_read(&request->active) && request->owner_mm == mm &&
		    tlb_gen > request->pending_tlb_gen)
			request->pending_tlb_gen = tlb_gen;
		raw_spin_unlock_irqrestore(&request->lock, flags);
	}
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_note_tlb_unmap);

bool x86_cpu_accel_reclaim_record(struct x86_cpu_accel_tlb_reclaim_completion *comp,
				  struct mm_struct *mm, u64 tlb_gen)
{
	unsigned int i;

	if (!comp || !mm || comp->overflow)
		return false;

	for (i = 0; i < comp->nr_mms; i++) {
		if (comp->mms[i].mm != mm)
			continue;
		if (tlb_gen > comp->mms[i].tlb_gen)
			comp->mms[i].tlb_gen = tlb_gen;
		return true;
	}

	if (comp->nr_mms == X86_CPU_ACCEL_TLB_RECLAIM_MAX_MMS) {
		comp->overflow = true;
		return false;
	}

	mmgrab(mm);
	comp->mms[comp->nr_mms].mm = mm;
	comp->mms[comp->nr_mms].tlb_gen = tlb_gen;
	comp->nr_mms++;
	return true;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_reclaim_record);

int x86_cpu_accel_reclaim_register(struct x86_cpu_accel_tlb_reclaim_completion *comp,
				   void *data,
				   const struct mmu_owner_subscription_ops *ops)
{
	struct x86_cpu_accel_request *request;
	unsigned int i;
	unsigned int registered = 0;

	if (!comp || comp->overflow || !ops)
		return -EOVERFLOW;

	for (i = 0; i < comp->nr_mms; i++) {
		struct mm_struct *mm = comp->mms[i].mm;
		struct mmu_owner_snapshot owners[X86_CPU_ACCEL_TLB_RECLAIM_MAX_ACKS];
		u64 tlb_gen = comp->mms[i].tlb_gen;
		int nr_owners, j;

		nr_owners = mmu_owner_snapshot(mm, tlb_gen, owners,
					       ARRAY_SIZE(owners));
		if (nr_owners < 0) {
			comp->overflow = true;
			return -EOVERFLOW;
		}

		for (j = 0; j < nr_owners; j++) {
			request = owners[j].data;
			if (!request || comp->nr_subscriptions ==
			    X86_CPU_ACCEL_TLB_RECLAIM_MAX_ACKS) {
				comp->overflow = true;
				mmu_owner_snapshot_put(&owners[j]);
				for (j++; j < nr_owners; j++)
					mmu_owner_snapshot_put(&owners[j]);
				return -EOVERFLOW;
			}

			if (mmu_owner_subscribe(&request->mmu_owner,
						owners[j].generation, tlb_gen,
						&comp->subscriptions[
							comp->nr_subscriptions],
						data, ops)) {
				comp->overflow = true;
				mmu_owner_snapshot_put(&owners[j]);
				for (j++; j < nr_owners; j++)
					mmu_owner_snapshot_put(&owners[j]);
				return -EOVERFLOW;
			}
			comp->nr_subscriptions++;
			registered++;
			mmu_owner_snapshot_put(&owners[j]);
		}
	}

	return registered;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_reclaim_register);

void x86_cpu_accel_reclaim_release(struct x86_cpu_accel_tlb_reclaim_completion *comp)
{
	unsigned int i;

	for (i = 0; i < comp->nr_subscriptions; i++)
		WARN_ON_ONCE(!list_empty(&comp->subscriptions[i].link));
	for (i = 0; i < comp->nr_mms; i++)
		mmdrop(comp->mms[i].mm);
	comp->nr_mms = 0;
	comp->nr_subscriptions = 0;
	comp->overflow = false;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_reclaim_release);

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
		    tlb_gen > request->pending_tlb_gen) {
			request->pending_tlb_gen = tlb_gen;
			mmu_owner_update_one_tlb_gen(&request->mmu_owner, tlb_gen);
		}
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

bool x86_cpu_accel_filter_tlb_unmap(unsigned int cpu,
				    const void *completion)
{
	struct x86_cpu_accel_request *request;
	unsigned long flags;
	bool filter = false;

	if (cpu >= nr_cpu_ids || !x86_cpu_accel_any_active())
		return false;
	request = per_cpu_ptr(&x86_cpu_accel_request, cpu);
	raw_spin_lock_irqsave(&request->lock, flags);
	if (atomic_read(&request->active) && request->owner_mm) {
		if (completion)
			filter = mmu_owner_has_subscription(&request->mmu_owner,
							    completion);
		if (!filter && !request->pending_tlb_gen &&
		    !request->pending_unscoped_tlb_flush)
			filter = true;
		if (filter)
			atomic64_inc(&request->tlb_shootdown_targets);
	}
	raw_spin_unlock_irqrestore(&request->lock, flags);
	return filter;
}
EXPORT_SYMBOL_GPL(x86_cpu_accel_filter_tlb_unmap);

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
	/*
	 * Release the target CPU before waking the controller. A synchronous
	 * TLB flush may be waiting for an IPI that was deferred during ownership;
	 * the controller can itself be blocked in that flush.
	 */
	x86_cpu_accel_direct_owner_exit(raw_smp_processor_id());
	WRITE_ONCE(request->data, NULL);
	smp_store_release(&request->entry, NULL);
	atomic_set_release(&request->done, 1);
}
