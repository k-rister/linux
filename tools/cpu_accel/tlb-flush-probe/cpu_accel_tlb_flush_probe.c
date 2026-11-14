// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/module.h>

static int __init cpu_accel_tlb_flush_probe_init(void)
{
	pr_info("cpu_accel_tlb_flush_probe: loaded\n");
	return 0;
}

static void __exit cpu_accel_tlb_flush_probe_exit(void)
{
	pr_info("cpu_accel_tlb_flush_probe: unloaded\n");
}

module_init(cpu_accel_tlb_flush_probe_init);
module_exit(cpu_accel_tlb_flush_probe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test-only module-load TLB flush trigger");
