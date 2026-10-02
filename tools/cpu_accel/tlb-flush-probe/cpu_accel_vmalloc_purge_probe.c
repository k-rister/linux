// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/vmalloc.h>

struct cpu_accel_vmalloc_args {
	size_t bytes;
	void *area;
	bool free_after_alloc;
};

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static int work_cpu = 3;
module_param(work_cpu, int, 0444);

static unsigned int prime_mb = 96;
module_param(prime_mb, uint, 0444);

static unsigned int held_mb = 192;
module_param(held_mb, uint, 0444);

static void *held_area;

static int allocate_vmalloc(void *data)
{
	struct cpu_accel_vmalloc_args *args = data;

	args->area = vmalloc(args->bytes);
	if (!args->area)
		return -ENOMEM;

	*(volatile char *)args->area = 1;
	if (args->free_after_alloc) {
		vfree(args->area);
		args->area = NULL;
	}

	return 0;
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
		.bytes = (size_t)prime_mb << 20,
		.free_after_alloc = true,
	};
	struct cpu_accel_vmalloc_args held = {
		.bytes = (size_t)held_mb << 20,
	};
	int ret;

	if (target_cpu < 0 || work_cpu < 0 || target_cpu == work_cpu ||
	    target_cpu >= nr_cpu_ids || work_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu) || !cpu_online(work_cpu))
		return -EINVAL;
	if (!prime_mb || !held_mb)
		return -EINVAL;

	/* Start from an empty lazy-VA set so the first free stays below threshold. */
	vm_unmap_aliases();
	ret = smp_call_on_cpu(target_cpu, allocate_vmalloc, &prime, false);
	if (ret)
		return ret;

	ret = smp_call_on_cpu(work_cpu, hold_vmalloc, &held, false);
	if (ret) {
		vm_unmap_aliases();
		return ret;
	}
	held_area = held.area;

	pr_info("cpu_accel_vmalloc_purge_probe: primed %u MiB on cpu=%d; held %u MiB on cpu=%d\n",
		prime_mb, target_cpu, held_mb, work_cpu);
	return 0;
}

static void __exit cpu_accel_vmalloc_purge_probe_exit(void)
{
	if (held_area) {
		vfree(held_area);
		held_area = NULL;
	}

	/* vm_unmap_aliases() waits for every queued purge helper to finish. */
	vm_unmap_aliases();
	pr_info("cpu_accel_vmalloc_purge_probe: purge completed\n");
}

module_init(cpu_accel_vmalloc_purge_probe_init);
module_exit(cpu_accel_vmalloc_purge_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe multi-CPU vmalloc lazy-area purge");
