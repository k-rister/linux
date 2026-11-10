// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"
#include "cpu_accel_workload.h"
#include "cpu_accel_user_image.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__x86_64__)
/* Optional glibc exports describing this thread's registered rseq area. */
extern const ptrdiff_t __rseq_offset __attribute__((weak));
extern const unsigned int __rseq_size __attribute__((weak));
#endif

/* Keep the bounded ring-3 code in one page-aligned linker section. */
#define CPU_ACCEL_USER_IMAGE \
	__attribute__((section("cpu_accel_user_image"), aligned(4096)))
#define CPU_ACCEL_VSYSCALL_ADDR	(-10UL << 20)
#define CPU_ACCEL_MAX_USER_RECLAIM_BYTES	(64UL * 1024UL * 1024UL)
#define CPU_ACCEL_USER_PTE_TABLE_PAGES	512UL
/* A1: fixed-point scale for cycles-per-ns (milli-cycles) so a non-integer-GHz
 * TSC is not truncated to a whole integer by the calibration. */
#define CPU_ACCEL_CP_SCALE	1000UL

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

struct cpu_accel_user_unmap_range;

struct cpu_accel_user_context {
	unsigned long stack_top;
	unsigned long reclaim_page;
	unsigned long reclaim_bytes;
	struct cpu_accel_shared *shared;
	struct cpu_accel_workload_region *region;
	int fd;
	uint64_t cycles_per_ns;
	struct cpu_accel_user_unmap_range *unmap_ranges;
	unsigned long nr_unmap_ranges;
};

struct cpu_accel_user_unmap_range {
	unsigned long start;
	unsigned long length;
};

#define CPU_ACCEL_MAX_USER_UNMAP_RANGES	256

static int cpu_accel_populate_file_mapping(void *mapping, size_t bytes,
					   size_t page_size,
					   bool preserve_pte_tables)
{
	for (size_t offset = 0; offset < bytes; offset += page_size) {
		const char *address = (const char *)mapping + offset;

		__asm__ volatile("movb (%0), %%al" : : "r" (address) : "rax", "memory");
	}
	if (preserve_pte_tables) {
		size_t table_bytes = page_size * CPU_ACCEL_USER_PTE_TABLE_PAGES;

		for (size_t offset = table_bytes - page_size;
		     offset < bytes; offset += table_bytes) {
			const char *address = (const char *)mapping + offset;

			/* Keep one anonymous COW PTE in each table through hole punch. */
			__asm__ volatile("movb $0x5a, (%0)" : : "r" (address) : "memory");
		}
		if (mprotect(mapping, bytes, PROT_READ))
			return -errno;
	}
	return 0;
}

struct cpu_accel_user_keep_range {
	unsigned long start;
	unsigned long end;
};

static inline uint64_t cpu_accel_read_tsc(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

/* Ordered (lfence;rdtsc) read so the oslat inter-iteration delta is not
 * skewed by a speculated-early counter.
 */
static inline uint64_t cpu_accel_read_tsc_ordered(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("lfence\n\t"
			 "rdtsc"
			 : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

static __attribute__((always_inline)) inline long
cpu_accel_user_syscall1(long number, unsigned long arg1)
{
	long ret;

	__asm__ volatile("syscall"
		     : "=a"(ret)
		     : "a"(number), "D"(arg1)
		     : "rcx", "r11", "memory");
	return ret;
}

static __attribute__((always_inline)) inline long
cpu_accel_user_syscall2(long number, unsigned long arg1, unsigned long arg2)
{
	long ret;

	__asm__ volatile("syscall"
		     : "=a"(ret)
		     : "a"(number), "D"(arg1), "S"(arg2)
		     : "rcx", "r11", "memory");
	return ret;
}

static __attribute__((always_inline)) inline long
cpu_accel_user_syscall3(long number, unsigned long arg1, unsigned long arg2,
			unsigned long arg3)
{
	long ret;

	__asm__ volatile("syscall"
		     : "=a"(ret)
		     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
		     : "rcx", "r11", "memory");
	return ret;
}

/* Switch to the admitted stack before removing the worker's normal mappings. */
CPU_ACCEL_USER_IMAGE
__attribute__((naked, noreturn))
static void cpu_accel_user_launch(struct cpu_accel_user_context *)
{
	__asm__ volatile("movq (%rdi), %rsp\n"
			 "andq $-16, %rsp\n"
			 "subq $8, %rsp\n"
			 "jmp cpu_accel_user_seal_and_start\n");
}

CPU_ACCEL_USER_IMAGE
__attribute__((noinline, noreturn, used))
static void cpu_accel_user_seal_and_start(
	struct cpu_accel_user_context *context)
{
	long ret;

	for (unsigned long i = 0; i < context->nr_unmap_ranges; i++) {
		const struct cpu_accel_user_unmap_range *range =
			&context->unmap_ranges[i];

		ret = cpu_accel_user_syscall2(SYS_munmap, range->start,
					     range->length);
		if (ret) {
			cpu_accel_user_syscall1(SYS_exit_group, 125);
			__builtin_unreachable();
		}
	}

	ret = cpu_accel_user_syscall3(SYS_ioctl, context->fd,
				      CPU_ACCEL_IOC_START, 0);
	cpu_accel_user_syscall1(SYS_exit_group, ret < 0 ? 126 : 0);
	__builtin_unreachable();
}

CPU_ACCEL_USER_IMAGE
static long cpu_accel_user_exit_syscall(int fd)
{
	long ret;

	__asm__ volatile("syscall"
		     : "=a"(ret)
		     : "a"(SYS_ioctl), "D"(fd),
		       "S"((unsigned long)CPU_ACCEL_IOC_USER_EXIT), "d"(0UL)
		     : "rcx", "r11", "memory");
	return ret;
}

CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_escape_image(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	long ret;

	__asm__ volatile("syscall"
		     : "=a"(ret)
		     : "a"(SYS_ioctl), "D"(context->fd),
		       "S"((unsigned long)CPU_ACCEL_IOC_USER_EXIT), "d"(0UL)
		     : "rcx", "r11", "memory");
	(void)ret;
	__builtin_unreachable();
}

CPU_ACCEL_USER_IMAGE
static unsigned int cpu_accel_user_lateness_bin(uint64_t lateness_ns)
{
	if (!lateness_ns)
		return 0;
	return lateness_ns > (1ULL << (CPU_ACCEL_WORKLOAD_LATENESS_BINS - 1)) ?
		CPU_ACCEL_WORKLOAD_LATENESS_BINS - 1 :
		64U - __builtin_clzll(lateness_ns);
}

CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_cyclictest(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	struct cpu_accel_workload_region *region = context->region;
	uint64_t cpm = context->cycles_per_ns;
	uint64_t start_tsc = cpu_accel_read_tsc();
	uint64_t period_ns = __atomic_load_n(&context->shared->period_ns,
					     __ATOMIC_RELAXED);
	uint64_t period_cycles;
	uint64_t deadline;
	uint64_t samples = 0;
	uint64_t max_lateness = 0;
	uint64_t min_lateness = UINT64_MAX;
	uint64_t last_lateness = 0;
	uint64_t sum_lateness = 0;
	uint32_t samples_valid = 0;

	if (!region) {
		cpu_accel_user_syscall1(SYS_exit_group, 124);
		__builtin_unreachable();
	}
	if (!cpm)
		cpm = CPU_ACCEL_CP_SCALE;
	period_cycles = (period_ns * cpm) / CPU_ACCEL_CP_SCALE;
	if (!period_cycles)
		period_cycles = 1;
	deadline = start_tsc + period_cycles;
	if (context->reclaim_page)
		(void)*(volatile unsigned char *)context->reclaim_page;

	for (;;) {
		uint64_t now = cpu_accel_read_tsc();
		uint64_t lateness_cycles;
		uint64_t lateness_ns;
		uint64_t timestamp_ns;

		if (now < deadline) {
			__asm__ volatile("pause" ::: "memory");
			continue;
		}
		lateness_cycles = now - deadline;
		lateness_ns = (lateness_cycles * CPU_ACCEL_CP_SCALE) / cpm;
		timestamp_ns = __atomic_load_n(&context->shared->start_ns,
					       __ATOMIC_RELAXED) +
			((now - start_tsc) * CPU_ACCEL_CP_SCALE) / cpm;
		if (samples_valid < CPU_ACCEL_WORKLOAD_MAX_SAMPLES) {
			((struct cpu_accel_workload_sample *)
			 (region->payload + region->samples_offset))
				[samples_valid].timestamp_ns = timestamp_ns;
			((struct cpu_accel_workload_sample *)
			 (region->payload + region->samples_offset))
				[samples_valid].lateness_ns =
				(int64_t)lateness_ns;
			samples_valid++;
		}
		samples++;
		if (lateness_ns > max_lateness)
			max_lateness = lateness_ns;
		if (lateness_ns < min_lateness)
			min_lateness = lateness_ns;
		last_lateness = lateness_ns;
		((uint64_t *)(region->payload + region->lateness_hist_offset))
			[cpu_accel_user_lateness_bin(lateness_ns)]++;
		if (lateness_ns < CPU_ACCEL_WORKLOAD_LINEAR_BINS)
			((uint64_t *)(region->payload +
				      region->linear_hist_offset))
				[lateness_ns]++;
		else
			((uint64_t *)(region->payload +
				      region->linear_hist_offset))
				[CPU_ACCEL_WORKLOAD_LINEAR_BINS]++;
		sum_lateness += lateness_ns;
		if (__atomic_load_n(&context->shared->stop_requested,
				    __ATOMIC_RELAXED))
			break;
		if (deadline > UINT64_MAX - period_cycles)
			break;
		deadline += period_cycles;
	}

	__atomic_store_n(&region->samples_produced, samples,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->samples_valid, samples_valid,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->max_lateness_ns, max_lateness,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->min_lateness_ns,
			 samples ? min_lateness : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&region->last_lateness_ns, last_lateness,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->sum_lateness_ns, sum_lateness,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->work_iterations, samples,
			 __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&region->completion_sequence, 1, __ATOMIC_RELEASE);
	(void)cpu_accel_user_exit_syscall(context->fd);
	__builtin_unreachable();
}

/*
 * Tight-loop (oslat) inter-iteration latency probe. Unlike the cyclictest
 * image's periodic deadline, this runs back-to-back and records the time
 * between consecutive ordered TSC reads. On a clean owned CPU each delta is
 * small and stable; any IPI, IRQ or forced reschedule widens one delta, so
 * the histogram spikes reveal OS latency on the owned CPU.
 */
CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_oslat(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	struct cpu_accel_workload_region *region = context->region;
	uint64_t cpm = context->cycles_per_ns;
	uint64_t start_tsc = cpu_accel_read_tsc_ordered();
	uint64_t prev = start_tsc;
	uint64_t samples = 0;
	uint64_t max_delta = 0;
	uint64_t min_delta = UINT64_MAX;
	uint64_t last_delta = 0;
	uint64_t sum_delta = 0;
	uint32_t samples_valid = 0;

	if (!region) {
		cpu_accel_user_syscall1(SYS_exit_group, 124);
		__builtin_unreachable();
	}
	if (!cpm)
		cpm = CPU_ACCEL_CP_SCALE;

	for (;;) {
		uint64_t now = cpu_accel_read_tsc_ordered();
		uint64_t delta_ns = ((now - prev) * CPU_ACCEL_CP_SCALE) / cpm;

		prev = now;
		if (samples_valid < CPU_ACCEL_WORKLOAD_MAX_SAMPLES) {
			struct cpu_accel_workload_sample *sample =
				(void *)(region->payload +
					 region->samples_offset);

			sample[samples_valid].timestamp_ns =
				__atomic_load_n(&context->shared->start_ns,
						__ATOMIC_RELAXED) +
				((now - start_tsc) * CPU_ACCEL_CP_SCALE) / cpm;
			sample[samples_valid].lateness_ns = delta_ns;
			samples_valid++;
		}
		samples++;
		if (delta_ns > max_delta)
			max_delta = delta_ns;
		if (delta_ns < min_delta)
			min_delta = delta_ns;
		last_delta = delta_ns;
		((uint64_t *)(region->payload + region->lateness_hist_offset))
			[cpu_accel_user_lateness_bin(delta_ns)]++;
		if (delta_ns < CPU_ACCEL_WORKLOAD_LINEAR_BINS)
			((uint64_t *)(region->payload +
				      region->linear_hist_offset))
				[delta_ns]++;
		else
			((uint64_t *)(region->payload +
				      region->linear_hist_offset))
				[CPU_ACCEL_WORKLOAD_LINEAR_BINS]++;
		sum_delta += delta_ns;
		if (__atomic_load_n(&context->shared->stop_requested,
				    __ATOMIC_RELAXED))
			break;
	}

	__atomic_store_n(&region->samples_produced, samples,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->samples_valid, samples_valid,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->max_lateness_ns, max_delta,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->min_lateness_ns,
			 samples ? min_delta : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&region->last_lateness_ns, last_delta,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->sum_lateness_ns, sum_delta,
			 __ATOMIC_RELAXED);
	__atomic_store_n(&region->work_iterations, samples,
			 __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&region->completion_sequence, 1, __ATOMIC_RELEASE);
	(void)cpu_accel_user_exit_syscall(context->fd);
	__builtin_unreachable();
}

CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_region_workload(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	struct cpu_accel_workload_region *region = context->region;
	uint64_t bytes;
	uint64_t entries;
	uint64_t block_bytes;
	uint64_t input_offset;
	uint64_t output_offset;
	uint64_t manager_ready;

	if (!region || region->region_role !=
	    CPU_ACCEL_WORKLOAD_ROLE_BUFFER) {
		cpu_accel_user_syscall1(SYS_exit_group, 124);
		__builtin_unreachable();
	}
	bytes = region->work_bytes;
	entries = __atomic_load_n(&region->ring_entries,
				  __ATOMIC_RELAXED);
	block_bytes = __atomic_load_n(&region->ring_block_bytes,
				      __ATOMIC_RELAXED);
	if (!bytes || !entries || !block_bytes ||
	    entries > CPU_ACCEL_WORKLOAD_RING_MAX_ENTRIES ||
	    block_bytes != (bytes + entries - 1) / entries) {
		cpu_accel_user_syscall1(SYS_exit_group, 124);
		__builtin_unreachable();
	}
	do {
		manager_ready = __atomic_load_n(&region->manager_ready,
						__ATOMIC_ACQUIRE);
		if (manager_ready != 1)
			__asm__ volatile("pause" ::: "memory");
	} while (manager_ready == 0);
	if (manager_ready != 1) {
		cpu_accel_user_syscall1(SYS_exit_group, 127);
		__builtin_unreachable();
	}
	input_offset = region->payload_offset;
	output_offset = region->payload_offset + bytes;
	for (uint64_t entry = 0; entry < entries; entry++) {
		uint64_t base = entry * block_bytes;
		uint64_t count = block_bytes;

		if (count > bytes - base)
			count = bytes - base;
		while (__atomic_load_n(&region->command_sequence,
				       __ATOMIC_ACQUIRE) <= entry)
			__asm__ volatile("pause" ::: "memory");
		for (uint64_t index = 0; index < count; index++)
			region->payload[output_offset + base + index] =
				(uint8_t)(region->payload[input_offset + base +
							  index] ^ 0x5a);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		__atomic_store_n(&region->result_sequence, entry + 1,
				 __ATOMIC_RELEASE);
	}
	__atomic_store_n(&region->work_iterations, 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&region->completion_sequence, 1, __ATOMIC_RELEASE);
	(void)cpu_accel_user_exit_syscall(context->fd);
	__builtin_unreachable();
}

/* Deliberately ignores the control mapping; recovery must be used to exit. */
CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_hang(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	uint64_t value = context->cycles_per_ns;

	for (;;) {
		value ^= cpu_accel_read_tsc();
		__asm__ volatile("pause" : "+r"(value) :: "memory");
	}
}

/*
 * Test-only owner-death workload.  Unlike cyclictest/hang this loop takes a
 * signal checkpoint (getpid) once per period, so a pending SIGKILL is
 * delivered while the user image is still active.  The owner then exits
 * through do_exit() without the USER_EXIT ioctl, exercising the
 * sched_process_exit death-detection path.  A clean duration/stop exit
 * still uses USER_EXIT.
 */
CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_death(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	struct cpu_accel_shared *shared = context->shared;
	uint64_t cycles_per_ns = context->cycles_per_ns;
	uint64_t start_tsc = cpu_accel_read_tsc();
	uint64_t period_ns = __atomic_load_n(&shared->period_ns,
					      __ATOMIC_RELAXED);
	uint64_t duration_ns = __atomic_load_n(&shared->duration_ns,
					       __ATOMIC_RELAXED);
	uint64_t period_cycles;
	uint64_t duration_cycles;
	uint64_t now;

	if (!cycles_per_ns)
		cycles_per_ns = 1;
	period_cycles = period_ns * cycles_per_ns;
	duration_cycles = duration_ns * cycles_per_ns;
	if (!period_cycles)
		period_cycles = 1;
	if (!duration_cycles)
		duration_cycles = period_cycles;

	for (;;) {
		(void)cpu_accel_user_syscall1(SYS_getpid, 0);
		now = cpu_accel_read_tsc();
		if (__atomic_load_n(&shared->stop_requested, __ATOMIC_RELAXED) ||
		    now - start_tsc >= duration_cycles)
			break;
		while (cpu_accel_read_tsc() < now + period_cycles)
			__asm__ volatile("pause" ::: "memory");
	}

	(void)cpu_accel_user_exit_syscall(context->fd);
	__builtin_unreachable();
}

static uint64_t cpu_accel_calibrate_cycles_per_ns(void)
{
	struct timespec start, end, delay = {
		.tv_sec = 0,
		.tv_nsec = 10000000,
	};
	uint64_t start_tsc;
	uint64_t end_tsc;
	uint64_t start_ns;
	uint64_t end_ns;
	uint64_t elapsed_ns;
	uint64_t cycles;

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
		return 1;
	start_tsc = cpu_accel_read_tsc();
	nanosleep(&delay, NULL);
	end_tsc = cpu_accel_read_tsc();
	if (clock_gettime(CLOCK_MONOTONIC, &end) < 0)
		return 1;
	start_ns = (uint64_t)start.tv_sec * 1000000000ULL + start.tv_nsec;
	end_ns = (uint64_t)end.tv_sec * 1000000000ULL + end.tv_nsec;
	elapsed_ns = end_ns - start_ns;
	cycles = end_tsc - start_tsc;
	if (!elapsed_ns || (cycles * CPU_ACCEL_CP_SCALE) / elapsed_ns == 0)
		return CPU_ACCEL_CP_SCALE;
	/* A1: milli-cycles per ns (1000x) so a non-integer-GHz TSC keeps its
	 * fractional part instead of being truncated to a whole integer. */
	return (cycles * CPU_ACCEL_CP_SCALE) / elapsed_ns;
}

static int cpu_accel_add_unmap_range(struct cpu_accel_user_context *context,
				     unsigned long start,
				     unsigned long end)
{
	struct cpu_accel_user_unmap_range *range;

	if (start >= end)
		return 0;
	if (context->nr_unmap_ranges >= CPU_ACCEL_MAX_USER_UNMAP_RANGES)
		return -E2BIG;

	range = &context->unmap_ranges[context->nr_unmap_ranges++];
	range->start = start;
	range->length = end - start;
	return 0;
}

static void cpu_accel_sort_keep_ranges(struct cpu_accel_user_keep_range *ranges,
				       unsigned int nr_ranges)
{
	for (unsigned int i = 1; i < nr_ranges; i++) {
		struct cpu_accel_user_keep_range range = ranges[i];
		unsigned int j = i;

		while (j && ranges[j - 1].start > range.start) {
			ranges[j] = ranges[j - 1];
			j--;
		}
		ranges[j] = range;
	}
}

static int cpu_accel_get_rseq_keep_range(unsigned long page_size,
					 unsigned long *start,
					 unsigned long *end)
{
	*start = 0;
	*end = 0;
#if defined(__x86_64__)
	uintptr_t thread_pointer;
	uintptr_t rseq_start;
	uintptr_t rseq_end;
	intptr_t signed_start;

	if (!&__rseq_offset || !&__rseq_size || !__rseq_size)
		return 0;
	if (!page_size || (page_size & (page_size - 1)) ||
	    __rseq_size > page_size)
		return -E2BIG;
	thread_pointer = (uintptr_t)__builtin_thread_pointer();
	if (__builtin_add_overflow((intptr_t)thread_pointer, __rseq_offset,
				   &signed_start) || signed_start <= 0)
		return -EINVAL;
	rseq_start = (uintptr_t)signed_start;
	if (rseq_start > UINTPTR_MAX - __rseq_size)
		return -EINVAL;
	rseq_end = rseq_start + __rseq_size;
	if (rseq_end > UINTPTR_MAX - (page_size - 1))
		return -EINVAL;
	*start = rseq_start & ~((uintptr_t)page_size - 1);
	*end = (rseq_end + page_size - 1) & ~((uintptr_t)page_size - 1);
#else
	(void)page_size;
#endif
	return 0;
}

static int cpu_accel_collect_unmap_ranges(
	struct cpu_accel_handle *handle, struct cpu_accel_user_context *context,
	unsigned long image_start, unsigned long image_end,
	unsigned long stack_start, unsigned long stack_end,
	unsigned long rseq_start, unsigned long rseq_end,
	unsigned long page_size)
{
	struct cpu_accel_user_keep_range keep[6] = {
		{ .start = image_start, .end = image_end },
		{ .start = stack_start, .end = stack_end },
		{ .start = (uintptr_t)handle->shared,
		  .end = (uintptr_t)handle->shared + CPU_ACCEL_MAP_SIZE },
		{ .start = (uintptr_t)context->region,
		  .end = context->region ?
			(uintptr_t)context->region +
			context->region->region_size : 0 },
		{ .start = rseq_start, .end = rseq_end },
		{ .start = context->reclaim_page,
		  .end = context->reclaim_page ?
			context->reclaim_page + context->reclaim_bytes : 0 },
	};
	FILE *maps;
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	int ret = 0;

	cpu_accel_sort_keep_ranges(keep, sizeof(keep) / sizeof(keep[0]));
	maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return -errno;

	while ((length = getline(&line, &capacity, maps)) >= 0) {
		unsigned long start, end, cursor;

		if (sscanf(line, "%lx-%lx", &start, &end) != 2 || start >= end) {
			ret = -EINVAL;
			break;
		}
		/* x86 keeps this fixed legacy mapping; munmap rejects it. */
		if (start == CPU_ACCEL_VSYSCALL_ADDR &&
		    end - start == page_size)
			continue;
		cursor = start;
		for (unsigned int i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
			if (keep[i].end <= cursor || keep[i].start >= end)
				continue;
			if (keep[i].start > cursor) {
				unsigned long stop = keep[i].start < end ?
					keep[i].start : end;

				ret = cpu_accel_add_unmap_range(context, cursor, stop);
				if (ret)
					goto out;
			}
			if (keep[i].end > cursor)
				cursor = keep[i].end;
			if (cursor >= end)
				break;
		}
		ret = cpu_accel_add_unmap_range(context, cursor, end);
		if (ret)
			break;
	}
	if (!ret && ferror(maps))
		ret = -EIO;

out:
	free(line);
	fclose(maps);
	return ret;
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if (errno || end == text || *end || parsed > UINT64_MAX)
		return -1;
	*value = parsed;
	return 0;
}

static uint64_t cpu_accel_hist_total(
		const struct cpu_accel_workload_region *region)
{
	uint64_t total = 0;

	if (!region)
		return 0;
	for (unsigned int i = 0; i < CPU_ACCEL_WORKLOAD_LATENESS_BINS; i++)
		total += ((const uint64_t *)
			  (region->payload + region->lateness_hist_offset))[i];
	return total;
}

/* Upper edge of the log2 bin containing the given permille rank. */
static uint64_t cpu_accel_hist_percentile(
		const struct cpu_accel_workload_region *region,
		uint64_t total, uint32_t permille)
{
	uint64_t target = (total * permille + 999) / 1000;
	uint64_t cum = 0;
	unsigned int i;

	if (!total || !target)
		return 0;
	for (i = 0; i < CPU_ACCEL_WORKLOAD_LATENESS_BINS; i++) {
		cum += ((const uint64_t *)
			(region->payload + region->lateness_hist_offset))[i];
		if (cum >= target)
			return i ? 1ULL << i : 0;
	}
	return 0;
}

static void print_status(const struct cpu_accel_shared *shared,
			 const struct cpu_accel_workload_region *region,
			 uint64_t work_bytes)
{
	bool telemetry_region = region &&
		region->region_role == CPU_ACCEL_WORKLOAD_ROLE_TELEMETRY;
	uint64_t user_active_ns =
		shared->user_active_end_ns > shared->user_active_start_ns ?
		shared->user_active_end_ns - shared->user_active_start_ns : 0;
	uint64_t hist_total = telemetry_region ?
		cpu_accel_hist_total(region) : 0;
	uint64_t avg_lateness_ns = hist_total ?
		(uint64_t)region->sum_lateness_ns / hist_total : 0;
	uint64_t samples = region ? region->samples_produced : 0;
	uint64_t max_lateness = region ? region->max_lateness_ns : 0;
	uint64_t min_lateness = region ? region->min_lateness_ns : 0;
	uint64_t last_lateness = region ? region->last_lateness_ns : 0;
	uint64_t work_iterations = region ? region->work_iterations : 0;

	printf("state=%u mode=%u sequence=%" PRIu64 " cpu=%u backend=%u backend_flags=%u"
	       " recovery_state=%u recovery_error=%d recovery_attempts=%u"
	       " recovery_requested_ns=%" PRIu64
	       " recovery_completed_ns=%" PRIu64 " samples=%" PRIu64
	       " max_lateness_ns=%" PRIu64 " min_lateness_ns=%" PRIu64
	       " last_lateness_ns=%" PRIu64 " duration_ns=%" PRIu64
	       " period_ns=%" PRIu64 " region_count=%u region_set_epoch=%"
	       PRIu64 " workload=%u work_bytes=%" PRIu64
	       " work_iterations=%" PRIu64
	       " lifecycle_entry_ns=%" PRIu64 " lifecycle_exit_ns=%" PRIu64
	       " user_active_start_ns=%" PRIu64
	       " user_active_end_ns=%" PRIu64 " user_active_ns=%" PRIu64
	       " user_escape_count=%" PRIu64
	       " irq_count=%" PRIu64
	       " irq_quarantined=%u irq_quarantine_blockers=%u"
	       " softirq_count=%" PRIu64 " timer_softirq_count=%" PRIu64
	       " hrtimer_softirq_count=%" PRIu64 " rcu_softirq_count=%" PRIu64
	       " sched_softirq_count=%" PRIu64 " workqueue_queued=%" PRIu64
	       " workqueue_executed=%" PRIu64 " context_switches=%" PRIu64
	       " need_resched_samples=%" PRIu64 " arch_irq_count=%" PRIu64
	       " arch_ipi_count=%" PRIu64 " arch_tlb_count=%" PRIu64
	       " arch_tlb_shootdown_targets=%" PRIu64
	       " arch_reschedule_deferred=%" PRIu64
	       " arch_call_function_deferred=%" PRIu64
	       " arch_nmi_count=%" PRIu64
	       " arch_mce_exception_count=%" PRIu64
	       " arch_mce_poll_count=%" PRIu64
	       " need_resched_entry=%u need_resched_exit=%u"
	       " softirq_pending_entry=%u softirq_pending_exit=%u"
	       " preempt_count_entry=%u preempt_count_exit=%u"
	       " arch_counters_valid=%u lifecycle_cpu_entry=%u"
	       " lifecycle_cpu_exit=%u migration_detected=%u"
	       " hist_total=%" PRIu64
	       " hist_p50_ns=%" PRIu64 " hist_p90_ns=%" PRIu64
	       " hist_p99_ns=%" PRIu64 " hist_p999_ns=%" PRIu64
	       " avg_lateness_ns=%" PRIu64 "\n",
	       shared->state, shared->mode, (uint64_t)shared->sequence,
	       shared->cpu,
	       shared->backend,
	       shared->backend_flags,
	       shared->recovery_state,
	       shared->recovery_error,
	       shared->recovery_attempts,
	       (uint64_t)shared->recovery_requested_ns,
	       (uint64_t)shared->recovery_completed_ns,
		       samples,
		       max_lateness,
		       min_lateness,
		       last_lateness,
	       (uint64_t)shared->duration_ns, (uint64_t)shared->period_ns,
		       shared->region_count,
		       (uint64_t)shared->region_set_epoch,
		       shared->workload, work_bytes,
		       work_iterations,
	       (uint64_t)shared->lifecycle_entry_ns,
	       (uint64_t)shared->lifecycle_exit_ns,
	       (uint64_t)shared->user_active_start_ns,
	       (uint64_t)shared->user_active_end_ns,
	       user_active_ns,
	       (uint64_t)shared->user_escape_count,
	       (uint64_t)shared->irq_count,
	       shared->irq_quarantined, shared->irq_quarantine_blockers,
	       (uint64_t)shared->softirq_count,
	       (uint64_t)shared->timer_softirq_count,
	       (uint64_t)shared->hrtimer_softirq_count,
	       (uint64_t)shared->rcu_softirq_count,
	       (uint64_t)shared->sched_softirq_count,
	       (uint64_t)shared->workqueue_queued,
	       (uint64_t)shared->workqueue_executed,
	       (uint64_t)shared->context_switches,
	       (uint64_t)shared->need_resched_samples,
	       (uint64_t)shared->arch_irq_count,
	       (uint64_t)shared->arch_ipi_count,
	       (uint64_t)shared->arch_tlb_count,
	       (uint64_t)shared->arch_tlb_shootdown_targets,
	       (uint64_t)shared->arch_reschedule_deferred,
	       (uint64_t)shared->arch_call_function_deferred,
	       (uint64_t)shared->arch_nmi_count,
	       (uint64_t)shared->arch_mce_exception_count,
	       (uint64_t)shared->arch_mce_poll_count,
	       shared->need_resched_entry, shared->need_resched_exit,
	       shared->softirq_pending_entry, shared->softirq_pending_exit,
	       shared->preempt_count_entry, shared->preempt_count_exit,
	       shared->arch_counters_valid,
	       shared->lifecycle_cpu_entry, shared->lifecycle_cpu_exit,
	       shared->migration_detected,
		       hist_total,
		       region ? cpu_accel_hist_percentile(region, hist_total,
							  500) : 0,
		       region ? cpu_accel_hist_percentile(region, hist_total,
							  900) : 0,
		       region ? cpu_accel_hist_percentile(region, hist_total,
							  990) : 0,
		       region ? cpu_accel_hist_percentile(region, hist_total,
							  999) : 0,
	       (uint64_t)avg_lateness_ns);

	if (!telemetry_region)
		return;
	for (unsigned int i = 0; i < CPU_ACCEL_WORKLOAD_LATENESS_BINS; i++) {
		uint64_t count = ((const uint64_t *)
			(region->payload + region->lateness_hist_offset))[i];

		if (!count)
			continue;
		printf("lateness_hist_bin=%u range_ns=[%" PRIu64 ",%" PRIu64
		       ") count=%" PRIu64 "\n", i,
		       (uint64_t)(i ? 1ULL << (i - 1) : 0),
		       (uint64_t)(1ULL << i),
		       count);
	}

	for (unsigned int i = 0; i <= CPU_ACCEL_WORKLOAD_LINEAR_BINS; i++) {
		uint64_t count = ((const uint64_t *)
			(region->payload + region->linear_hist_offset))[i];

		if (!count)
			continue;
		if (i < CPU_ACCEL_WORKLOAD_LINEAR_BINS)
			printf("linear_ns_hist_ns=%u count=%" PRIu64 "\n", i,
			       count);
		else
			printf("linear_ns_hist_ns>=%u count=%" PRIu64 "\n",
			       (unsigned int)CPU_ACCEL_WORKLOAD_LINEAR_BINS,
			       count);
	}
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s run [--cpu N] [--duration-ms N] [--period-us N]\n"
		"      [--workload timestamp|memmove|user-region|user-cyclictest|\n"
		"                   user-hang|user-reclaim|user-death|user-oslat]\n"
		"      [--work-bytes N]\n"
		"      [--test-reclaim-file PATH]\n"
		"      [--escape-after-ms N] [--escape-retries N]\n"
		"      [--worker-cgroup PATH]\n"
		"      [--persistent] [--require-quiescent] [--quarantine-irqs]\n"
		"  %s exit\n"
		"  %s stop\n"
		"  %s status\n"
		"  %s reset\n",
		program, program, program, program, program);
}

static int pin_control_cpu(void)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static int pin_cpu(unsigned int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static int move_self_to_cgroup(const char *cgroup)
{
	char procs_path[PATH_MAX];
	char pid[32];
	size_t written = 0;
	int fd, path_len, pid_len;

	if (!cgroup)
		return 0;

	path_len = snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs",
			    cgroup);
	if (path_len < 0 || path_len >= (int)sizeof(procs_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	pid_len = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
	if (pid_len < 0 || pid_len >= (int)sizeof(pid)) {
		errno = EOVERFLOW;
		return -1;
	}

	fd = open(procs_path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	while (written < (size_t)pid_len) {
		ssize_t ret = write(fd, pid + written, pid_len - written);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0) {
			if (!ret)
				errno = EIO;
			close(fd);
			return -1;
		}
		written += ret;
	}
	return close(fd);
}

static int close_worker_fds(int keep_fd)
{
	if (keep_fd <= STDERR_FILENO)
		return syscall(SYS_close_range, 3U, UINT_MAX, 0U);
	if (keep_fd > STDERR_FILENO + 1 &&
	    syscall(SYS_close_range, 3U, (unsigned int)keep_fd - 1, 0U) < 0)
		return -1;
	return syscall(SYS_close_range, (unsigned int)keep_fd + 1, UINT_MAX, 0U);
}

static bool cpu_accel_user_workload_needs_region(uint32_t workload)
{
	return workload == CPU_ACCEL_WORKLOAD_USER_REGION ||
		workload == CPU_ACCEL_WORKLOAD_USER_CYCLICTEST ||
		workload == CPU_ACCEL_WORKLOAD_USER_OSLAT;
}

static uint64_t cpu_accel_align_up_page(uint64_t value, uint64_t page_size)
{
	return (value + page_size - 1) & ~(page_size - 1);
}

static int cpu_accel_workload_region_size(uint32_t workload,
					  uint64_t work_bytes,
					  uint64_t page_size,
					  uint64_t max_region_bytes,
					  uint64_t *size_bytes)
{
	uint64_t payload_bytes;
	uint64_t raw_size;

	if (!page_size || (page_size & (page_size - 1)))
		return -EINVAL;
	if (workload == CPU_ACCEL_WORKLOAD_USER_REGION) {
		if (!work_bytes || work_bytes > CPU_ACCEL_MAX_WORK_BYTES)
			return -EINVAL;
		payload_bytes = work_bytes * 2;
	} else {
		payload_bytes = (uint64_t)CPU_ACCEL_WORKLOAD_MAX_SAMPLES *
			sizeof(struct cpu_accel_workload_sample) +
			CPU_ACCEL_WORKLOAD_LATENESS_BINS * sizeof(uint64_t) +
			(CPU_ACCEL_WORKLOAD_LINEAR_BINS + 1) *
			sizeof(uint64_t);
	}
	if (payload_bytes > UINT64_MAX - CPU_ACCEL_WORKLOAD_HEADER_BYTES)
		return -EINVAL;
	raw_size = CPU_ACCEL_WORKLOAD_HEADER_BYTES + payload_bytes;
	*size_bytes = cpu_accel_align_up_page(raw_size, page_size);
	if (*size_bytes > max_region_bytes)
		return -EINVAL;
	return 0;
}

static void
cpu_accel_workload_region_init(struct cpu_accel_workload_region *region,
			       uint64_t size_bytes, uint64_t epoch,
			       uint64_t token, uint32_t workload,
			       uint64_t work_bytes)
{
	memset(region, 0, size_bytes);
	region->magic = CPU_ACCEL_WORKLOAD_MAGIC;
	region->abi_version = CPU_ACCEL_WORKLOAD_ABI_VERSION;
	region->header_size = CPU_ACCEL_WORKLOAD_HEADER_BYTES;
	region->region_role = workload == CPU_ACCEL_WORKLOAD_USER_REGION ?
		CPU_ACCEL_WORKLOAD_ROLE_BUFFER :
		CPU_ACCEL_WORKLOAD_ROLE_TELEMETRY;
	region->region_size = size_bytes;
	region->region_epoch = epoch;
	region->mmap_token = token;
	region->work_bytes = work_bytes;
	if (workload == CPU_ACCEL_WORKLOAD_USER_REGION) {
		uint64_t entries = work_bytes <
			CPU_ACCEL_WORKLOAD_RING_MAX_ENTRIES ? work_bytes :
			CPU_ACCEL_WORKLOAD_RING_MAX_ENTRIES;

		region->payload_offset = 0;
		region->ring_entries = entries;
		region->ring_block_bytes = (work_bytes + entries - 1) / entries;
		for (uint64_t index = 0; index < work_bytes; index++)
			region->payload[index] = (uint8_t)index;
	} else {
		region->samples_offset = 0;
		region->lateness_hist_offset =
			(uint64_t)CPU_ACCEL_WORKLOAD_MAX_SAMPLES *
			sizeof(struct cpu_accel_workload_sample);
		region->linear_hist_offset =
			region->lateness_hist_offset +
			CPU_ACCEL_WORKLOAD_LATENESS_BINS * sizeof(uint64_t);
		region->payload_offset =
			region->linear_hist_offset +
			(CPU_ACCEL_WORKLOAD_LINEAR_BINS + 1) *
			sizeof(uint64_t);
	}
}

static int run_user_worker_image(struct cpu_accel_handle *handle,
				 struct cpu_accel_config *config,
				 const char *reclaim_file,
				 uint64_t region_token, uint64_t region_size,
				 uint64_t region_epoch)
{
	struct cpu_accel_user_context *context;
	struct cpu_accel_region_create region_create = {};
	struct cpu_accel_workload_region *owner_region = MAP_FAILED;
	uintptr_t entry_ip;
	uintptr_t image_end;
	uintptr_t image_start;
	uintptr_t stack_start;
	uintptr_t stack_end;
	unsigned long rseq_start;
	unsigned long rseq_end;
	size_t reclaim_bytes = 0;
	size_t unmap_offset;
	void *stack;
	void *reclaim_mapping = MAP_FAILED;
	long page_size;
	int reclaim_fd = -1;
	int user_hang = config->workload == CPU_ACCEL_WORKLOAD_USER_HANG;
	int user_reclaim = config->workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM;
	int user_death = config->workload == CPU_ACCEL_WORKLOAD_USER_DEATH;
	int user_oslat = config->workload == CPU_ACCEL_WORKLOAD_USER_OSLAT;
	int user_region = config->workload == CPU_ACCEL_WORKLOAD_USER_REGION;
	int ret = 1;

	if (region_token) {
		region_create.size_bytes = region_size;
		region_create.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL;
		region_create.mmap_token = region_token;
		owner_region = cpu_accel_region_mmap(handle, &region_create,
						     false);
		if (owner_region == MAP_FAILED) {
			perror("map owner workload region");
			goto out;
		}

		if (!cpu_accel_workload_header_valid(owner_region, region_size,
						     region_epoch,
						     region_token)) {
			fprintf(stderr, "invalid inherited workload region\n");
			goto out;
		}
		if (config->workload == CPU_ACCEL_WORKLOAD_USER_REGION) {
			config->work_bytes = owner_region->work_bytes;
			if (!config->work_bytes ||
			    config->work_bytes > CPU_ACCEL_MAX_WORK_BYTES) {
				fprintf(stderr,
					"invalid inherited workload size\n");
				goto out;
			}
		}
	} else if (cpu_accel_user_workload_needs_region(config->workload)) {
		fprintf(stderr, "missing workload region mapping\n");
		goto out;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || (size_t)page_size > SIZE_MAX / 16 ||
	    (size_t)page_size > SIZE_MAX / CPU_ACCEL_USER_PTE_TABLE_PAGES) {
		fprintf(stderr, "invalid page size\n");
		goto out;
	}
	image_start = (uintptr_t)__start_cpu_accel_user_image;
	image_end = (uintptr_t)__stop_cpu_accel_user_image;
	if (image_end <= image_start ||
	    image_end > UINTPTR_MAX - ((uintptr_t)page_size - 1) ||
	    (image_start & ((uintptr_t)page_size - 1))) {
		fprintf(stderr, "invalid accelerator image section bounds\n");
		goto out;
	}
	image_end = (image_end + (uintptr_t)page_size - 1) &
		~((uintptr_t)page_size - 1);
	if (user_reclaim) {
		struct stat st;
		int private_pte_tables;

		if (!reclaim_file || !strcmp(reclaim_file, "-")) {
			fprintf(stderr, "user-reclaim requires a test file\n");
			goto out;
		}
		reclaim_fd = open(reclaim_file, O_RDONLY | O_CLOEXEC);
		if (reclaim_fd < 0) {
			perror("open test reclaim file");
			goto out;
		}
		if (fstat(reclaim_fd, &st) < 0) {
			perror("stat test reclaim file");
			goto out;
		}
		if (!S_ISREG(st.st_mode) || st.st_size < page_size ||
		    st.st_size > (off_t)CPU_ACCEL_MAX_USER_RECLAIM_BYTES ||
		    st.st_size % page_size) {
			fprintf(stderr,
				"reclaim file must be page-aligned and 1 page to 64 MiB\n");
			goto out;
		}
		reclaim_bytes = st.st_size;
		private_pte_tables = reclaim_bytes >
			(size_t)page_size * CPU_ACCEL_USER_PTE_TABLE_PAGES;
		reclaim_mapping = mmap(NULL, reclaim_bytes,
				       private_pte_tables ?
				       PROT_READ | PROT_WRITE : PROT_READ,
				       private_pte_tables ? MAP_PRIVATE : MAP_SHARED,
				       reclaim_fd, 0);
		if (reclaim_mapping == MAP_FAILED) {
			perror("mmap test reclaim page");
			goto out;
		}
		if (private_pte_tables &&
		    (madvise(reclaim_mapping, reclaim_bytes, MADV_NOHUGEPAGE) ||
		     madvise(reclaim_mapping, reclaim_bytes, MADV_RANDOM))) {
			perror("madvise test reclaim mapping");
			goto out;
		}
		close(reclaim_fd);
		reclaim_fd = -1;
		/* Populate file-backed PTEs before the active epoch. */
		if (cpu_accel_populate_file_mapping(reclaim_mapping, reclaim_bytes,
						    page_size,
						    private_pte_tables)) {
			perror("populate test reclaim mapping");
			goto out;
		}
	} else if (reclaim_file && strcmp(reclaim_file, "-")) {
		fprintf(stderr, "test reclaim file requires user-reclaim workload\n");
		goto out;
	}
	stack = mmap(NULL, (size_t)page_size * 16,
		     PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		perror("mmap user accelerator stack");
		goto out;
	}

	context = stack;
	context->region = owner_region == MAP_FAILED ? NULL : owner_region;
	context->shared = (struct cpu_accel_shared *)(uintptr_t)handle->shared;
	context->reclaim_page = user_reclaim ?
		(uintptr_t)reclaim_mapping : 0;
	context->reclaim_bytes = reclaim_bytes;
	context->fd = handle->fd;
	context->cycles_per_ns = cpu_accel_calibrate_cycles_per_ns();
	if (user_hang)
		entry_ip = (uintptr_t)cpu_accel_user_hang;
	else if (user_death)
		entry_ip = (uintptr_t)cpu_accel_user_death;
	else if (user_oslat)
		entry_ip = (uintptr_t)cpu_accel_user_oslat;
	else if (user_region)
		entry_ip = (uintptr_t)cpu_accel_user_region_workload;
	else
		entry_ip = (uintptr_t)cpu_accel_user_cyclictest;
	config->flags |= CPU_ACCEL_FLAG_PERSISTENT;
	config->user_entry_ip = entry_ip;
	config->user_escape_ip = (uintptr_t)cpu_accel_user_escape_image;
	config->user_stack_top = (uintptr_t)stack + (size_t)page_size * 16;
	config->user_stack_bytes = (size_t)page_size * 16;
	config->user_reclaim_page = user_reclaim ?
		(uintptr_t)reclaim_mapping : 0;
	context->stack_top = config->user_stack_top;
	if (entry_ip < image_start || entry_ip >= image_end ||
	    (uintptr_t)cpu_accel_user_escape_image < image_start ||
	    (uintptr_t)cpu_accel_user_escape_image >= image_end) {
		fprintf(stderr, "invalid accelerator image section bounds\n");
		goto out_stack;
	}
	config->user_image_start = image_start;
	config->user_image_bytes = image_end - image_start;
	config->user_arg = (uintptr_t)context;
	stack_start = (uintptr_t)stack;
	stack_end = (uintptr_t)stack + (size_t)page_size * 16;
	ret = cpu_accel_get_rseq_keep_range((unsigned long)page_size,
					    &rseq_start, &rseq_end);
	if (ret) {
		errno = -ret;
		perror("locate registered rseq area");
		goto out_stack;
	}
	unmap_offset = (sizeof(*context) +
			_Alignof(struct cpu_accel_user_unmap_range) - 1) &
		~((size_t)_Alignof(struct cpu_accel_user_unmap_range) - 1);
	context->unmap_ranges = (void *)((uintptr_t)stack + unmap_offset);
	context->nr_unmap_ranges = 0;
	ret = cpu_accel_collect_unmap_ranges(handle, context, image_start,
					     image_end, stack_start, stack_end,
					     rseq_start, rseq_end,
					     (unsigned long)page_size);
	if (ret) {
		errno = -ret;
		perror("collect mappings to seal user worker");
		goto out_stack;
	}

	if (cpu_accel_configure(handle, config) < 0) {
		perror("configure user accelerator");
		goto out_stack;
	}
	/* The image trampoline unmaps these VMAs, starts the workload, then exits. */
	cpu_accel_user_launch(context);

out_stack:
	munmap(stack, (size_t)page_size * 16);
out:
	if (reclaim_fd >= 0)
		close(reclaim_fd);
	if (reclaim_mapping != MAP_FAILED)
		munmap(reclaim_mapping, reclaim_bytes);
	if (owner_region != MAP_FAILED)
		munmap(owner_region, region_size);
	cpu_accel_close(handle);
	return ret;
}

static int run_user_worker(int argc, char **argv)
{
	enum { USER_WORKER_ARG_COUNT = 9 };
	struct cpu_accel_config config = {};
	struct cpu_accel_handle handle;
	uint64_t values[USER_WORKER_ARG_COUNT];
	const char *reclaim_file = argv[USER_WORKER_ARG_COUNT + 2];
	int fd;

	if (argc != USER_WORKER_ARG_COUNT + 3) {
		fprintf(stderr, "invalid internal user-worker arguments\n");
		return 2;
	}
	for (int i = 0; i < USER_WORKER_ARG_COUNT; i++) {
		if (parse_u64(argv[i + 2], &values[i])) {
			fprintf(stderr, "invalid internal user-worker argument\n");
			return 2;
		}
	}
	if (values[0] > INT_MAX || values[1] > UINT_MAX ||
	    values[2] > CPU_ACCEL_WORKLOAD_USER_OSLAT ||
	    values[3] > UINT_MAX ||
	    (values[6] && values[7] > SIZE_MAX) ||
	    (values[2] != CPU_ACCEL_WORKLOAD_USER_CYCLICTEST &&
	     values[2] != CPU_ACCEL_WORKLOAD_USER_REGION &&
	     values[2] != CPU_ACCEL_WORKLOAD_USER_HANG &&
	     values[2] != CPU_ACCEL_WORKLOAD_USER_RECLAIM &&
	     values[2] != CPU_ACCEL_WORKLOAD_USER_DEATH &&
	     values[2] != CPU_ACCEL_WORKLOAD_USER_OSLAT)) {
		fprintf(stderr, "out-of-range internal user-worker argument\n");
		return 2;
	}
	if ((!!values[6] || !!values[7] || !!values[8]) !=
	    cpu_accel_user_workload_needs_region(values[2])) {
		fprintf(stderr, "invalid internal user-worker region\n");
		return 2;
	}
	if ((values[2] == CPU_ACCEL_WORKLOAD_USER_RECLAIM &&
	     (!*reclaim_file || !strcmp(reclaim_file, "-"))) ||
	    (values[2] != CPU_ACCEL_WORKLOAD_USER_RECLAIM &&
	     strcmp(reclaim_file, "-"))) {
		fprintf(stderr, "invalid internal user-worker reclaim file\n");
		return 2;
	}

	fd = values[0];
	config.cpu = values[1];
	config.workload = values[2];
	config.flags = values[3];
	config.duration_ns = values[4];
	config.period_ns = values[5];
	if (pin_cpu(config.cpu) < 0) {
		perror("pin user accelerator");
		close(fd);
		return 1;
	}
	if (cpu_accel_attach_fd(&handle, fd) < 0) {
		perror("map inherited accelerator device");
		close(fd);
		return 1;
	}
	return run_user_worker_image(&handle, &config, reclaim_file,
				     values[6], values[7], values[8]);
}

static bool user_escape_completed(
		const volatile struct cpu_accel_shared *shared)
{
	return __atomic_load_n(&shared->state, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_STATE_ESCAPED &&
	       __atomic_load_n(&shared->recovery_state, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_RECOVERY_SUCCEEDED &&
	       __atomic_load_n(&shared->user_escape_count, __ATOMIC_ACQUIRE) == 1;
}

static bool user_reclaim_owner_stop_completed(
		const volatile struct cpu_accel_shared *shared)
{
	return __atomic_load_n(&shared->state, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_STATE_COMPLETE &&
	       __atomic_load_n(&shared->mode, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_MODE_LINUX &&
	       __atomic_load_n(&shared->backend, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_BACKEND_X86_RING3 &&
	       __atomic_load_n(&shared->recovery_state, __ATOMIC_ACQUIRE) ==
		CPU_ACCEL_RECOVERY_FAILED &&
	       __atomic_load_n(&shared->recovery_error, __ATOMIC_ACQUIRE) ==
		-EALREADY &&
	       __atomic_load_n(&shared->user_escape_count, __ATOMIC_ACQUIRE) == 0;
}

static int maybe_write_active_marker(
		const volatile struct cpu_accel_shared *shared,
		bool *marker_written);

static int wait_for_user_worker(pid_t child, int *status,
				const volatile struct cpu_accel_shared *shared,
				uint64_t timeout_ms)
{
	/* Test harnesses may wait for the ring-3 owner before injecting activity. */
	const char *active_marker = getenv("CPU_ACCEL_ACTIVE_MARKER");
	const struct timespec poll_delay = {
		.tv_sec = 0,
		.tv_nsec = 1000000,
	};
	bool marker_written = false;
	uint64_t waited_ms = 0;

	if ((!active_marker || !*active_marker) && !timeout_ms)
		return waitpid(child, status, 0) < 0 ? -1 : 0;

	for (;;) {
		pid_t waited;

		if (maybe_write_active_marker(shared, &marker_written) < 0)
			return -1;

		waited = waitpid(child, status, WNOHANG);
		if (waited == child)
			return 0;
		if (waited < 0)
			return -1;
		if (timeout_ms && waited_ms >= timeout_ms)
			return 1;
		nanosleep(&poll_delay, NULL);
		if (timeout_ms)
			waited_ms++;
	}
}

/* Publish CPU_ACCEL_ACTIVE_MARKER once the owner is verifiably active so test
 * harnesses can inject activity (e.g. RCU callbacks) without a fixed delay.
 * Returns 0 when no write was needed or it succeeded, -1 on write error.
 */
static int maybe_write_active_marker(
		const volatile struct cpu_accel_shared *shared,
		bool *marker_written)
{
	const char *active_marker = getenv("CPU_ACCEL_ACTIVE_MARKER");
	static const char ready[] = "active\n";
	int fd;

	if (*marker_written || !active_marker || !*active_marker)
		return 0;
	if (__atomic_load_n(&shared->state, __ATOMIC_ACQUIRE) !=
	    CPU_ACCEL_STATE_RUNNING ||
	    __atomic_load_n(&shared->mode, __ATOMIC_ACQUIRE) !=
	    CPU_ACCEL_MODE_ACCELERATOR ||
	    !__atomic_load_n(&shared->user_active_start_ns, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&shared->user_active_end_ns, __ATOMIC_ACQUIRE))
		return 0;
	fd = open(active_marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror("create active marker");
		return -1;
	}
	if (write(fd, ready, sizeof(ready) - 1) != sizeof(ready) - 1) {
		perror("write active marker");
		close(fd);
		return -1;
	}
	close(fd);
	*marker_written = true;
	return 0;
}

/* A2: the manager owns the wall-clock duration. Wait for the owner to reach
 * RUNNING, sleep the requested duration against CLOCK_MONOTONIC (this process
 * is pinned to a ticked CPU, so the absolute sleep is accurate), then send an
 * orderly STOP. Sets *status to the owner's wait status on return. */
static int cpu_accel_run_until_stop(struct cpu_accel_handle *handle,
					  pid_t child, int *status,
					  uint64_t duration_ns)
{
	const struct timespec poll = { 0, 1000000 };
	struct timespec now;
	struct timespec abs;
	bool marker_written = false;
	bool need_marker = getenv("CPU_ACCEL_ACTIVE_MARKER") != NULL;

	for (;;) {
		pid_t waited = waitpid(child, status, WNOHANG);

		if (waited == child)
			return 0;	/* owner already exited (early) */
		if (waited < 0)
			return -1;
		if (maybe_write_active_marker(handle->shared, &marker_written) < 0)
			return -1;
		if (__atomic_load_n(&handle->shared->state, __ATOMIC_ACQUIRE) ==
		    CPU_ACCEL_STATE_RUNNING &&
		    (!need_marker || marker_written))
			break;
		nanosleep(&poll, NULL);
	}

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return -1;
	abs.tv_sec = now.tv_sec + (time_t)(duration_ns / 1000000000ULL);
	abs.tv_nsec = (long)(now.tv_nsec + (long)(duration_ns % 1000000000ULL));
	if (abs.tv_nsec >= 1000000000L) {
		abs.tv_sec++;
		abs.tv_nsec -= 1000000000L;
	}
	if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL) < 0 &&
	    errno != EINTR)
		return -1;

	if (cpu_accel_stop(handle) < 0) {
		if (errno != EALREADY)
			return -1;
		if (waitpid(child, status, WNOHANG) != child)
			return -1;
		return 0;
	}

	if (waitpid(child, status, 0) < 0)
		return -1;
	return 0;
}

struct cpu_accel_region_consumer {
	struct cpu_accel_workload_region *region;
	uint64_t work_bytes;
	int status;
	int stop;
};

static void *cpu_accel_consume_user_region(void *argument)
{
	struct cpu_accel_region_consumer *consumer = argument;
	struct cpu_accel_workload_region *region = consumer->region;
	const struct timespec poll = { 0, 100000 };
	uint64_t entries;
	uint64_t block_bytes;

	entries = __atomic_load_n(&region->ring_entries, __ATOMIC_RELAXED);
	block_bytes = __atomic_load_n(&region->ring_block_bytes,
				      __ATOMIC_RELAXED);
	if (!consumer->work_bytes || !entries || !block_bytes ||
	    entries > CPU_ACCEL_WORKLOAD_RING_MAX_ENTRIES ||
	    block_bytes !=
		    (consumer->work_bytes + entries - 1) / entries) {
		__atomic_store_n(&region->manager_ready, 2, __ATOMIC_RELEASE);
		consumer->status = 1;
		return NULL;
	}
	__atomic_store_n(&region->manager_ready, 1, __ATOMIC_RELEASE);

	for (uint64_t entry = 0; entry < entries; entry++) {
		uint64_t base = entry * block_bytes;
		uint64_t count = block_bytes;
		uint64_t sequence;

		if (count > consumer->work_bytes - base)
			count = consumer->work_bytes - base;
		sequence = __atomic_load_n(&region->command_sequence,
					   __ATOMIC_RELAXED);
		if (sequence != entry) {
			__atomic_store_n(&region->command_overflow, 1,
					 __ATOMIC_RELAXED);
			consumer->status = 1;
			return NULL;
		}
		for (uint64_t index = 0; index < count; index++)
			region->payload[base + index] = (uint8_t)(base + index);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		__atomic_store_n(&region->command_sequence, entry + 1,
				 __ATOMIC_RELEASE);

		for (;;) {
			sequence = __atomic_load_n(&region->result_sequence,
						   __ATOMIC_ACQUIRE);
			if (sequence > entry)
				break;
			if (consumer->stop ||
			    __atomic_load_n(&region->completion_sequence,
					    __ATOMIC_ACQUIRE)) {
				consumer->status = 1;
				return NULL;
			}
			nanosleep(&poll, NULL);
		}
		if (sequence != entry + 1) {
			__atomic_store_n(&region->result_overflow, 1,
					 __ATOMIC_RELAXED);
			consumer->status = 1;
			return NULL;
		}
		for (uint64_t index = 0; index < count; index++) {
			uint8_t expected = (uint8_t)(base + index) ^ 0x5a;

			if (region->payload[region->payload_offset +
					    consumer->work_bytes + base +
					    index] !=
			    expected) {
				consumer->status = 1;
				return NULL;
			}
		}
		__atomic_store_n(&region->result_consumed, entry + 1,
				 __ATOMIC_RELEASE);
	}
	consumer->status = 0;
	return NULL;
}

static int run_user_workload(const struct cpu_accel_config *requested,
			     uint64_t escape_after_ms,
			     uint64_t escape_attempts,
			     const char *reclaim_file,
			     const char *worker_cgroup)
{
	struct cpu_accel_config config = *requested;
	struct cpu_accel_handle handle;
	struct cpu_accel_capabilities capabilities = {};
	struct cpu_accel_region_create region_create = {};
	struct cpu_accel_workload_region *workload_region = MAP_FAILED;
	uint64_t region_epoch = 0;
	char fd_arg[24], cpu_arg[24], workload_arg[24];
	char flags_arg[24], duration_arg[24], period_arg[24];
	char region_token_arg[24], region_size_arg[24], region_epoch_arg[24];
	const char *reclaim_file_arg = reclaim_file ? reclaim_file : "-";
	char *const worker_argv[] = {
		"/proc/self/exe", "--cpu-accel-user-worker", fd_arg,
		cpu_arg, workload_arg, flags_arg, duration_arg, period_arg,
		region_token_arg, region_size_arg, region_epoch_arg,
		(char *)reclaim_file_arg, NULL,
	};
	char *const worker_env[] = { NULL };
	pid_t child;
	pthread_t consumer_thread;
	struct cpu_accel_region_consumer consumer = {};
	int status;
	int user_hang = requested->workload == CPU_ACCEL_WORKLOAD_USER_HANG;
	int user_reclaim = requested->workload ==
		CPU_ACCEL_WORKLOAD_USER_RECLAIM;
	bool needs_region =
		cpu_accel_user_workload_needs_region(requested->workload);
	bool region_created = false;
	bool region_mapped = false;
	bool consumer_started = false;
	bool reclaim_owner_stopped;
	uint64_t completion_sequence = 0;
	int escape_ret = 0;
	int ret;

	if (cpu_accel_open(&handle) < 0) {
		perror("open /dev/cpu_accel");
		return 1;
	}
	if (cpu_accel_get_capabilities(&handle, &capabilities) < 0) {
		perror("get workload region capabilities");
		cpu_accel_close(&handle);
		return 1;
	}

	if (user_hang && !escape_after_ms) {
		fprintf(stderr, "user-hang requires --escape-after-ms\n");
		cpu_accel_close(&handle);
		return 2;
	}
	/* Keep timeout and escape control runnable while the target is owned. */
	if (pin_control_cpu() < 0) {
		perror("pin user control process to CPU 0");
		cpu_accel_close(&handle);
		return 1;
	}

	/* The device opener is the companion, not a valid accelerator worker. */
	errno = 0;
	ret = cpu_accel_configure(&handle, &config);
	if (ret != -1 || errno != EXDEV) {
		int configure_errno = errno;

		fprintf(stderr,
			"same-mm user admission returned %d/errno %d, expected EXDEV\n",
			ret, configure_errno);
		cpu_accel_close(&handle);
		return 1;
	}
	config = *requested;
	/* A2: no CPU_ACCEL_FLAG_PERSISTENT -- no kernel watchdog. The cyclictest
	 * image is open-ended (run-forever); the manager owns the wall-clock
	 * duration and sends the orderly STOP. */
	if (needs_region) {
		struct cpu_accel_region_ref region_ref = {};
		uint64_t region_size;
		long page_size = sysconf(_SC_PAGESIZE);

		if (cpu_accel_workload_region_size(config.workload,
						   config.work_bytes,
						   page_size,
						   capabilities.max_region_bytes,
						   &region_size)) {
			errno = EINVAL;
			perror("size workload region");
			cpu_accel_close(&handle);
			return 1;
		}
		region_create.size_bytes = region_size;
		region_create.permissions =
			CPU_ACCEL_REGION_PERM_BIDIRECTIONAL;
		region_create.numa_node = -1;
		if (cpu_accel_region_create(&handle, &region_create) < 0) {
			perror("create workload region");
			cpu_accel_close(&handle);
			return 1;
		}
		region_created = true;
		workload_region = cpu_accel_region_mmap(&handle,
							&region_create, true);
		if (workload_region == MAP_FAILED) {
			perror("map manager workload region");
			goto out_close;
		}
		region_mapped = true;
		cpu_accel_workload_region_init(workload_region, region_size,
					       region_create.epoch,
					       region_create.mmap_token,
					       config.workload,
					       config.work_bytes);
		region_ref.region_id = region_create.region_id;
		region_ref.epoch = region_create.epoch;
		if (cpu_accel_region_attach(&handle, &region_ref) < 0) {
			perror("attach workload region");
			goto out_close;
		}
		region_epoch = region_ref.epoch;
		workload_region->region_epoch = region_epoch;
	} else {
		region_create.mmap_token = 0;
		region_create.size_bytes = 0;
		region_create.epoch = 0;
	}
	snprintf(fd_arg, sizeof(fd_arg), "%d", handle.fd);
	snprintf(cpu_arg, sizeof(cpu_arg), "%u", config.cpu);
	snprintf(workload_arg, sizeof(workload_arg), "%u", config.workload);
	snprintf(flags_arg, sizeof(flags_arg), "%u", config.flags);
	snprintf(duration_arg, sizeof(duration_arg), "%llu",
		 (unsigned long long)config.duration_ns);
	snprintf(period_arg, sizeof(period_arg), "%llu",
		 (unsigned long long)config.period_ns);
	snprintf(region_token_arg, sizeof(region_token_arg), "%llu",
		 (unsigned long long)region_create.mmap_token);
	snprintf(region_size_arg, sizeof(region_size_arg), "%llu",
		 (unsigned long long)region_create.size_bytes);
	snprintf(region_epoch_arg, sizeof(region_epoch_arg), "%llu",
		 (unsigned long long)region_epoch);

	child = fork();
	if (child < 0) {
		perror("fork user accelerator");
		goto out_close;
	}
	if (!child) {
		int fd_flags = fcntl(handle.fd, F_GETFD);

		if (close_worker_fds(handle.fd) < 0) {
			perror("close unrelated descriptors before exec");
			_exit(1);
		}
		if (fd_flags < 0 ||
		    fcntl(handle.fd, F_SETFD, fd_flags & ~FD_CLOEXEC) < 0) {
			perror("prepare accelerator fd for exec");
			_exit(1);
		}
		if (pin_cpu(config.cpu) < 0) {
			perror("pin user accelerator");
			_exit(1);
		}
		if (move_self_to_cgroup(worker_cgroup) < 0) {
			perror("move user accelerator to cgroup");
			_exit(1);
		}
		execve("/proc/self/exe", worker_argv, worker_env);
		perror("exec isolated accelerator worker");
		_exit(127);
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION) {
		consumer.region = workload_region;
		consumer.work_bytes = config.work_bytes;
		if (pthread_create(&consumer_thread, NULL,
				   cpu_accel_consume_user_region,
				   &consumer)) {
			__atomic_store_n(&workload_region->manager_ready, 2,
					 __ATOMIC_RELEASE);
		} else {
			consumer_started = true;
		}
	}

	/* No escape: A2 -- this manager (pinned to CPU 0, a ticked CPU) owns the
	 * wall-clock duration. The owner's cyclictest image is open-ended (no
	 * PERSISTENT, no kernel watchdog), so it runs forever until this manager
	 * sends an orderly STOP. The STOP sets stop_requested; the image breaks,
	 * exits via USER_EXIT, and the driver records state=COMPLETE.
	 *
	 * With --escape-after-ms: wait that long for the owner to stop on its own
	 * (e.g. kernel maintenance draining it); if it is still active, fire the
	 * timed NMI escape and wait for recovery.
	 */
	if (escape_after_ms) {
		int wait_ret = wait_for_user_worker(child, &status,
						    handle.shared,
						    escape_after_ms);

		if (wait_ret == 1) {
			for (uint64_t attempt = 0; attempt < escape_attempts;
			     attempt++) {
				escape_ret = cpu_accel_user_escape(&handle);
				if (!escape_ret)
					break;
				if (errno == EINVAL &&
				    user_escape_completed(handle.shared)) {
					escape_ret = 0;
					break;
				}
				if (errno == EALREADY && user_reclaim &&
				    user_reclaim_owner_stop_completed(handle.shared)) {
					/* Reclaim's subscribed stop beat the timed NMI. */
					escape_ret = 0;
					break;
				}
			}
			if (escape_ret)
				perror("user escape");
			wait_ret = wait_for_user_worker(child, &status,
							handle.shared, 0);
		}
		if (wait_ret < 0) {
			ret = -1;
		} else if (escape_ret) {
			ret = -1;
		} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
			fprintf(stderr,
				"user accelerator child failed with status %d\n",
				WEXITSTATUS(status));
			ret = -1;
		} else if (WIFSIGNALED(status)) {
			fprintf(stderr, "user accelerator child killed by signal %d\n",
				WTERMSIG(status));
			ret = -1;
		} else {
			ret = 0;
		}
	} else if (cpu_accel_run_until_stop(&handle, child, &status,
						       config.duration_ns) < 0) {
		perror("manager duration/stop");
		ret = -1;
	} else if (WIFEXITED(status) && WEXITSTATUS(status)) {
		fprintf(stderr, "user accelerator child failed with status %d\n",
			WEXITSTATUS(status));
		ret = -1;
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "user accelerator child killed by signal %d\n",
			WTERMSIG(status));
		ret = -1;
	} else {
		ret = 0;
	}
	if (consumer_started) {
		__atomic_store_n(&consumer.stop, 1, __ATOMIC_RELEASE);
		if (pthread_join(consumer_thread, NULL) ||
		    consumer.status)
			ret = -1;
	}
	if (needs_region)
		completion_sequence =
			__atomic_load_n(&workload_region->completion_sequence,
					__ATOMIC_ACQUIRE);
	print_status((struct cpu_accel_shared *)handle.shared,
		     region_mapped ? workload_region : NULL,
		     config.work_bytes);
	reclaim_owner_stopped = user_reclaim &&
		user_reclaim_owner_stop_completed(handle.shared);
	if ((escape_after_ms && handle.shared->state != CPU_ACCEL_STATE_ESCAPED &&
	     !reclaim_owner_stopped) ||
	    (user_reclaim && handle.shared->state != CPU_ACCEL_STATE_ESCAPED &&
	     !reclaim_owner_stopped) ||
	    (!escape_after_ms && !user_reclaim &&
	     handle.shared->state != CPU_ACCEL_STATE_COMPLETE) ||
	    handle.shared->mode != CPU_ACCEL_MODE_LINUX ||
	    handle.shared->backend != CPU_ACCEL_BACKEND_X86_RING3 ||
	    ((escape_after_ms || user_reclaim) &&
	     !reclaim_owner_stopped &&
	     handle.shared->recovery_state !=
	     CPU_ACCEL_RECOVERY_SUCCEEDED) ||
	    (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION &&
	     completion_sequence != 1) ||
	    ((escape_after_ms || user_reclaim) &&
	     !reclaim_owner_stopped &&
	     handle.shared->user_escape_count != 1)) {
		fprintf(stderr, "user accelerator did not complete its ring-3 contract\n");
		ret = -1;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION &&
	    (workload_region->command_sequence !=
		     workload_region->ring_entries ||
	     workload_region->result_sequence !=
		     workload_region->ring_entries ||
	     workload_region->result_consumed !=
		     workload_region->ring_entries ||
	     workload_region->command_overflow ||
	     workload_region->result_overflow)) {
		fprintf(stderr, "user region ring validation failed\n");
		ret = -1;
	}
	if (cpu_accel_exit(&handle) < 0) {
		perror("exit user accelerator");
		ret = -1;
	} else if (cpu_accel_reset(&handle) < 0) {
		perror("reset user accelerator");
		ret = -1;
	}
	if (needs_region && config.workload == CPU_ACCEL_WORKLOAD_USER_REGION &&
	    __atomic_load_n(&workload_region->completion_sequence,
			    __ATOMIC_ACQUIRE) == 1) {
		const uint8_t *input = workload_region->payload;
		const uint8_t *output = workload_region->payload +
			workload_region->work_bytes;

		for (uint64_t index = 0; index < workload_region->work_bytes;
		     index++) {
			if (output[index] !=
			    (uint8_t)(input[index] ^ 0x5a)) {
				fprintf(stderr,
					"user region output validation failed\n");
				ret = -1;
				break;
			}
		}
	}
out_close:
	if (region_mapped) {
		if (munmap(workload_region, region_create.size_bytes) < 0) {
			perror("unmap manager workload region");
			ret = -1;
		}
		region_mapped = false;
	}
	if (region_created) {
		struct cpu_accel_region_info region_info = {
			.region_id = region_create.region_id,
		};
		struct cpu_accel_region_ref region_ref = {
			.region_id = region_create.region_id,
		};

		if (cpu_accel_region_get_info(&handle, &region_info) == 0)
			region_ref.epoch = region_info.epoch;
		if (cpu_accel_region_destroy(&handle, &region_ref) < 0) {
			perror("destroy workload region");
			ret = -1;
		}
	}
	cpu_accel_close(&handle);
	return ret < 0 ? 1 : 0;
}

static int run_workload(const char *program, int argc, char **argv)
{
	struct cpu_accel_config config = {
		.cpu = 1,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = CPU_ACCEL_DEFAULT_DURATION_NS,
		.period_ns = CPU_ACCEL_DEFAULT_PERIOD_NS,
	};
	struct sigaction action = {
		.sa_handler = handle_signal,
	};
	struct cpu_accel_handle handle;
	uint64_t value;
	uint64_t escape_after_ms = 0;
	uint64_t escape_attempts = 1;
	const char *reclaim_file = NULL;
	const char *worker_cgroup = NULL;
	unsigned int timeout_ms;
	int persistent = 0;
	int require_quiescent = 0;
	int quarantine_irqs = 0;
	int ret;

	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGTERM, &action, NULL) < 0) {
		perror("sigaction");
		return 1;
	}

	for (int index = 0; index < argc; index++) {
		if (!strcmp(argv[index], "--cpu") && index + 1 < argc) {
			if (parse_u64(argv[++index], &value) || value > UINT_MAX) {
				fprintf(stderr, "%s: invalid CPU\n", program);
				return 2;
			}
			config.cpu = value;
		} else if (!strcmp(argv[index], "--duration-ms") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &value) ||
			    value > CPU_ACCEL_MAX_DURATION_NS / 1000000ULL) {
				fprintf(stderr, "%s: invalid duration\n", program);
				return 2;
			}
			config.duration_ns = value * 1000000ULL;
		} else if (!strcmp(argv[index], "--period-us") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &value) || !value ||
			    value > UINT64_MAX / 1000ULL) {
				fprintf(stderr, "%s: invalid period\n", program);
				return 2;
			}
			config.period_ns = value * 1000ULL;
		} else if (!strcmp(argv[index], "--workload") &&
			   index + 1 < argc) {
			const char *workload = argv[++index];

			if (!strcmp(workload, "timestamp"))
				config.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP;
			else if (!strcmp(workload, "memmove"))
				config.workload = CPU_ACCEL_WORKLOAD_MEMMOVE;
			else if (!strcmp(workload, "user-region"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_REGION;
			else if (!strcmp(workload, "user-cyclictest"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_CYCLICTEST;
			else if (!strcmp(workload, "user-hang"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_HANG;
			else if (!strcmp(workload, "user-reclaim"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_RECLAIM;
			else if (!strcmp(workload, "user-death"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_DEATH;
			else if (!strcmp(workload, "user-oslat"))
				config.workload = CPU_ACCEL_WORKLOAD_USER_OSLAT;
			else {
				fprintf(stderr, "%s: invalid workload\n", program);
				return 2;
			}
		} else if (!strcmp(argv[index], "--work-bytes") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &value) ||
			    value > CPU_ACCEL_MAX_WORK_BYTES) {
				fprintf(stderr, "%s: invalid work size\n", program);
				return 2;
			}
			config.work_bytes = value;
		} else if (!strcmp(argv[index], "--test-reclaim-file") &&
			   index + 1 < argc) {
			reclaim_file = argv[++index];
			if (!*reclaim_file || !strcmp(reclaim_file, "-")) {
				fprintf(stderr, "%s: invalid reclaim file path\n", program);
				return 2;
			}
		} else if (!strcmp(argv[index], "--worker-cgroup") &&
			   index + 1 < argc) {
			worker_cgroup = argv[++index];
			if (!*worker_cgroup) {
				fprintf(stderr, "%s: invalid worker cgroup path\n", program);
				return 2;
			}
		} else if (!strcmp(argv[index], "--escape-after-ms") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &escape_after_ms) ||
			    escape_after_ms > CPU_ACCEL_MAX_DURATION_NS / 1000000ULL) {
				fprintf(stderr, "%s: invalid escape delay\n", program);
				return 2;
			}
		} else if (!strcmp(argv[index], "--escape-retries") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &escape_attempts) ||
			    !escape_attempts || escape_attempts > 16) {
				fprintf(stderr, "%s: invalid escape retry count\n", program);
				return 2;
			}
		} else if (!strcmp(argv[index], "--persistent")) {
			persistent = 1;
		} else if (!strcmp(argv[index], "--require-quiescent")) {
			require_quiescent = 1;
		} else if (!strcmp(argv[index], "--quarantine-irqs")) {
			quarantine_irqs = 1;
		} else {
			usage(stderr, program);
			return 2;
		}
	}
	if (persistent)
		config.flags |= CPU_ACCEL_FLAG_PERSISTENT;
	if (require_quiescent)
		config.flags |= CPU_ACCEL_FLAG_REQUIRE_QUIESCENT;
	if (quarantine_irqs)
		config.flags |= CPU_ACCEL_FLAG_IRQ_QUARANTINE;
	if (escape_after_ms && config.workload != CPU_ACCEL_WORKLOAD_USER_CYCLICTEST &&
	    config.workload != CPU_ACCEL_WORKLOAD_USER_HANG &&
	    config.workload != CPU_ACCEL_WORKLOAD_USER_RECLAIM &&
	    config.workload != CPU_ACCEL_WORKLOAD_USER_DEATH &&
	    config.workload != CPU_ACCEL_WORKLOAD_USER_OSLAT) {
		fprintf(stderr,
			"%s: --escape-after-ms requires a user workload\n",
			program);
		return 2;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM && !reclaim_file) {
		fprintf(stderr,
			"%s: user-reclaim requires --test-reclaim-file\n",
			program);
		return 2;
	}
	if (config.workload != CPU_ACCEL_WORKLOAD_USER_RECLAIM && reclaim_file) {
		fprintf(stderr,
			"%s: --test-reclaim-file requires user-reclaim\n", program);
		return 2;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION &&
	    !config.work_bytes) {
		fprintf(stderr, "%s: user-region requires --work-bytes\n", program);
		return 2;
	}
	if (config.workload != CPU_ACCEL_WORKLOAD_USER_REGION && config.work_bytes &&
	    config.workload != CPU_ACCEL_WORKLOAD_MEMMOVE) {
		fprintf(stderr, "%s: --work-bytes requires memmove or user-region\n",
			program);
		return 2;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_MEMMOVE &&
	    config.work_bytes > CPU_ACCEL_MAX_MEMMOVE_WORK_BYTES) {
		fprintf(stderr, "%s: memmove work size exceeds %llu bytes\n",
			program,
			(unsigned long long)CPU_ACCEL_MAX_MEMMOVE_WORK_BYTES);
		return 2;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_REGION ||
	    config.workload == CPU_ACCEL_WORKLOAD_USER_CYCLICTEST ||
	    config.workload == CPU_ACCEL_WORKLOAD_USER_HANG ||
	    config.workload == CPU_ACCEL_WORKLOAD_USER_RECLAIM ||
	    config.workload == CPU_ACCEL_WORKLOAD_USER_DEATH ||
	    config.workload == CPU_ACCEL_WORKLOAD_USER_OSLAT)
		return run_user_workload(&config, escape_after_ms, escape_attempts,
					 reclaim_file, worker_cgroup);
	if (worker_cgroup) {
		fprintf(stderr, "%s: --worker-cgroup requires a user workload\n",
			program);
		return 2;
	}

	timeout_ms = (unsigned int)(config.duration_ns / 1000000ULL) + 1000;
	if (pin_control_cpu() < 0) {
		perror("pin control process to CPU 0");
		return 1;
	}
	if (cpu_accel_open(&handle) < 0) {
		perror("open /dev/cpu_accel");
		return 1;
	}
	ret = cpu_accel_configure(&handle, &config);
	if (ret < 0) {
		perror("configure");
		cpu_accel_close(&handle);
		return 1;
	}
	ret = cpu_accel_start(&handle);
	if (ret < 0) {
		perror("start");
		cpu_accel_close(&handle);
		return 1;
	}

	ret = cpu_accel_wait(&handle, timeout_ms);
	if (ret < 0 && (errno == ETIMEDOUT ||
			(errno == EINTR && interrupted))) {
		(void)cpu_accel_stop(&handle);
		ret = cpu_accel_wait(&handle, 1000);
	}
	if (cpu_accel_exit(&handle) < 0) {
		if (!ret)
			ret = -1;
		perror("exit");
	}
	if (ret < 0 && !interrupted)
		perror("wait");
	print_status((struct cpu_accel_shared *)handle.shared, NULL,
		     config.work_bytes);
	cpu_accel_close(&handle);
	return ret < 0 && !interrupted ? 1 : 0;
}

int main(int argc, char **argv)
{
	struct cpu_accel_handle handle;
	int ret = 0;

	if (argc < 2) {
		usage(stderr, argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "--cpu-accel-user-worker"))
		return run_user_worker(argc, argv);
	if (!strcmp(argv[1], "run"))
		return run_workload(argv[0], argc - 2, argv + 2);
	if (strcmp(argv[1], "exit") && strcmp(argv[1], "stop") &&
	    strcmp(argv[1], "status") &&
	    strcmp(argv[1], "reset")) {
		usage(stderr, argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "status")) {
		if (cpu_accel_open(&handle) < 0) {
			perror("open /dev/cpu_accel");
			return 1;
		}
	} else {
		if (cpu_accel_open_control(&handle) < 0) {
			perror("open /dev/cpu_accel");
			return 1;
		}
	}
	if (!strcmp(argv[1], "exit")) {
		ret = cpu_accel_exit(&handle);
		if (ret < 0)
			perror("exit");
	} else if (!strcmp(argv[1], "stop")) {
		ret = cpu_accel_stop(&handle);
		if (ret < 0)
			perror("stop");
	} else if (!strcmp(argv[1], "reset")) {
		ret = cpu_accel_reset(&handle);
		if (ret < 0)
			perror("reset");
	} else {
		print_status((struct cpu_accel_shared *)handle.shared, NULL, 0);
	}
	cpu_accel_close(&handle);
	return ret < 0 ? 1 : 0;
}
