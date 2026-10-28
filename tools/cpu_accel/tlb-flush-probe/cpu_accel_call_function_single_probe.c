// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/module.h>
#include <linux/param.h>
#include <linux/perf_event.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static atomic_t callback_count = ATOMIC_INIT(0);
static atomic_t irq_work_callback_count = ATOMIC_INIT(0);
static atomic_t irq_work_overflow_count = ATOMIC_INIT(0);
static atomic_t irq_work_queue_count = ATOMIC_INIT(0);
static atomic_t irq_work_probe_armed = ATOMIC_INIT(0);
static bool fire;
static bool fire_reuse;
static bool fire_on_cpu;
static bool fire_work_on_cpu;
static bool fire_irq_work;
static bool irq_work_test;
static struct perf_event *irq_work_perf_event;

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

static void cpu_accel_irq_work_probe_callback(struct irq_work *work)
{
	(void)work;
	atomic_inc(&irq_work_callback_count);
}

static DEFINE_IRQ_WORK(cpu_accel_irq_work_probe,
		       cpu_accel_irq_work_probe_callback);

static void cpu_accel_irq_work_probe_overflow(struct perf_event *event,
					       struct perf_sample_data *data,
					       struct pt_regs *regs)
{
	(void)event;
	(void)data;
	(void)regs;
	if (!atomic_read(&irq_work_probe_armed))
		return;
	atomic_inc(&irq_work_overflow_count);
	if (irq_work_queue(&cpu_accel_irq_work_probe))
		atomic_inc(&irq_work_queue_count);
}

static void cpu_accel_irq_work_probe_stop(void)
{
	atomic_set(&irq_work_probe_armed, 0);
	if (irq_work_perf_event)
		perf_event_disable(irq_work_perf_event);
	irq_work_sync(&cpu_accel_irq_work_probe);
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

static int probe_fire_irq_work(const char *value,
			       const struct kernel_param *kp)
{
	bool trigger;
	int overflow_count;
	int queue_count;
	int callback_count_while_owner;
	int callback_count_after_exit;
	int ret;

	(void)kp;
	ret = kstrtobool(value, &trigger);
	if (ret || !trigger)
		return ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
	    !cpu_online(target_cpu))
		return -ENXIO;
	if (!irq_work_test || !irq_work_perf_event)
		return -EOPNOTSUPP;

	atomic_set(&callback_count, 0);
	atomic_set(&irq_work_callback_count, 0);
	atomic_set(&irq_work_overflow_count, 0);
	atomic_set(&irq_work_queue_count, 0);
	atomic_set(&irq_work_probe_armed, 1);

	/* A PMU NMI queues local IRQ work while the target is in ring 3. */
	msleep(100);
	overflow_count = atomic_read(&irq_work_overflow_count);
	queue_count = atomic_read(&irq_work_queue_count);
	callback_count_while_owner = atomic_read(&irq_work_callback_count);
	if (!overflow_count || !queue_count || callback_count_while_owner) {
		cpu_accel_irq_work_probe_stop();
		return -EIO;
	}

	/* The synchronous call requests owner exit before it waits. */
	ret = smp_call_function_single(target_cpu,
				       cpu_accel_call_function_single_probe_callback,
				       &callback_count, 1);
	if (ret) {
		cpu_accel_irq_work_probe_stop();
		return ret;
	}

	cpu_accel_irq_work_probe_stop();
	callback_count_after_exit = atomic_read(&irq_work_callback_count);
	if (atomic_read(&callback_count) != 1 ||
	    callback_count_after_exit < queue_count)
		return -EIO;

	pr_info("cpu_accel_call_function_single_probe: cpu=%d mode=irq-work "
		"nmi_overflows=%d irq_work_queued=%d "
		"irq_work_callback_count_while_owner=%d "
		"irq_work_callback_count_after_exit=%d callback_count=%d\n",
		target_cpu, overflow_count, queue_count,
		callback_count_while_owner,
		callback_count_after_exit, atomic_read(&callback_count));
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

static const struct kernel_param_ops
cpu_accel_irq_work_probe_ops = {
	.set = probe_fire_irq_work,
};
module_param_cb(fire_irq_work, &cpu_accel_irq_work_probe_ops,
		&fire_irq_work, 0200);
module_param(irq_work_test, bool, 0444);

static int __init cpu_accel_call_function_single_probe_init(void)
{
	struct perf_event_attr attr = {};
	int ret;

	if (irq_work_test) {
		if (target_cpu < 0 || target_cpu >= nr_cpu_ids ||
		    !cpu_online(target_cpu))
			return -ENXIO;

		attr.type = PERF_TYPE_HARDWARE;
		attr.size = sizeof(attr);
		attr.config = PERF_COUNT_HW_CPU_CYCLES;
		attr.sample_period = 10000000;
		attr.pinned = 1;
		attr.disabled = 1;
		irq_work_perf_event = perf_event_create_kernel_counter(
			&attr, target_cpu, NULL,
			cpu_accel_irq_work_probe_overflow, NULL);
		if (IS_ERR(irq_work_perf_event)) {
			ret = PTR_ERR(irq_work_perf_event);
			irq_work_perf_event = NULL;
			return ret;
		}
		perf_event_enable(irq_work_perf_event);
	}

	pr_info("cpu_accel_call_function_single_probe: loaded for cpu=%d\n",
		target_cpu);
	return 0;
}

static void __exit cpu_accel_call_function_single_probe_exit(void)
{
	cpu_accel_irq_work_probe_stop();
	if (irq_work_perf_event)
		perf_event_release_kernel(irq_work_perf_event);
	pr_info("cpu_accel_call_function_single_probe: unloaded\n");
}

module_init(cpu_accel_call_function_single_probe_init);
module_exit(cpu_accel_call_function_single_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test single-CPU call-function and IRQ-work owner handling");
