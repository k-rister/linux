/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM cpu_accel

#if !defined(_TRACE_CPU_ACCEL_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_CPU_ACCEL_H

#include <linux/tracepoint.h>

TRACE_EVENT(owner_stop_request,

	TP_PROTO(unsigned int cpu, u64 owner_id, bool user_mm,
		 bool callback_registered, bool callback_sent,
		 unsigned long caller),

	TP_ARGS(cpu, owner_id, user_mm, callback_registered, callback_sent,
		caller),

	TP_STRUCT__entry(
		__field(unsigned int, cpu)
		__field(u64, owner_id)
		__field(bool, user_mm)
		__field(bool, callback_registered)
		__field(bool, callback_sent)
		__field(unsigned long, caller)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->owner_id = owner_id;
		__entry->user_mm = user_mm;
		__entry->callback_registered = callback_registered;
		__entry->callback_sent = callback_sent;
		__entry->caller = caller;
	),

	TP_printk("cpu=%u owner=%llu user_mm=%u stop_cb=%u sent=%u caller=%pS",
		  __entry->cpu, (unsigned long long)__entry->owner_id,
		  __entry->user_mm, __entry->callback_registered,
		  __entry->callback_sent, (void *)__entry->caller)
);

TRACE_EVENT(owner_exit_complete,

	TP_PROTO(unsigned int cpu, u64 owner_id, bool user_mm,
		 bool stop_requested, u64 tlb_gen, bool unscoped_flush),

	TP_ARGS(cpu, owner_id, user_mm, stop_requested, tlb_gen,
		unscoped_flush),

	TP_STRUCT__entry(
		__field(unsigned int, cpu)
		__field(u64, owner_id)
		__field(bool, user_mm)
		__field(bool, stop_requested)
		__field(u64, tlb_gen)
		__field(bool, unscoped_flush)
	),

	TP_fast_assign(
		__entry->cpu = cpu;
		__entry->owner_id = owner_id;
		__entry->user_mm = user_mm;
		__entry->stop_requested = stop_requested;
		__entry->tlb_gen = tlb_gen;
		__entry->unscoped_flush = unscoped_flush;
	),

	TP_printk("cpu=%u owner=%llu user_mm=%u stop_requested=%u tlb_gen=%llu unscoped=%u",
		  __entry->cpu, (unsigned long long)__entry->owner_id,
		  __entry->user_mm, __entry->stop_requested,
		  (unsigned long long)__entry->tlb_gen,
		  __entry->unscoped_flush)
);

TRACE_EVENT(tlb_flush_wait,

	TP_PROTO(u64 flush_id, bool complete, unsigned int targets,
		 unsigned int owner_targets, unsigned long caller),

	TP_ARGS(flush_id, complete, targets, owner_targets, caller),

	TP_STRUCT__entry(
		__field(u64, flush_id)
		__field(bool, complete)
		__field(unsigned int, targets)
		__field(unsigned int, owner_targets)
		__field(unsigned long, caller)
	),

	TP_fast_assign(
		__entry->flush_id = flush_id;
		__entry->complete = complete;
		__entry->targets = targets;
		__entry->owner_targets = owner_targets;
		__entry->caller = caller;
	),

	TP_printk("id=%llu phase=%s targets=%u owners=%u caller=%pS",
		  (unsigned long long)__entry->flush_id,
		  __entry->complete ? "complete" : "begin",
		  __entry->targets, __entry->owner_targets,
		  (void *)__entry->caller)
);

#endif /* _TRACE_CPU_ACCEL_H */

#include <trace/define_trace.h>
