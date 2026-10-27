// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

static unsigned int target_cpu = 3;
module_param(target_cpu, uint, 0444);
MODULE_PARM_DESC(target_cpu, "CPU on which to queue the per-CPU work");

static struct dentry *debugfs_dir;
static struct work_struct sync_work;
static struct workqueue_struct *sync_wq;
static atomic_t trigger_busy = ATOMIC_INIT(0);

static void cpu_accel_sync_work_flush_probe_fn(struct work_struct *work)
{
	(void)work;
}

static ssize_t run_probe(const char __user *user_buf, size_t count,
			 loff_t *ppos, bool flush_queue)
{
	char input[16];
	bool run;
	int ret;

	(void)ppos;
	if (!count || count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, user_buf, count))
		return -EFAULT;
	input[count] = '\0';
	ret = kstrtobool(input, &run);
	if (ret)
		return ret;
	if (!run)
		return count;
	if (target_cpu >= nr_cpu_ids || !cpu_online(target_cpu))
		return -ENXIO;
	if (atomic_cmpxchg(&trigger_busy, 0, 1))
		return -EBUSY;

	if (!queue_work_on(target_cpu,
			   flush_queue ? sync_wq : system_percpu_wq,
			   &sync_work)) {
		ret = -EBUSY;
		goto out;
	}
	if (flush_queue)
		flush_workqueue_with_sync_wait(sync_wq);
	else
		flush_work_with_sync_wait(&sync_work);
	ret = count;
out:
	atomic_set(&trigger_busy, 0);
	return ret;
}

static ssize_t run_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	(void)file;
	return run_probe(user_buf, count, ppos, false);
}

static ssize_t run_workqueue_write(struct file *file,
				   const char __user *user_buf,
				   size_t count, loff_t *ppos)
{
	(void)file;
	return run_probe(user_buf, count, ppos, true);
}

static const struct file_operations run_fops = {
	.owner = THIS_MODULE,
	.write = run_write,
	.llseek = noop_llseek,
};

static const struct file_operations run_workqueue_fops = {
	.owner = THIS_MODULE,
	.write = run_workqueue_write,
	.llseek = noop_llseek,
};

static int __init cpu_accel_sync_work_flush_probe_init(void)
{
	struct dentry *run_file;
	struct dentry *run_workqueue_file;
	int ret;

	INIT_WORK(&sync_work, cpu_accel_sync_work_flush_probe_fn);
	sync_wq = alloc_workqueue("cpu_accel_sync_work", WQ_PERCPU, 1);
	if (!sync_wq)
		return -ENOMEM;
	debugfs_dir = debugfs_create_dir("cpu_accel_sync_work_flush", NULL);
	if (IS_ERR_OR_NULL(debugfs_dir)) {
		ret = debugfs_dir ? PTR_ERR(debugfs_dir) : -ENODEV;
		goto err_destroy_wq;
	}
	run_file = debugfs_create_file("run", 0200, debugfs_dir, NULL,
				       &run_fops);
	if (IS_ERR_OR_NULL(run_file)) {
		ret = run_file ? PTR_ERR(run_file) : -ENOMEM;
		goto err_remove_debugfs;
	}
	run_workqueue_file = debugfs_create_file("run_workqueue", 0200,
						 debugfs_dir, NULL,
						 &run_workqueue_fops);
	if (IS_ERR_OR_NULL(run_workqueue_file)) {
		ret = run_workqueue_file ? PTR_ERR(run_workqueue_file) : -ENOMEM;
		goto err_remove_debugfs;
	}
	return 0;

err_remove_debugfs:
	debugfs_remove_recursive(debugfs_dir);
err_destroy_wq:
	destroy_workqueue(sync_wq);
	return ret;
}

static void __exit cpu_accel_sync_work_flush_probe_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);
	cancel_work_sync_with_sync_wait(&sync_work);
	destroy_workqueue_with_sync_wait(sync_wq);
}

module_init(cpu_accel_sync_work_flush_probe_init);
module_exit(cpu_accel_sync_work_flush_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test sync-wait per-CPU work flushes");
