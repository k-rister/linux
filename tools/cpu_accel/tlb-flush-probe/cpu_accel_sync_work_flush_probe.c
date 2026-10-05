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
static atomic_t trigger_busy = ATOMIC_INIT(0);

static void cpu_accel_sync_work_flush_probe_fn(struct work_struct *work)
{
	(void)work;
}

static ssize_t run_write(struct file *file, const char __user *user_buf,
			 size_t count, loff_t *ppos)
{
	char input[16];
	bool run;
	int ret;

	(void)file;
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

	if (!queue_work_on(target_cpu, system_percpu_wq, &sync_work)) {
		ret = -EBUSY;
		goto out;
	}
	flush_work_with_sync_wait(&sync_work);
	ret = count;
out:
	atomic_set(&trigger_busy, 0);
	return ret;
}

static const struct file_operations run_fops = {
	.owner = THIS_MODULE,
	.write = run_write,
	.llseek = noop_llseek,
};

static int __init cpu_accel_sync_work_flush_probe_init(void)
{
	struct dentry *run_file;
	int ret;

	INIT_WORK(&sync_work, cpu_accel_sync_work_flush_probe_fn);
	debugfs_dir = debugfs_create_dir("cpu_accel_sync_work_flush", NULL);
	if (IS_ERR_OR_NULL(debugfs_dir))
		return debugfs_dir ? PTR_ERR(debugfs_dir) : -ENODEV;
	run_file = debugfs_create_file("run", 0200, debugfs_dir, NULL,
				       &run_fops);
	if (IS_ERR_OR_NULL(run_file)) {
		ret = run_file ? PTR_ERR(run_file) : -ENOMEM;
		debugfs_remove_recursive(debugfs_dir);
		return ret;
	}
	return 0;
}

static void __exit cpu_accel_sync_work_flush_probe_exit(void)
{
	debugfs_remove_recursive(debugfs_dir);
	cancel_work_sync(&sync_work);
}

module_init(cpu_accel_sync_work_flush_probe_init);
module_exit(cpu_accel_sync_work_flush_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test sync-wait per-CPU work flush");
