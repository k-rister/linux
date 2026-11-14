// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/sizes.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>

#include <asm/cpu_accel.h>
#include <asm/ptrace.h>

struct cpu_accel_vmalloc_args {
	size_t bytes;
	void *area;
	void **areas;
	size_t nr_areas;
	size_t area_bytes;
};

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static int work_cpu = 3;
module_param(work_cpu, int, 0444);

static unsigned int prime_mb = 96;
module_param(prime_mb, uint, 0444);

static unsigned int held_mb = 192;
module_param(held_mb, uint, 0444);

static bool wait_for_reentry;
module_param(wait_for_reentry, bool, 0444);

static unsigned int reentry_timeout_ms = 400;
module_param(reentry_timeout_ms, uint, 0444);

static void *held_area;
static void **prime_areas;
static size_t nr_prime_areas;
static bool purge_armed;
static bool reentry_seen;
static bool reentry_timed_out;
static atomic_t sync_wait_gate_claimed = ATOMIC_INIT(0);
static atomic_t rearm_owner_entered = ATOMIC_INIT(0);
static atomic_t purge_depth = ATOMIC_INIT(0);
static atomic_t kernel_tlb_flush_depth = ATOMIC_INIT(0);
static struct task_struct *purge_task;

struct cpu_accel_probe_data {
	bool armed;
};

static int purge_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	data->armed = READ_ONCE(purge_armed);
	if (data->armed && atomic_inc_return(&purge_depth) == 1)
		WRITE_ONCE(purge_task, current);
	(void)regs;
	return 0;
}

static int purge_return(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	if (data->armed && atomic_dec_and_test(&purge_depth) &&
	    READ_ONCE(purge_task) == current)
		WRITE_ONCE(purge_task, NULL);
	(void)regs;
	return 0;
}

static int kernel_tlb_flush_entry(struct kretprobe_instance *ri,
				  struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	data->armed = READ_ONCE(purge_armed) &&
		READ_ONCE(purge_task) == current;
	if (data->armed)
		atomic_inc(&kernel_tlb_flush_depth);
	(void)regs;
	return 0;
}

static int kernel_tlb_flush_return(struct kretprobe_instance *ri,
				   struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	if (data->armed)
		atomic_dec(&kernel_tlb_flush_depth);
	(void)regs;
	return 0;
}

static int user_enter_entry(struct kretprobe_instance *ri,
			    struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	data->armed = READ_ONCE(purge_armed) &&
		regs->di == (unsigned long)target_cpu;
	return 0;
}

static int user_enter_return(struct kretprobe_instance *ri,
			     struct pt_regs *regs)
{
	struct cpu_accel_probe_data *data = (void *)ri->data;

	if (data->armed && !regs_return_value(regs))
		atomic_set_release(&rearm_owner_entered, 1);
	return 0;
}

static int sync_wait_begin_pre(struct kprobe *kp, struct pt_regs *regs)
{
	u64 deadline;
	(void)kp;

	if (!READ_ONCE(purge_armed) ||
	    READ_ONCE(purge_task) != current ||
	    atomic_read(&kernel_tlb_flush_depth) ||
	    regs->di != (unsigned long)target_cpu ||
	    atomic_cmpxchg(&sync_wait_gate_claimed, 0, 1))
		return 0;

	/*
	 * Keep the vmalloc sync-wait begin from publishing its waiter until the
	 * test has re-entered the target CPU. The following stop must then see
	 * that owner; after begin, owner_enter rejects new users until wait_end.
	 */
	deadline = ktime_get_ns() +
		   (u64)READ_ONCE(reentry_timeout_ms) * NSEC_PER_MSEC;
	while (!atomic_read_acquire(&rearm_owner_entered) &&
	       ktime_get_ns() < deadline)
		cpu_relax();

	if (atomic_read_acquire(&rearm_owner_entered))
		WRITE_ONCE(reentry_seen, true);
	else
		WRITE_ONCE(reentry_timed_out, true);
	return 0;
}

static struct kretprobe purge_probe = {
	.handler = purge_return,
	.entry_handler = purge_entry,
	.data_size = sizeof(struct cpu_accel_probe_data),
	.maxactive = 8,
	.kp.symbol_name = "__purge_vmap_area_lazy",
};

static struct kretprobe user_enter_probe = {
	.handler = user_enter_return,
	.entry_handler = user_enter_entry,
	.data_size = sizeof(struct cpu_accel_probe_data),
	.maxactive = 8,
	.kp.symbol_name = "x86_cpu_accel_user_enter",
};

static struct kretprobe kernel_tlb_flush_probe = {
	.handler = kernel_tlb_flush_return,
	.entry_handler = kernel_tlb_flush_entry,
	.data_size = sizeof(struct cpu_accel_probe_data),
	.maxactive = 8,
	.kp.symbol_name = "flush_tlb_kernel_range",
};

static struct kprobe sync_wait_begin_probe = {
	.pre_handler = sync_wait_begin_pre,
	.symbol_name = "arch_smp_sync_wait_begin",
};

static int allocate_vmalloc(void *data)
{
	struct cpu_accel_vmalloc_args *args = data;
	size_t i;

	if (args->areas) {
		for (i = 0; i < args->nr_areas; i++) {
			args->areas[i] = vmalloc(args->area_bytes);
			if (!args->areas[i])
				goto free_areas;
			*(volatile char *)args->areas[i] = 1;
		}
		return 0;
	}

	args->area = vmalloc(args->bytes);
	if (!args->area)
		return -ENOMEM;

	*(volatile char *)args->area = 1;
	return 0;

free_areas:
	while (i)
		vfree(args->areas[--i]);
	return -ENOMEM;
}

static void free_prime_areas(void)
{
	size_t i;

	for (i = 0; i < nr_prime_areas; i++) {
		if (prime_areas[i]) {
			vfree(prime_areas[i]);
			prime_areas[i] = NULL;
		}
	}
}

static int hold_vmalloc(void *data)
{
	struct cpu_accel_vmalloc_args *args = data;

	args->area = vmalloc(args->bytes);
	if (!args->area)
		return -ENOMEM;
	*(volatile char *)args->area = 1;

	return 0;
}

static int __init cpu_accel_vmalloc_purge_probe_init(void)
{
	struct cpu_accel_vmalloc_args prime = {
		.nr_areas = prime_mb,
		.area_bytes = SZ_1M,
	};
	struct cpu_accel_vmalloc_args held = {
		.bytes = (size_t)held_mb << 20,
	};
	int ret;

	if (target_cpu < 0 || work_cpu < 0 || target_cpu == work_cpu ||
	    target_cpu >= nr_cpu_ids || work_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu) || !cpu_online(work_cpu))
		return -EINVAL;
	if (!prime_mb || prime_mb > 512 || !held_mb)
		return -EINVAL;
	if (wait_for_reentry &&
	    (!reentry_timeout_ms || reentry_timeout_ms > 1000))
		return -EINVAL;

	if (wait_for_reentry) {
		ret = register_kretprobe(&purge_probe);

		if (ret < 0)
			return ret;
		ret = register_kretprobe(&user_enter_probe);
		if (ret < 0) {
			unregister_kretprobe(&purge_probe);
			return ret;
		}
		ret = register_kretprobe(&kernel_tlb_flush_probe);
		if (ret < 0) {
			unregister_kretprobe(&user_enter_probe);
			unregister_kretprobe(&purge_probe);
			return ret;
		}
		ret = register_kprobe(&sync_wait_begin_probe);
		if (ret < 0) {
			unregister_kretprobe(&kernel_tlb_flush_probe);
			unregister_kretprobe(&user_enter_probe);
			unregister_kretprobe(&purge_probe);
			return ret;
		}
	}

	/* Start from an empty lazy-VA set so the first free stays below threshold. */
	vm_unmap_aliases();
	prime_areas = kcalloc(prime.nr_areas, sizeof(*prime_areas), GFP_KERNEL);
	if (!prime_areas) {
		ret = -ENOMEM;
		goto unregister_probes;
	}
	nr_prime_areas = prime.nr_areas;
	prime.areas = prime_areas;
	ret = smp_call_on_cpu(target_cpu, allocate_vmalloc, &prime, false);
	if (ret)
		goto free_prime;

	ret = smp_call_on_cpu(work_cpu, hold_vmalloc, &held, false);
	if (ret) {
		free_prime_areas();
		vm_unmap_aliases();
		goto free_prime;
	}
	held_area = held.area;

	pr_info("cpu_accel_vmalloc_purge_probe: primed %u MiB on cpu=%d; held %u MiB on cpu=%d\n",
		prime_mb, target_cpu, held_mb, work_cpu);
	return 0;

free_prime:
	kfree(prime_areas);
	prime_areas = NULL;
	nr_prime_areas = 0;
unregister_probes:
	if (wait_for_reentry) {
		unregister_kprobe(&sync_wait_begin_probe);
		unregister_kretprobe(&kernel_tlb_flush_probe);
		unregister_kretprobe(&user_enter_probe);
		unregister_kretprobe(&purge_probe);
	}
	return ret;
}

static void __exit cpu_accel_vmalloc_purge_probe_exit(void)
{
	WRITE_ONCE(purge_armed, wait_for_reentry);
	free_prime_areas();
	kfree(prime_areas);
	prime_areas = NULL;
	nr_prime_areas = 0;
	if (held_area) {
		vfree(held_area);
		held_area = NULL;
	}

	/* vm_unmap_aliases() waits for every queued purge helper to finish. */
	vm_unmap_aliases();
	WRITE_ONCE(purge_armed, false);
	if (wait_for_reentry) {
		unregister_kprobe(&sync_wait_begin_probe);
		unregister_kretprobe(&kernel_tlb_flush_probe);
		unregister_kretprobe(&user_enter_probe);
		unregister_kretprobe(&purge_probe);
		pr_info("cpu_accel_vmalloc_purge_probe: purge completed reentry_seen=%u reentry_timed_out=%u\n",
			READ_ONCE(reentry_seen), READ_ONCE(reentry_timed_out));
	} else {
		pr_info("cpu_accel_vmalloc_purge_probe: purge completed\n");
	}
}

module_init(cpu_accel_vmalloc_purge_probe_init);
module_exit(cpu_accel_vmalloc_purge_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe multi-CPU vmalloc lazy-area purge");
