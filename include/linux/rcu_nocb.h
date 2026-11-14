/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RCU_NOCB_H
#define _LINUX_RCU_NOCB_H

#include <linux/types.h>

#ifdef CONFIG_RCU_NOCB_CPU
/* Caller must disable preemption to keep the local rcu_data stable. */
bool rcu_current_cpu_is_offloaded(void);
#else
static inline bool rcu_current_cpu_is_offloaded(void)
{
	return false;
}
#endif

#endif /* _LINUX_RCU_NOCB_H */
