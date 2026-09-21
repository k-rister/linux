// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/processor.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include <uapi/linux/cpu_accel.h>

struct cpu_accel_device {
	struct miscdevice misc;
	/* Serializes control-plane state transitions. */
	struct mutex lock;
	struct completion lifecycle_done;
	struct task_struct *lifecycle_thread;
	struct timer_list watchdog_timer;
	struct cpu_accel_shared *shared;
	struct cpu_accel_config config;
	atomic_t opened;
	atomic_t enter_requested;
	atomic_t stop_requested;
	atomic_t watchdog_fired;
	atomic_t running;
	unsigned int sequence;
	unsigned int controller_cpu;
	int lifecycle_ret;
	bool configured;
};

static struct cpu_accel_device cpu_accel;

static void cpu_accel_watchdog(struct timer_list *timer)
{
	struct cpu_accel_device *dev =
		container_of(timer, struct cpu_accel_device, watchdog_timer);

	if (!atomic_read(&dev->enter_requested))
		return;

	atomic_set(&dev->watchdog_fired, 1);
	atomic_set(&dev->stop_requested, 1);
	WRITE_ONCE(dev->shared->stop_requested, 1);
}

static void cpu_accel_reset_shared(struct cpu_accel_device *dev)
{
	memset(dev->shared, 0, CPU_ACCEL_MAP_SIZE);
	dev->shared->abi_version = CPU_ACCEL_ABI_VERSION;
	dev->shared->struct_size = sizeof(*dev->shared);
	WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_IDLE);
}

/*
 * This is the first architecture-neutral accelerator workload.  The
 * lifecycle backend dispatches this function synchronously to the target CPU.
 * The target stays online, but does not return to the scheduler while this
 * function is running.
 */
static void cpu_accel_run(struct cpu_accel_device *dev)
{
	struct cpu_accel_shared *shared = dev->shared;
	u64 start = 0, deadline = 0, now, lateness;
	u64 samples = 0;
	u64 max_lateness = 0;
	u64 min_lateness = U64_MAX;
	u32 samples_valid = 0;
	u32 final_state = CPU_ACCEL_STATE_COMPLETE;
	bool persistent = dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT;

	if (atomic_read(&dev->stop_requested)) {
		final_state = atomic_read(&dev->watchdog_fired) ?
			CPU_ACCEL_STATE_WATCHDOG : CPU_ACCEL_STATE_STOPPED;
		goto finish;
	}

	atomic_set(&dev->running, 1);
	WRITE_ONCE(shared->state, CPU_ACCEL_STATE_RUNNING);
	start = ktime_get_mono_fast_ns();
	shared->start_ns = start;
	deadline = start + dev->config.period_ns;

	for (;;) {
		now = ktime_get_mono_fast_ns();
		if (now < deadline) {
			cpu_relax();
			continue;
		}

		lateness = now - deadline;
		if (samples_valid < CPU_ACCEL_MAX_SAMPLES) {
			shared->samples[samples_valid].timestamp_ns = now;
			shared->samples[samples_valid].lateness_ns = lateness;
			samples_valid++;
		}
		samples++;
		max_lateness = max(max_lateness, lateness);
		min_lateness = min(min_lateness, lateness);

		if (atomic_read(&dev->watchdog_fired)) {
			final_state = CPU_ACCEL_STATE_WATCHDOG;
			break;
		}
		if (atomic_read(&dev->stop_requested)) {
			final_state = CPU_ACCEL_STATE_STOPPED;
			break;
		}
		if (now - start >= dev->config.duration_ns) {
			final_state = persistent ? CPU_ACCEL_STATE_WATCHDOG :
				CPU_ACCEL_STATE_COMPLETE;
			break;
		}
		if (deadline > U64_MAX - dev->config.period_ns) {
			final_state = CPU_ACCEL_STATE_ERROR;
			break;
		}
		deadline += dev->config.period_ns;
	}

finish:
	now = ktime_get_mono_fast_ns();
	shared->end_ns = now;
	shared->samples_produced = samples;
	shared->samples_valid = samples_valid;
	shared->max_lateness_ns = max_lateness;
	shared->min_lateness_ns = samples ? min_lateness : 0;
	shared->last_lateness_ns = samples_valid ?
		shared->samples[samples_valid - 1].lateness_ns : 0;
	shared->stop_requested = 0;
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->running, 0);
	/* Publish all result fields before publishing the terminal state. */
	smp_wmb();
	WRITE_ONCE(shared->state, final_state);
}

static void cpu_accel_lifecycle_entry(void *data)
{
	struct cpu_accel_device *dev = data;
	unsigned long irq_flags;

	preempt_disable();
	local_irq_save(irq_flags);
	cpu_accel_run(dev);
	local_irq_restore(irq_flags);
	preempt_enable();
}

/*
 * Keep this dispatch in one place.  Future architecture backends can replace
 * the synchronous SMP call with a direct accelerator entry/exit mechanism
 * without changing the control plane or workload implementation.
 */
static int cpu_accel_lifecycle_enter(struct cpu_accel_device *dev)
{
	return smp_call_function_single(dev->config.cpu,
				       cpu_accel_lifecycle_entry, dev, 1);
}

static int cpu_accel_lifecycle_thread(void *data)
{
	struct cpu_accel_device *dev = data;
	int ret;

	/* Hold the CPU-hotplug read lock for the complete accelerator interval. */
	cpus_read_lock();
	ret = cpu_accel_lifecycle_enter(dev);
	cpus_read_unlock();
	dev->lifecycle_ret = ret;
	atomic_set(&dev->enter_requested, 0);
	if (ret) {
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING)
			WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_ERROR);
	}
	complete(&dev->lifecycle_done);
	module_put(THIS_MODULE);
	return 0;
}

static int cpu_accel_create_lifecycle_thread(struct cpu_accel_device *dev)
{
	if (dev->lifecycle_thread)
		return -EBUSY;
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	dev->lifecycle_thread = kthread_create(cpu_accel_lifecycle_thread, dev,
					       "cpu_accel_ctl");
	if (IS_ERR(dev->lifecycle_thread)) {
		int ret = PTR_ERR(dev->lifecycle_thread);

		dev->lifecycle_thread = NULL;
		module_put(THIS_MODULE);
		return ret;
	}

	kthread_bind(dev->lifecycle_thread, dev->controller_cpu);
	wake_up_process(dev->lifecycle_thread);
	return 0;
}

static void cpu_accel_destroy_lifecycle_thread(struct cpu_accel_device *dev)
{
	if (!dev->lifecycle_thread)
		return;

	kthread_stop(dev->lifecycle_thread);
	dev->lifecycle_thread = NULL;
}

static int cpu_accel_lifecycle_exit(struct cpu_accel_device *dev)
{
	if (dev->lifecycle_thread) {
		if (!wait_for_completion_timeout(&dev->lifecycle_done,
						 msecs_to_jiffies(6000)))
			return -ETIMEDOUT;

		timer_delete_sync(&dev->watchdog_timer);
		cpu_accel_destroy_lifecycle_thread(dev);
	}

	return dev->lifecycle_ret;
}

static int cpu_accel_open(struct inode *inode, struct file *file)
{
	if (atomic_cmpxchg(&cpu_accel.opened, 0, 1))
		return -EBUSY;

	file->private_data = &cpu_accel;
	return 0;
}

static int cpu_accel_release(struct inode *inode, struct file *file)
{
	struct cpu_accel_device *dev = file->private_data;
	int ret;

	mutex_lock(&dev->lock);
	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
	}
	if (dev->lifecycle_thread) {
		ret = cpu_accel_lifecycle_exit(dev);
		if (ret)
			pr_err("unable to exit accelerator CPU %u after release: %d\n",
			       dev->config.cpu, ret);
	}
	mutex_unlock(&dev->lock);

	atomic_set(&dev->opened, 0);
	return 0;
}

static int cpu_accel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cpu_accel_device *dev = file->private_data;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != CPU_ACCEL_MAP_SIZE)
		return -EINVAL;

	return remap_vmalloc_range(vma, dev->shared, 0);
}

static int cpu_accel_start_locked(struct cpu_accel_device *dev)
{
	int current_cpu;
	int ret;

	if (!dev->configured || dev->lifecycle_thread)
		return -EINVAL;
	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING)
		return -EBUSY;

	current_cpu = get_cpu();
	if (current_cpu == dev->config.cpu) {
		put_cpu();
		return -EBUSY;
	}
	dev->controller_cpu = current_cpu;
	put_cpu();

	cpus_read_lock();
	ret = cpu_online(dev->config.cpu) ? 0 : -ENODEV;
	cpus_read_unlock();
	if (ret)
		return ret;

	dev->sequence++;
	dev->shared->sequence = dev->sequence;
	dev->shared->samples_produced = 0;
	dev->shared->samples_valid = 0;
	dev->shared->stop_requested = 0;
	dev->shared->start_ns = 0;
	dev->shared->end_ns = 0;
	dev->shared->max_lateness_ns = 0;
	dev->shared->min_lateness_ns = 0;
	dev->shared->last_lateness_ns = 0;
	reinit_completion(&dev->lifecycle_done);
	dev->lifecycle_ret = -EINPROGRESS;
	atomic_set(&dev->stop_requested, 0);
	atomic_set(&dev->watchdog_fired, 0);
	atomic_set(&dev->enter_requested, 1);
	WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_READY);

	ret = cpu_accel_create_lifecycle_thread(dev);
	if (ret) {
		atomic_set(&dev->enter_requested, 0);
		WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_ERROR);
	} else if (dev->config.flags & CPU_ACCEL_FLAG_PERSISTENT) {
		unsigned long watchdog_jiffies;

		watchdog_jiffies = max_t(unsigned long, 1,
					 nsecs_to_jiffies(dev->config.duration_ns));
		mod_timer(&dev->watchdog_timer, jiffies + watchdog_jiffies);
	}
	return ret;
}

static long cpu_accel_ioctl(struct file *file, unsigned int command,
			    unsigned long argument)
{
	struct cpu_accel_device *dev = file->private_data;
	struct cpu_accel_config config;
	int ret = 0;

	if (_IOC_TYPE(command) != CPU_ACCEL_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&dev->lock);
	switch (command) {
	case CPU_ACCEL_IOC_CONFIG:
		if (copy_from_user(&config, (void __user *)argument,
				   sizeof(config))) {
			ret = -EFAULT;
			break;
		}
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread) {
			ret = -EBUSY;
			break;
		}
		if (!config.flags)
			config.flags = CPU_ACCEL_FLAG_IRQS_OFF;
		if (!(config.flags & CPU_ACCEL_FLAG_IRQS_OFF) ||
		    config.flags & ~(CPU_ACCEL_FLAG_IRQS_OFF |
				     CPU_ACCEL_FLAG_PERSISTENT) ||
		    !config.period_ns || !config.duration_ns ||
		    config.duration_ns > CPU_ACCEL_MAX_DURATION_NS ||
		    config.cpu >= nr_cpu_ids || config.cpu == 0) {
			ret = -EINVAL;
			break;
		}

		cpus_read_lock();
		if (!cpu_online(config.cpu))
			ret = -ENODEV;
		cpus_read_unlock();
		if (ret)
			break;

		dev->config = config;
		dev->configured = true;
		cpu_accel_reset_shared(dev);
		dev->shared->cpu = config.cpu;
		dev->shared->flags = config.flags;
		dev->shared->duration_ns = config.duration_ns;
		dev->shared->period_ns = config.period_ns;
		break;

	case CPU_ACCEL_IOC_START:
		ret = cpu_accel_start_locked(dev);
		break;

	case CPU_ACCEL_IOC_STOP:
		if (READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_RUNNING &&
		    READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_READY) {
			ret = -EALREADY;
			break;
		}
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
		break;

	case CPU_ACCEL_IOC_EXIT:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY) {
			ret = -EBUSY;
			break;
		}
		ret = cpu_accel_lifecycle_exit(dev);
		break;

	case CPU_ACCEL_IOC_RESET:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
		    dev->lifecycle_thread) {
			ret = -EBUSY;
			break;
		}
		dev->configured = false;
		dev->sequence = 0;
		cpu_accel_reset_shared(dev);
		break;

	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&dev->lock);
	return ret;
}

static const struct file_operations cpu_accel_fops = {
	.owner		= THIS_MODULE,
	.open		= cpu_accel_open,
	.release	= cpu_accel_release,
	.mmap		= cpu_accel_mmap,
	.unlocked_ioctl	= cpu_accel_ioctl,
};

static int __init cpu_accel_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct cpu_accel_shared) > CPU_ACCEL_MAP_SIZE);

	mutex_init(&cpu_accel.lock);
	init_completion(&cpu_accel.lifecycle_done);
	atomic_set(&cpu_accel.opened, 0);
	atomic_set(&cpu_accel.enter_requested, 0);
	atomic_set(&cpu_accel.stop_requested, 0);
	atomic_set(&cpu_accel.watchdog_fired, 0);
	atomic_set(&cpu_accel.running, 0);
	timer_setup(&cpu_accel.watchdog_timer, cpu_accel_watchdog, 0);
	cpu_accel.shared = vmalloc_user(CPU_ACCEL_MAP_SIZE);
	if (!cpu_accel.shared)
		return -ENOMEM;
	cpu_accel_reset_shared(&cpu_accel);

	cpu_accel.misc.minor = MISC_DYNAMIC_MINOR;
	cpu_accel.misc.name = "cpu_accel";
	cpu_accel.misc.fops = &cpu_accel_fops;
	cpu_accel.misc.mode = 0600;
	ret = misc_register(&cpu_accel.misc);
	if (ret) {
		vfree(cpu_accel.shared);
		return ret;
	}

	pr_info("single-CPU accelerator lifecycle prototype loaded\n");
	return 0;
}

static void __exit cpu_accel_exit(void)
{
	mutex_lock(&cpu_accel.lock);
	if (READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(cpu_accel.shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&cpu_accel.stop_requested, 1);
		WRITE_ONCE(cpu_accel.shared->stop_requested, 1);
	}
	if (cpu_accel.lifecycle_thread)
		cpu_accel_lifecycle_exit(&cpu_accel);
	timer_delete_sync(&cpu_accel.watchdog_timer);
	mutex_unlock(&cpu_accel.lock);

	misc_deregister(&cpu_accel.misc);
	vfree(cpu_accel.shared);
	cpu_accel.shared = NULL;
}

module_init(cpu_accel_init);
module_exit(cpu_accel_exit);

MODULE_DESCRIPTION("Single-CPU accelerator lifecycle foundation prototype");
MODULE_LICENSE("GPL");
