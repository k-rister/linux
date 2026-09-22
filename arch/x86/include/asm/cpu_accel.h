/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_CPU_ACCEL_H
#define _ASM_X86_CPU_ACCEL_H

typedef void (*x86_cpu_accel_entry_fn)(void *data);

int x86_cpu_accel_direct_enter(unsigned int cpu,
			       x86_cpu_accel_entry_fn entry, void *data);
bool x86_cpu_accel_defer_reschedule(unsigned int cpu);
u64 x86_cpu_accel_reschedule_deferred(unsigned int cpu);
bool x86_cpu_accel_defer_call_function(unsigned int cpu);
u64 x86_cpu_accel_call_function_deferred(unsigned int cpu);

#endif
