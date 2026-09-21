// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpuhplock.h>
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
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include <uapi/linux/cpu_accel.h>

struct cpu_accel_device {
	struct miscdevice misc;
	struct mutex lock;
	wait_queue_head_t start_wait;
	struct completion run_done;
	struct task_struct *thread;
	struct cpu_accel_shared *shared;
	struct cpu_accel_config config;
	atomic_t opened;
	atomic_t start_requested;
	atomic_t stop_requested;
	atomic_t running;
	unsigned int sequence;
	bool configured;
};

static struct cpu_accel_device cpu_accel;

static void cpu_accel_reset_shared(struct cpu_accel_device *dev)
{
	memset(dev->shared, 0, CPU_ACCEL_MAP_SIZE);
	dev->shared->abi_version = CPU_ACCEL_ABI_VERSION;
	dev->shared->struct_size = sizeof(*dev->shared);
	WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_IDLE);
}

static void cpu_accel_run(struct cpu_accel_device *dev)
{
	struct cpu_accel_shared *shared = dev->shared;
	unsigned long irq_flags;
	u64 start, deadline, now, lateness;
	u64 samples = 0;
	u64 max_lateness = 0;
	u64 min_lateness = U64_MAX;
	u32 samples_valid = 0;
	u32 final_state = CPU_ACCEL_STATE_COMPLETE;

	preempt_disable();
	local_irq_save(irq_flags);

	if (atomic_read(&dev->stop_requested)) {
		final_state = CPU_ACCEL_STATE_STOPPED;
		goto out;
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

		if (atomic_read(&dev->stop_requested)) {
			final_state = CPU_ACCEL_STATE_STOPPED;
			break;
		}
		if (now - start >= dev->config.duration_ns)
			break;
		if (deadline > U64_MAX - dev->config.period_ns) {
			final_state = CPU_ACCEL_STATE_ERROR;
			break;
		}
		deadline += dev->config.period_ns;
	}

out:
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
	atomic_set(&dev->running, 0);
	smp_wmb();
	WRITE_ONCE(shared->state, final_state);

	local_irq_restore(irq_flags);
	preempt_enable();
	complete(&dev->run_done);
}

static int cpu_accel_thread(void *data)
{
	struct cpu_accel_device *dev = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(dev->start_wait,
			kthread_should_stop() ||
			atomic_read(&dev->start_requested));
		if (kthread_should_stop())
			break;
		if (!atomic_xchg(&dev->start_requested, 0))
			continue;
		cpu_accel_run(dev);
	}

	return 0;
}

static int cpu_accel_create_thread(struct cpu_accel_device *dev)
{
	if (dev->thread)
		return 0;

	dev->thread = kthread_create_on_cpu(cpu_accel_thread, dev, dev->config.cpu,
					   "cpu_accel");
	if (IS_ERR(dev->thread)) {
		int ret = PTR_ERR(dev->thread);

		dev->thread = NULL;
		return ret;
	}

	wake_up_process(dev->thread);
	return 0;
}

static void cpu_accel_destroy_thread(struct cpu_accel_device *dev)
{
	if (!dev->thread)
		return;

	kthread_stop(dev->thread);
	dev->thread = NULL;
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

	mutex_lock(&dev->lock);
	if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY ||
	    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING) {
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
		wake_up(&dev->start_wait);
		wait_for_completion_timeout(&dev->run_done,
					    msecs_to_jiffies(6000));
	}
	cpu_accel_destroy_thread(dev);
	mutex_unlock(&dev->lock);

	atomic_set(&dev->opened, 0);
	return 0;
}

static int cpu_accel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cpu_accel_device *dev = file->private_data;

	if (vma->vm_pgoff || vma->vm_end - vma->vm_start != CPU_ACCEL_MAP_SIZE)
		return -EINVAL;

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return remap_vmalloc_range(vma, dev->shared, 0);
}

static long cpu_accel_ioctl(struct file *file, unsigned int command,
				    unsigned long argument)
{
	struct cpu_accel_device *dev = file->private_data;
	struct cpu_accel_config config;
	int current_cpu;
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
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY) {
			ret = -EBUSY;
			break;
		}
		if (!config.flags)
			config.flags = CPU_ACCEL_FLAG_IRQS_OFF;
		if (config.flags != CPU_ACCEL_FLAG_IRQS_OFF ||
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

		cpu_accel_destroy_thread(dev);
		dev->config = config;
		dev->configured = true;
		dev->shared->cpu = config.cpu;
		dev->shared->flags = config.flags;
		dev->shared->duration_ns = config.duration_ns;
		dev->shared->period_ns = config.period_ns;
		WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_IDLE);
		break;

	case CPU_ACCEL_IOC_START:
		if (!dev->configured ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY) {
			ret = -EINVAL;
			break;
		}

		current_cpu = get_cpu();
		if (current_cpu == dev->config.cpu) {
			put_cpu();
			ret = -EBUSY;
			break;
		}
		put_cpu();

		cpus_read_lock();
		if (!cpu_online(dev->config.cpu))
			ret = -ENODEV;
		cpus_read_unlock();
		if (ret)
			break;

		ret = cpu_accel_create_thread(dev);
		if (ret)
			break;
		dev->sequence++;
		dev->shared->sequence = dev->sequence;
		dev->shared->samples_produced = 0;
		dev->shared->samples_valid = 0;
		dev->shared->stop_requested = 0;
		reinit_completion(&dev->run_done);
		atomic_set(&dev->stop_requested, 0);
		atomic_set(&dev->start_requested, 1);
		WRITE_ONCE(dev->shared->state, CPU_ACCEL_STATE_READY);
		wake_up(&dev->start_wait);
		wake_up_process(dev->thread);
		break;

	case CPU_ACCEL_IOC_STOP:
		if (READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_RUNNING &&
		    READ_ONCE(dev->shared->state) != CPU_ACCEL_STATE_READY) {
			ret = -EALREADY;
			break;
		}
		atomic_set(&dev->stop_requested, 1);
		WRITE_ONCE(dev->shared->stop_requested, 1);
		wake_up(&dev->start_wait);
		break;

	case CPU_ACCEL_IOC_RESET:
		if (READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_RUNNING ||
		    READ_ONCE(dev->shared->state) == CPU_ACCEL_STATE_READY) {
			ret = -EBUSY;
			break;
		}
		cpu_accel_destroy_thread(dev);
		dev->configured = false;
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
	init_waitqueue_head(&cpu_accel.start_wait);
	init_completion(&cpu_accel.run_done);
	atomic_set(&cpu_accel.opened, 0);
	atomic_set(&cpu_accel.start_requested, 0);
	atomic_set(&cpu_accel.stop_requested, 0);
	atomic_set(&cpu_accel.running, 0);
	cpu_accel.shared = vzalloc(CPU_ACCEL_MAP_SIZE);
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

	pr_info("single-CPU accelerator prototype loaded\n");
	return 0;
}

static void __exit cpu_accel_exit(void)
{
	mutex_lock(&cpu_accel.lock);
	if (cpu_accel.thread) {
		atomic_set(&cpu_accel.stop_requested, 1);
		wake_up(&cpu_accel.start_wait);
		wait_for_completion_timeout(&cpu_accel.run_done,
					    msecs_to_jiffies(6000));
		cpu_accel_destroy_thread(&cpu_accel);
	}
	misc_deregister(&cpu_accel.misc);
	vfree(cpu_accel.shared);
	cpu_accel.shared = NULL;
	mutex_unlock(&cpu_accel.lock);
}

module_init(cpu_accel_init);
module_exit(cpu_accel_exit);

MODULE_DESCRIPTION("Single-CPU accelerator foundation prototype");
MODULE_LICENSE("GPL");
