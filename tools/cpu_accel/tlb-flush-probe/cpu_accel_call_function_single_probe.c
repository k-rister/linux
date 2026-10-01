// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static atomic_t callback_count = ATOMIC_INIT(0);
static bool fire;
static bool fire_reuse;
static bool fire_on_cpu;
static bool fire_work_on_cpu;

static void cpu_accel_call_function_single_probe_callback(void *data)
{
	atomic_inc(data);
}

static int cpu_accel_smp_call_on_cpu_probe_callback(void *data)
{
	atomic_inc(data);
	return 0;
}

static long cpu_accel_work_on_cpu_probe_callback(void *data)
{
	atomic_inc(data);
	return 0;
}

static int probe_run(const char *value, const struct kernel_param *kp,
		     bool reuse)
{
	bool trigger;
	int expected_count;
	int ret;

	(void)kp;
	ret = kstrtobool(value, &trigger);
	if (ret || !trigger)
		return ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu))
		return -ENXIO;

	atomic_set(&callback_count, 0);
	if (reuse) {
		ret = smp_call_function_single(target_cpu,
					       cpu_accel_call_function_single_probe_callback,
					       &callback_count, 0);
		if (ret)
			return ret;
		msleep(50);
		ret = smp_call_function_single(target_cpu,
					       cpu_accel_call_function_single_probe_callback,
					       &callback_count, 0);
		expected_count = 3;
	} else {
		ret = smp_call_function_single(target_cpu,
					       cpu_accel_call_function_single_probe_callback,
					       &callback_count, 1);
		expected_count = 1;
	}
	if (ret)
		return ret;

	if (reuse) {
		ret = smp_call_function_single(target_cpu,
					       cpu_accel_call_function_single_probe_callback,
					       &callback_count, 1);
		if (ret)
			return ret;
	}
	if (atomic_read(&callback_count) != expected_count)
		return -EIO;

	pr_info("cpu_accel_call_function_single_probe: cpu=%d mode=%s callback_count=%d\n",
		target_cpu, reuse ? "async-reuse" : "sync", expected_count);
	return 0;
}

static int probe_fire(const char *value, const struct kernel_param *kp)
{
	return probe_run(value, kp, false);
}

static int probe_fire_reuse(const char *value, const struct kernel_param *kp)
{
	return probe_run(value, kp, true);
}

static int probe_fire_on_cpu(const char *value, const struct kernel_param *kp)
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
	ret = smp_call_on_cpu(target_cpu,
			      cpu_accel_smp_call_on_cpu_probe_callback,
			      &callback_count, false);
	if (ret)
		return ret;
	if (atomic_read(&callback_count) != 1)
		return -EIO;

	pr_info("cpu_accel_call_function_single_probe: cpu=%d mode=smp-call-on-cpu callback_count=1\n",
		target_cpu);
	return 0;
}

static int probe_fire_work_on_cpu(const char *value,
				  const struct kernel_param *kp)
{
	bool trigger;
	long ret;

	(void)kp;
	ret = kstrtobool(value, &trigger);
	if (ret || !trigger)
		return ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu))
		return -ENXIO;

	atomic_set(&callback_count, 0);
	ret = work_on_cpu(target_cpu, cpu_accel_work_on_cpu_probe_callback,
			  &callback_count);
	if (ret)
		return ret;
	if (atomic_read(&callback_count) != 1)
		return -EIO;

	pr_info("cpu_accel_call_function_single_probe: cpu=%d mode=work-on-cpu callback_count=1\n",
		target_cpu);
	return 0;
}

static const struct kernel_param_ops
cpu_accel_call_function_single_probe_ops = {
	.set = probe_fire,
};
module_param_cb(fire, &cpu_accel_call_function_single_probe_ops, &fire, 0200);

static const struct kernel_param_ops
cpu_accel_call_function_single_probe_reuse_ops = {
	.set = probe_fire_reuse,
};
module_param_cb(fire_reuse, &cpu_accel_call_function_single_probe_reuse_ops,
		&fire_reuse, 0200);

static const struct kernel_param_ops
cpu_accel_call_function_single_probe_on_cpu_ops = {
	.set = probe_fire_on_cpu,
};
module_param_cb(fire_on_cpu, &cpu_accel_call_function_single_probe_on_cpu_ops,
		&fire_on_cpu, 0200);

static const struct kernel_param_ops
cpu_accel_work_on_cpu_probe_ops = {
	.set = probe_fire_work_on_cpu,
};
module_param_cb(fire_work_on_cpu, &cpu_accel_work_on_cpu_probe_ops,
		&fire_work_on_cpu, 0200);

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
