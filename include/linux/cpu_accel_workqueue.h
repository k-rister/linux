/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CPU_ACCEL_WORKQUEUE_H
#define _LINUX_CPU_ACCEL_WORKQUEUE_H

#include <linux/types.h>

struct workqueue_struct;

#ifdef CONFIG_CPU_ACCEL_CORE
void cpu_accel_flush_workqueue(struct workqueue_struct *wq);
void cpu_accel_drain_workqueue(struct workqueue_struct *wq);
void cpu_accel_destroy_workqueue(struct workqueue_struct *wq);

int workqueue_accel_cpu_reserve(unsigned int cpu);
void workqueue_accel_cpu_release(unsigned int cpu);
bool workqueue_accel_cpu_reserved(unsigned int cpu);
#else
void __flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);
void destroy_workqueue(struct workqueue_struct *wq);

static inline void cpu_accel_flush_workqueue(struct workqueue_struct *wq)
{
	__flush_workqueue(wq);
}

static inline void cpu_accel_drain_workqueue(struct workqueue_struct *wq)
{
	drain_workqueue(wq);
}

static inline void cpu_accel_destroy_workqueue(struct workqueue_struct *wq)
{
	destroy_workqueue(wq);
}

static inline int workqueue_accel_cpu_reserve(unsigned int cpu)
{
	return 0;
}

static inline void workqueue_accel_cpu_release(unsigned int cpu)
{
}

static inline bool workqueue_accel_cpu_reserved(unsigned int cpu)
{
	return false;
}
#endif

#endif /* _LINUX_CPU_ACCEL_WORKQUEUE_H */
