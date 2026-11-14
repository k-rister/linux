// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define CPU_ACCEL_RCU_MAX_CALLBACKS 256

struct cpu_accel_rcu_probe_callback {
	struct rcu_head head;
};

static unsigned int target_cpu = 2;
module_param(target_cpu, uint, 0444);
MODULE_PARM_DESC(target_cpu, "CPU on which to queue RCU callbacks");

static unsigned int reader_cpu;
module_param(reader_cpu, uint, 0444);
MODULE_PARM_DESC(reader_cpu, "CPU on which to hold an RCU read-side section");

static struct cpu_accel_rcu_probe_callback
	callbacks[CPU_ACCEL_RCU_MAX_CALLBACKS];
static atomic_t callback_queued = ATOMIC_INIT(0);
static atomic_t callback_completed = ATOMIC_INIT(0);
static atomic_t target_callback_count = ATOMIC_INIT(0);
static atomic64_t callback_first_ns = ATOMIC64_INIT(0);
static atomic64_t callback_last_ns = ATOMIC64_INIT(0);
static atomic64_t target_callback_first_ns = ATOMIC64_INIT(0);
static atomic64_t target_callback_last_ns = ATOMIC64_INIT(0);
static DEFINE_SPINLOCK(callback_cpu_lock);
static cpumask_t callback_cpus;

static DECLARE_WAIT_QUEUE_HEAD(reader_wait);
static DECLARE_COMPLETION(reader_started);
static struct task_struct *reader_task;
static bool reader_hold;
static bool reader_active;

static struct dentry *probe_dir;

static void atomic64_record_min(atomic64_t *value, u64 sample)
{
	s64 old = atomic64_read(value);

	while ((!old || sample < old) &&
	       atomic64_cmpxchg(value, old, sample) != old)
		old = atomic64_read(value);
}

static void atomic64_record_max(atomic64_t *value, u64 sample)
{
	s64 old = atomic64_read(value);

	while (sample > old && atomic64_cmpxchg(value, old, sample) != old)
		old = atomic64_read(value);
}

static void cpu_accel_rcu_probe_callback(struct rcu_head *head)
{
	unsigned int cpu = raw_smp_processor_id();
	u64 now = ktime_get_mono_fast_ns();
	unsigned long flags;

	(void)head;
	atomic64_record_min(&callback_first_ns, now);
	atomic64_record_max(&callback_last_ns, now);
	spin_lock_irqsave(&callback_cpu_lock, flags);
	cpumask_set_cpu(cpu, &callback_cpus);
	spin_unlock_irqrestore(&callback_cpu_lock, flags);
	if (cpu == READ_ONCE(target_cpu)) {
		atomic_inc(&target_callback_count);
		atomic64_record_min(&target_callback_first_ns, now);
		atomic64_record_max(&target_callback_last_ns, now);
	}
	atomic_inc(&callback_completed);
}

static int cpu_accel_rcu_reader(void *unused)
{
	(void)unused;
	while (!kthread_should_stop()) {
		wait_event(reader_wait,
			   READ_ONCE(reader_hold) || kthread_should_stop());
		if (kthread_should_stop())
			break;

		rcu_read_lock();
		WRITE_ONCE(reader_active, true);
		complete(&reader_started);
		while (READ_ONCE(reader_hold) && !kthread_should_stop())
			cpu_relax();
		rcu_read_unlock();
		WRITE_ONCE(reader_active, false);
		wake_up_all(&reader_wait);
	}
	return 0;
}

static int copy_u32_from_user(const char __user *user_buf, size_t count,
			      unsigned int *value)
{
	char input[32];

	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, user_buf, count))
		return -EFAULT;
	input[count] = '\0';
	return kstrtouint(input, 0, value);
}

static ssize_t hold_write(struct file *file, const char __user *user_buf,
			  size_t count, loff_t *ppos)
{
	unsigned int value;
	int ret;

	(void)file;
	(void)ppos;
	ret = copy_u32_from_user(user_buf, count, &value);
	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	if (READ_ONCE(reader_hold) || READ_ONCE(reader_active))
		return -EBUSY;

	reinit_completion(&reader_started);
	WRITE_ONCE(reader_hold, true);
	wake_up_all(&reader_wait);
	if (!wait_for_completion_timeout(&reader_started,
					msecs_to_jiffies(1000))) {
		WRITE_ONCE(reader_hold, false);
		wake_up_all(&reader_wait);
		return -ETIMEDOUT;
	}
	return count;
}

static ssize_t release_write(struct file *file, const char __user *user_buf,
			     size_t count, loff_t *ppos)
{
	unsigned int value;
	int ret;

	(void)file;
	(void)ppos;
	ret = copy_u32_from_user(user_buf, count, &value);
	if (ret)
		return ret;
	if (value != 1)
		return -EINVAL;
	if (!READ_ONCE(reader_hold))
		return -EINVAL;

	WRITE_ONCE(reader_hold, false);
	wake_up_all(&reader_wait);
	if (!wait_event_timeout(reader_wait, !READ_ONCE(reader_active),
				msecs_to_jiffies(1000)))
		return -ETIMEDOUT;
	return count;
}

static long queue_callbacks_on_cpu(void *data)
{
	unsigned int count = *(unsigned int *)data;
	unsigned int i;

	if (raw_smp_processor_id() != target_cpu)
		return -EXDEV;
	for (i = 0; i < count; i++)
		call_rcu(&callbacks[i].head, cpu_accel_rcu_probe_callback);
	return count;
}

static ssize_t queue_write(struct file *file, const char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	unsigned int callback_count;
	unsigned long flags;
	long ret;
	int parse_ret;

	(void)file;
	(void)ppos;
	parse_ret = copy_u32_from_user(user_buf, count, &callback_count);
	if (parse_ret)
		return parse_ret;
	if (!callback_count || callback_count > CPU_ACCEL_RCU_MAX_CALLBACKS)
		return -ERANGE;
	if (!READ_ONCE(reader_active) || !READ_ONCE(reader_hold))
		return -EINVAL;
	if (atomic_read(&callback_queued) != atomic_read(&callback_completed))
		return -EBUSY;
	if (!cpu_online(target_cpu))
		return -ENXIO;

	atomic_set(&callback_queued, callback_count);
	atomic_set(&callback_completed, 0);
	atomic_set(&target_callback_count, 0);
	atomic64_set(&callback_first_ns, 0);
	atomic64_set(&callback_last_ns, 0);
	atomic64_set(&target_callback_first_ns, 0);
	atomic64_set(&target_callback_last_ns, 0);
	spin_lock_irqsave(&callback_cpu_lock, flags);
	cpumask_clear(&callback_cpus);
	spin_unlock_irqrestore(&callback_cpu_lock, flags);

	ret = work_on_cpu(target_cpu, queue_callbacks_on_cpu, &callback_count);
	if (ret < 0) {
		atomic_set(&callback_queued, 0);
		return ret;
	}
	if (ret != callback_count)
		return -EIO;
	return count;
}

static int status_show(struct seq_file *seq, void *unused)
{
	cpumask_t cpus;
	unsigned long flags;

	(void)unused;
	spin_lock_irqsave(&callback_cpu_lock, flags);
	cpumask_copy(&cpus, &callback_cpus);
	spin_unlock_irqrestore(&callback_cpu_lock, flags);
	seq_printf(seq,
		   "target_cpu=%u reader_cpu=%u reader_active=%u queued=%d completed=%d target_callbacks=%d callback_first_ns=%lld callback_last_ns=%lld target_first_ns=%lld target_last_ns=%lld callback_cpus=%*pbl\n",
		   target_cpu, reader_cpu, READ_ONCE(reader_active),
		   atomic_read(&callback_queued),
		   atomic_read(&callback_completed),
		   atomic_read(&target_callback_count),
		   (long long)atomic64_read(&callback_first_ns),
		   (long long)atomic64_read(&callback_last_ns),
		   (long long)atomic64_read(&target_callback_first_ns),
		   (long long)atomic64_read(&target_callback_last_ns),
		   cpumask_pr_args(&cpus));
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, inode->i_private);
}

static const struct file_operations hold_fops = {
	.owner = THIS_MODULE,
	.write = hold_write,
	.llseek = noop_llseek,
};

static const struct file_operations release_fops = {
	.owner = THIS_MODULE,
	.write = release_write,
	.llseek = noop_llseek,
};

static const struct file_operations queue_fops = {
	.owner = THIS_MODULE,
	.write = queue_write,
	.llseek = noop_llseek,
};

static const struct file_operations status_fops = {
	.owner = THIS_MODULE,
	.open = status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init cpu_accel_rcu_callback_probe_init(void)
{
	struct dentry *entry;
	int ret;

	if (target_cpu >= nr_cpu_ids || reader_cpu >= nr_cpu_ids ||
	    target_cpu == reader_cpu || !cpu_online(target_cpu) ||
	    !cpu_online(reader_cpu))
		return -EINVAL;

	reader_task = kthread_create_on_cpu(cpu_accel_rcu_reader, NULL,
					   reader_cpu,
					   "cpu_accel_rcu_reader/%u");
	if (IS_ERR(reader_task))
		return PTR_ERR(reader_task);
	wake_up_process(reader_task);

	probe_dir = debugfs_create_dir("cpu_accel_rcu_callback_probe", NULL);
	if (IS_ERR_OR_NULL(probe_dir)) {
		ret = probe_dir ? PTR_ERR(probe_dir) : -ENODEV;
		goto err_stop_reader;
	}
	entry = debugfs_create_file("hold", 0200, probe_dir, NULL, &hold_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_remove_debugfs;
	}
	entry = debugfs_create_file("release", 0200, probe_dir, NULL,
				    &release_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_remove_debugfs;
	}
	entry = debugfs_create_file("queue", 0200, probe_dir, NULL, &queue_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_remove_debugfs;
	}
	entry = debugfs_create_file("status", 0400, probe_dir, NULL,
				    &status_fops);
	if (IS_ERR_OR_NULL(entry)) {
		ret = entry ? PTR_ERR(entry) : -ENOMEM;
		goto err_remove_debugfs;
	}
	return 0;

err_remove_debugfs:
	debugfs_remove_recursive(probe_dir);
err_stop_reader:
	kthread_stop(reader_task);
	return ret;
}

static void __exit cpu_accel_rcu_callback_probe_exit(void)
{
	debugfs_remove_recursive(probe_dir);
	WRITE_ONCE(reader_hold, false);
	wake_up_all(&reader_wait);
	kthread_stop(reader_task);
	rcu_barrier();
}

module_init(cpu_accel_rcu_callback_probe_init);
module_exit(cpu_accel_rcu_callback_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe RCU callback routing during CPU accelerator ownership");
