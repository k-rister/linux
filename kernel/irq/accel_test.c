// SPDX-License-Identifier: LGPL-2.1+

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irq_accel.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/nodemask.h>
#include <kunit/test.h>

#include "internals.h"

static irqreturn_t noop_handler(int irq, void *data)
{
	return IRQ_HANDLED;
}

static void noop(struct irq_data *data) { }
static unsigned int noop_ret(struct irq_data *data) { return 0; }

static int noop_affinity(struct irq_data *data, const struct cpumask *dest,
			 bool force)
{
	irq_data_update_effective_affinity(data, dest);

	return 0;
}

static struct irq_chip fake_irq_chip = {
	.name           = "fake",
	.irq_startup    = noop_ret,
	.irq_shutdown   = noop,
	.irq_enable     = noop,
	.irq_disable    = noop,
	.irq_ack        = noop,
	.irq_mask       = noop,
	.irq_unmask     = noop,
	.irq_set_affinity = noop_affinity,
	.flags          = IRQCHIP_SKIP_SET_WAKE,
};

static int irq_accel_test_setup_fake_irq(struct kunit *test,
					 struct irq_affinity_desc *affd)
{
	struct irq_desc *desc;
	int virq;

	virq = irq_domain_alloc_descs(-1, 1, 0, NUMA_NO_NODE, affd);
	KUNIT_ASSERT_GE(test, virq, 0);

	irq_set_chip_and_handler(virq, &fake_irq_chip, handle_simple_irq);

	desc = irq_to_desc(virq);
	KUNIT_ASSERT_PTR_NE(test, desc, NULL);

	irq_settings_clr_norequest(desc);

	return virq;
}

static void irq_accel_internal_affinity_test(struct kunit *test)
{
	struct irq_desc *desc;
	struct irq_data *data;
	struct cpumask affinity;
	unsigned int cpu;
	unsigned long flags;
	bool reserved = false;
	bool requested = false;
	bool targets_reserved;
	int virq, ret;

	if (!IS_ENABLED(CONFIG_SMP) ||
	    !IS_ENABLED(CONFIG_GENERIC_IRQ_MIGRATION))
		kunit_skip(test, "requires SMP and generic IRQ migration");

	cpus_read_lock();
	if (cpumask_weight(cpu_online_mask) < 2) {
		cpus_read_unlock();
		kunit_skip(test, "requires at least two online CPUs");
	}

	cpu = cpumask_first(cpu_online_mask);
	virq = irq_accel_test_setup_fake_irq(test, NULL);
	desc = irq_to_desc(virq);
	data = irq_desc_get_irq_data(desc);
	cpumask_copy(&affinity, cpu_online_mask);

	ret = irq_accel_reserve_cpu(cpu);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_unlock;
	reserved = true;

	/* Simulate an IRQ configured for this CPU before accelerator admission. */
	cpumask_copy(desc->irq_common_data.affinity, cpumask_of(cpu));
	irqd_mark_affinity_was_set(data);

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_cleanup;
	requested = true;
	targets_reserved = cpumask_test_cpu(cpu, irq_data_get_effective_affinity_mask(data));
	KUNIT_EXPECT_FALSE(test, targets_reserved);

	/* Exercise an internal caller that bypasses irq_set_affinity_locked(). */
	raw_spin_lock_irqsave(&desc->lock, flags);
	ret = irq_do_set_affinity(data, &affinity, false);
	targets_reserved = cpumask_test_cpu(cpu, irq_data_get_effective_affinity_mask(data));
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, targets_reserved);

	/* Public affinity requests must remain rejected for the owned CPU. */
	ret = irq_set_affinity(virq, cpumask_of(cpu));
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);
	ret = irq_force_affinity(virq, cpumask_of(cpu));
	KUNIT_EXPECT_EQ(test, ret, -EBUSY);

out_cleanup:
	if (reserved)
		irq_accel_release_cpu(cpu);
	if (requested)
		free_irq(virq, NULL);
out_unlock:
	cpus_read_unlock();
}

static void irq_accel_managed_startup_test(struct kunit *test)
{
	struct irq_desc *desc;
	struct irq_data *data;
	struct irq_affinity_desc affinity = {
		.is_managed = 1,
	};
	unsigned int cpu;
	bool reserved = false;
	bool requested = false;
	int virq, ret;

	if (!IS_ENABLED(CONFIG_SMP) ||
	    !IS_ENABLED(CONFIG_GENERIC_IRQ_MIGRATION))
		kunit_skip(test, "requires SMP and generic IRQ migration");

	cpus_read_lock();
	if (cpumask_weight(cpu_online_mask) < 2) {
		cpus_read_unlock();
		kunit_skip(test, "requires at least two online CPUs");
	}

	cpu = cpumask_first(cpu_online_mask);
	cpumask_copy(&affinity.mask, cpumask_of(cpu));
	virq = irq_accel_test_setup_fake_irq(test, &affinity);
	desc = irq_to_desc(virq);
	data = irq_desc_get_irq_data(desc);

	ret = irq_accel_reserve_cpu(cpu);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_unlock;
	reserved = true;

	ret = request_irq(virq, noop_handler, 0, "test_irq", NULL);
	KUNIT_EXPECT_EQ(test, ret, 0);
	if (ret)
		goto out_cleanup;
	requested = true;

	KUNIT_EXPECT_TRUE(test, irqd_affinity_is_managed(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_managed_and_shutdown(data));
	KUNIT_EXPECT_FALSE(test, irqd_is_activated(data));
	KUNIT_EXPECT_FALSE(test, irqd_is_started(data));

	/* Releasing ownership must restart the IRQ on its now-available CPU. */
	irq_accel_release_cpu(cpu);
	reserved = false;
	KUNIT_EXPECT_TRUE(test, irqd_is_activated(data));
	KUNIT_EXPECT_TRUE(test, irqd_is_started(data));

out_cleanup:
	if (reserved)
		irq_accel_release_cpu(cpu);
	if (requested)
		free_irq(virq, NULL);
out_unlock:
	cpus_read_unlock();
}

static struct kunit_case irq_accel_test_cases[] = {
	KUNIT_CASE(irq_accel_internal_affinity_test),
	KUNIT_CASE(irq_accel_managed_startup_test),
	{}
};

static struct kunit_suite irq_accel_test_suite = {
	.name = "irq_accel_test_cases",
	.test_cases = irq_accel_test_cases,
};

kunit_test_suite(irq_accel_test_suite);
MODULE_DESCRIPTION("CPU accelerator IRQ unit test suite");
MODULE_LICENSE("GPL");
