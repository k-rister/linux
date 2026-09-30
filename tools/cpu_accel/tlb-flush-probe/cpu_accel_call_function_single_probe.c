// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/smp.h>

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static atomic_t callback_count = ATOMIC_INIT(0);
static bool fire;

static void cpu_accel_call_function_single_probe_callback(void *data)
{
	atomic_inc(data);
}

static int probe_fire(const char *value,
		      const struct kernel_param *kp)
{
	bool trigger;
	int ret;

	(void)kp;
	ret = kstrtobool(value, &trigger);
	if (ret || !trigger)
		return ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu))
		return -ENXIO;

	atomic_set(&callback_count, 0);
	ret = smp_call_function_single(target_cpu,
				       cpu_accel_call_function_single_probe_callback,
				       &callback_count, 1);
	if (ret)
		return ret;
	if (atomic_read(&callback_count) != 1)
		return -EIO;

	pr_info("cpu_accel_call_function_single_probe: cpu=%d callback_count=1\n",
		target_cpu);
	return 0;
}

static const struct kernel_param_ops
cpu_accel_call_function_single_probe_ops = {
	.set = probe_fire,
};
module_param_cb(fire, &cpu_accel_call_function_single_probe_ops, &fire, 0200);

static int __init cpu_accel_call_function_single_probe_init(void)
{
	pr_info("cpu_accel_call_function_single_probe: loaded for cpu=%d\n",
		target_cpu);
	return 0;
}

static void __exit cpu_accel_call_function_single_probe_exit(void)
{
	pr_info("cpu_accel_call_function_single_probe: unloaded\n");
}

module_init(cpu_accel_call_function_single_probe_init);
module_exit(cpu_accel_call_function_single_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test synchronous single-CPU call-function owner stop");
