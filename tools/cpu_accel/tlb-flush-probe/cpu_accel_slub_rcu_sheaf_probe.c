// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>

struct cpu_accel_rcu_sheaf_object {
	struct rcu_head rcu;
	unsigned long value;
};

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static struct kmem_cache *rcu_sheaf_cache;

static int prime_rcu_sheaf(void *unused)
{
	struct cpu_accel_rcu_sheaf_object *object;

	(void)unused;
	object = kmem_cache_alloc(rcu_sheaf_cache, GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	object->value = 1;
	kfree_rcu(object, rcu);
	return 0;
}

static int __init cpu_accel_slub_rcu_sheaf_probe_init(void)
{
	int ret;

	if (!IS_ENABLED(CONFIG_KVFREE_RCU_BATCHED))
		return -EOPNOTSUPP;
	if (target_cpu < 0 || target_cpu >= nr_cpu_ids || !cpu_online(target_cpu))
		return -ENXIO;

	rcu_sheaf_cache = kmem_cache_create("cpu_accel_rcu_sheaf_test",
					    sizeof(struct cpu_accel_rcu_sheaf_object),
					    0, SLAB_NO_MERGE, NULL);
	if (!rcu_sheaf_cache)
		return -ENOMEM;

	ret = smp_call_on_cpu(target_cpu, prime_rcu_sheaf, NULL, false);
	if (ret) {
		kmem_cache_destroy(rcu_sheaf_cache);
		rcu_sheaf_cache = NULL;
		return ret;
	}

	pr_info("cpu_accel_slub_rcu_sheaf_probe: primed cache on cpu=%d\n",
		target_cpu);
	return 0;
}

static void __exit cpu_accel_slub_rcu_sheaf_probe_exit(void)
{
	if (rcu_sheaf_cache)
		kmem_cache_destroy(rcu_sheaf_cache);

	pr_info("cpu_accel_slub_rcu_sheaf_probe: cache destroyed\n");
}

module_init(cpu_accel_slub_rcu_sheaf_probe_init);
module_exit(cpu_accel_slub_rcu_sheaf_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test SLUB RCU sheaf flush owner-stop handling");
