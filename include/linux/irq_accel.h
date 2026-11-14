/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQ_ACCEL_H
#define _LINUX_IRQ_ACCEL_H

#include <linux/types.h>

struct cpumask;
struct irq_accel_quarantine;

#ifdef CONFIG_CPU_ACCEL_CORE
bool irq_accel_cpu_reserved(unsigned int cpu);
bool irq_accel_affinity_allowed(const struct cpumask *mask);
int irq_accel_reserve_cpu(unsigned int cpu);
void irq_accel_release_cpu(unsigned int cpu);
int irq_accel_quarantine_cpu(unsigned int cpu,
			     struct irq_accel_quarantine **quarantine,
			     unsigned int *migrated,
			     unsigned int *blocked);
int irq_accel_restore_cpu(struct irq_accel_quarantine *quarantine);
#else
static inline bool irq_accel_cpu_reserved(unsigned int cpu)
{
	return false;
}
static inline bool irq_accel_affinity_allowed(const struct cpumask *mask)
{
	return true;
}
static inline int irq_accel_reserve_cpu(unsigned int cpu)
{
	return -EOPNOTSUPP;
}
static inline void irq_accel_release_cpu(unsigned int cpu)
{
}
static inline int irq_accel_quarantine_cpu(unsigned int cpu,
					   struct irq_accel_quarantine **quarantine,
					   unsigned int *migrated,
					   unsigned int *blocked)
{
	return -EOPNOTSUPP;
}
static inline int irq_accel_restore_cpu(struct irq_accel_quarantine *quarantine)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_IRQ_ACCEL_H */
