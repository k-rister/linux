// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/srcu.h>
#include <linux/srcutree.h>
#include <linux/workqueue.h>

static int target_cpu = 2;
module_param(target_cpu, int, 0444);

static struct srcu_struct probe_srcu;

static int __init cpu_accel_srcu_cleanup_probe_init(void)
{
	int ret;

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids || !cpu_online(target_cpu))
		return -EINVAL;

	ret = init_srcu_struct(&probe_srcu);
	if (ret)
		return ret;

	/* Establish the normal no-outstanding-callback cleanup precondition. */
	srcu_barrier(&probe_srcu);
	pr_info("cpu_accel_srcu_cleanup_probe: initialized for cpu=%d\n",
		target_cpu);
	return 0;
}

static void __exit cpu_accel_srcu_cleanup_probe_exit(void)
{
	struct srcu_data *sdp = per_cpu_ptr(probe_srcu.sda, target_cpu);

	/*
	 * Queue the initialized SRCU callback worker with no callbacks pending.
	 * cleanup_srcu_struct() must stop an active owner before flushing it.
	 */
	if (!schedule_work_on(target_cpu, &sdp->work))
		pr_err("cpu_accel_srcu_cleanup_probe: callback work was already queued\n");
	else
		pr_info("cpu_accel_srcu_cleanup_probe: queued callback work on cpu=%d\n",
			target_cpu);

	cleanup_srcu_struct(&probe_srcu);
	pr_info("cpu_accel_srcu_cleanup_probe: cleanup completed\n");
}

module_init(cpu_accel_srcu_cleanup_probe_init);
module_exit(cpu_accel_srcu_cleanup_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Probe active-owner stop during SRCU cleanup work flush");
