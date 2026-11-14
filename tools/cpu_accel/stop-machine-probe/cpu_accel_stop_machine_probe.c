// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/stop_machine.h>

#include <asm/cpu_accel.h>

static unsigned int target_cpu = 2;
module_param(target_cpu, uint, 0444);
MODULE_PARM_DESC(target_cpu,
		 "CPU whose user owner is checked around stop_machine()");

static bool inject_error;
module_param(inject_error, bool, 0644);
MODULE_PARM_DESC(inject_error,
		 "Make the stop_machine() callback return -EIO");

static bool fire;
static atomic_t callback_count = ATOMIC_INIT(0);

static int stop_machine_probe_fn(void *unused)
{
	atomic_inc(&callback_count);
	return inject_error ? -EIO : 0;
}

static int fire_set(const char *value, const struct kernel_param *kp)
{
	bool trigger;
	bool target_owner_before;
	bool target_owner_after;
	bool any_owner_before;
	bool any_owner_after;
	unsigned int callbacks;
	unsigned int online_cpus;
	int ret;

	(void)kp;

	ret = kstrtobool(value, &trigger);
	if (ret || !trigger)
		return ret;

	if (target_cpu >= nr_cpu_ids || !cpu_online(target_cpu))
		return -ENXIO;

	atomic_set(&callback_count, 0);
	target_owner_before = x86_cpu_accel_cpu_active_user_owner(target_cpu);
	any_owner_before = x86_cpu_accel_any_active();

	ret = stop_machine(stop_machine_probe_fn, NULL, cpu_online_mask);

	target_owner_after = x86_cpu_accel_cpu_active_user_owner(target_cpu);
	any_owner_after = x86_cpu_accel_any_active();
	callbacks = atomic_read(&callback_count);
	online_cpus = num_online_cpus();

	pr_info("cpu_accel_stop_machine_probe: target_cpu=%u ret=%d callback_count=%u online_cpus=%u target_owner_before=%d target_owner_after=%d any_owner_before=%d any_owner_after=%d\n",
		target_cpu, ret, callbacks, online_cpus,
		target_owner_before, target_owner_after,
		any_owner_before, any_owner_after);

	if (inject_error) {
		if (ret != -EIO)
			return -EINVAL;
	} else if (ret) {
		return ret;
	}

	if (callbacks != online_cpus)
		return -EIO;
	if (target_owner_after || any_owner_after)
		return -EBUSY;

	fire = false;
	return 0;
}

static const struct kernel_param_ops fire_param_ops = {
	.set = fire_set,
};
module_param_cb(fire, &fire_param_ops, &fire, 0200);
MODULE_PARM_DESC(fire, "Run stop_machine() on all online CPUs");

static int __init cpu_accel_stop_machine_probe_init(void)
{
	pr_info("cpu_accel_stop_machine_probe: loaded target_cpu=%u\n",
		target_cpu);
	return 0;
}

static void __exit cpu_accel_stop_machine_probe_exit(void)
{
	pr_info("cpu_accel_stop_machine_probe: unloaded\n");
}

module_init(cpu_accel_stop_machine_probe_init);
module_exit(cpu_accel_stop_machine_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test stop_machine() owner drain and fault propagation");
