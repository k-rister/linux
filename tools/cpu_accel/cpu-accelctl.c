// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
}

struct cpu_accel_user_context {
	struct cpu_accel_shared *shared;
	int fd;
	uint64_t cycles_per_ns;
};

static inline uint64_t cpu_accel_read_tsc(void)
{
	uint32_t low;
	uint32_t high;

	__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
	return ((uint64_t)high << 32) | low;
}

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

/* This function is the complete first ring-3 accelerator image. */
#define CPU_ACCEL_USER_IMAGE __attribute__((aligned(4096)))

CPU_ACCEL_USER_IMAGE
static void cpu_accel_user_oslat(void *argument)
{
	struct cpu_accel_user_context *context = argument;
	struct cpu_accel_shared *shared = context->shared;
	uint64_t cycles_per_ns = context->cycles_per_ns;
	uint64_t start_tsc = cpu_accel_read_tsc();
	uint64_t period_ns = __atomic_load_n(&shared->period_ns,
						    __ATOMIC_RELAXED);
	uint64_t duration_ns = __atomic_load_n(&shared->duration_ns,
						     __ATOMIC_RELAXED);
	uint64_t period_cycles = period_ns * cycles_per_ns;
	uint64_t duration_cycles = duration_ns * cycles_per_ns;
	uint64_t deadline = start_tsc + period_cycles;
	uint64_t samples = 0;
	uint64_t max_lateness = 0;
	uint64_t min_lateness = UINT64_MAX;
	uint64_t last_lateness = 0;
	uint32_t samples_valid = 0;

	if (!cycles_per_ns)
		cycles_per_ns = 1;
	if (!period_cycles)
		period_cycles = 1;
	if (!duration_cycles)
		duration_cycles = period_cycles;

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
		lateness_ns = lateness_cycles / cycles_per_ns;
		timestamp_ns = __atomic_load_n(&shared->start_ns,
					       __ATOMIC_RELAXED) +
			(now - start_tsc) / cycles_per_ns;
		if (samples_valid < CPU_ACCEL_MAX_SAMPLES) {
			shared->samples[samples_valid].timestamp_ns = timestamp_ns;
			shared->samples[samples_valid].lateness_ns = lateness_ns;
			samples_valid++;
		}
		samples++;
		if (lateness_ns > max_lateness)
			max_lateness = lateness_ns;
		if (lateness_ns < min_lateness)
			min_lateness = lateness_ns;
		last_lateness = lateness_ns;
		if (__atomic_load_n(&shared->stop_requested, __ATOMIC_RELAXED) ||
		    now - start_tsc >= duration_cycles)
			break;
		if (deadline > UINT64_MAX - period_cycles)
			break;
		deadline += period_cycles;
	}

	__atomic_store_n(&shared->samples_produced, samples, __ATOMIC_RELAXED);
	__atomic_store_n(&shared->samples_valid, samples_valid, __ATOMIC_RELAXED);
	__atomic_store_n(&shared->max_lateness_ns, max_lateness,
				__ATOMIC_RELAXED);
	__atomic_store_n(&shared->min_lateness_ns,
				samples ? min_lateness : 0, __ATOMIC_RELAXED);
	__atomic_store_n(&shared->last_lateness_ns, last_lateness,
				__ATOMIC_RELAXED);
	__atomic_store_n(&shared->work_iterations, samples, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
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
	if (!elapsed_ns || cycles / elapsed_ns == 0)
		return 1;
	return cycles / elapsed_ns;
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

static void print_status(const volatile struct cpu_accel_shared *shared)
{
	printf("state=%u mode=%u sequence=%" PRIu64 " cpu=%u backend=%u samples=%" PRIu64
	       " max_lateness_ns=%" PRIu64 " min_lateness_ns=%" PRIu64
	       " last_lateness_ns=%" PRIu64 " duration_ns=%" PRIu64
	       " period_ns=%" PRIu64 " workload=%u work_bytes=%" PRIu64
	       " work_iterations=%" PRIu64 " shared_entry=%u shared_owner=%u shared_epoch=%" PRIu64
	       " lifecycle_entry_ns=%" PRIu64 " lifecycle_exit_ns=%" PRIu64
	       " irq_count=%" PRIu64
	       " irq_quarantined=%u irq_quarantine_blockers=%u"
	       " softirq_count=%" PRIu64 " timer_softirq_count=%" PRIu64
	       " hrtimer_softirq_count=%" PRIu64 " rcu_softirq_count=%" PRIu64
	       " sched_softirq_count=%" PRIu64 " workqueue_queued=%" PRIu64
	       " workqueue_executed=%" PRIu64 " context_switches=%" PRIu64
	       " need_resched_samples=%" PRIu64 " arch_irq_count=%" PRIu64
	       " arch_ipi_count=%" PRIu64 " arch_tlb_count=%" PRIu64
	       " need_resched_entry=%u need_resched_exit=%u"
	       " softirq_pending_entry=%u softirq_pending_exit=%u"
	       " preempt_count_entry=%u preempt_count_exit=%u"
	       " arch_counters_valid=%u lifecycle_cpu_entry=%u"
	       " lifecycle_cpu_exit=%u migration_detected=%u\n",
	       shared->state, shared->mode, (uint64_t)shared->sequence,
	       shared->cpu,
	       shared->backend,
	       (uint64_t)shared->samples_produced,
	       (uint64_t)shared->max_lateness_ns,
	       (uint64_t)shared->min_lateness_ns,
	       (uint64_t)shared->last_lateness_ns,
	       (uint64_t)shared->duration_ns, (uint64_t)shared->period_ns,
	       shared->workload, (uint64_t)shared->work_bytes,
	       (uint64_t)shared->work_iterations,
	       shared->shared_entry, shared->shared_owner,
	       (uint64_t)shared->shared_epoch,
	       (uint64_t)shared->lifecycle_entry_ns,
	       (uint64_t)shared->lifecycle_exit_ns,
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
	       shared->need_resched_entry, shared->need_resched_exit,
	       shared->softirq_pending_entry, shared->softirq_pending_exit,
	       shared->preempt_count_entry, shared->preempt_count_exit,
	       shared->arch_counters_valid,
	       shared->lifecycle_cpu_entry, shared->lifecycle_cpu_exit,
	       shared->migration_detected);
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage:\n"
		"  %s run [--cpu N] [--duration-ms N] [--period-us N]\n"
		"      [--workload timestamp|memmove|shared-memmove|user-oslat]\n"
		"      [--work-bytes N] [--shared-entry N]\n"
		"      [--persistent] [--require-quiescent] [--quarantine-irqs]\n"
		"  %s exit\n"
		"  %s status\n"
		"  %s reset\n",
		program, program, program, program);
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

static int run_user_oslat(const struct cpu_accel_config *requested)
{
	struct cpu_accel_config config = *requested;
	struct cpu_accel_handle handle;
	struct cpu_accel_user_context *context;
	void *stack;
	long page_size;
	pid_t child;
	int status;
	int ret;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0 || (size_t)page_size > SIZE_MAX / 16) {
		fprintf(stderr, "invalid page size\n");
		return 1;
	}
	stack = mmap(NULL, (size_t)page_size * 16,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (stack == MAP_FAILED) {
		perror("mmap user accelerator stack");
		return 1;
	}
	if (cpu_accel_open(&handle) < 0) {
		perror("open /dev/cpu_accel");
		munmap(stack, (size_t)page_size * 16);
		return 1;
	}

	context = stack;
	context->shared = (struct cpu_accel_shared *)(uintptr_t)handle.shared;
	context->fd = handle.fd;
	context->cycles_per_ns = cpu_accel_calibrate_cycles_per_ns();
	config.flags |= CPU_ACCEL_FLAG_PERSISTENT;
	config.user_entry_ip = (uintptr_t)cpu_accel_user_oslat;
	config.user_stack_top = (uintptr_t)stack + (size_t)page_size * 16;
	config.user_stack_bytes = (size_t)page_size * 16;
	config.user_image_start = (uintptr_t)cpu_accel_user_oslat &
		~((uintptr_t)page_size - 1);
	config.user_image_bytes = page_size;
	config.user_arg = (uintptr_t)context;

	child = fork();
	if (child < 0) {
		perror("fork user accelerator");
		cpu_accel_close(&handle);
		munmap(stack, (size_t)page_size * 16);
		return 1;
	}
	if (!child) {
		if (pin_cpu(config.cpu) < 0) {
			perror("pin user accelerator");
			_exit(1);
		}
		ret = cpu_accel_configure(&handle, &config);
		if (ret < 0) {
			perror("configure user accelerator");
			_exit(1);
		}
		ret = cpu_accel_start(&handle);
		if (ret < 0) {
			perror("start user accelerator");
			_exit(1);
		}
		cpu_accel_close(&handle);
		munmap(stack, (size_t)page_size * 16);
		_exit(0);
	}

	if (waitpid(child, &status, 0) < 0) {
		perror("wait for user accelerator");
		ret = -1;
	} else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "user accelerator child failed\n");
		ret = -1;
	} else {
		ret = 0;
	}
	print_status(handle.shared);
	if (handle.shared->state != CPU_ACCEL_STATE_COMPLETE ||
	    handle.shared->mode != CPU_ACCEL_MODE_LINUX ||
	    handle.shared->backend != CPU_ACCEL_BACKEND_X86_RING3 ||
	    !handle.shared->samples_valid) {
		fprintf(stderr, "user accelerator did not complete its ring-3 contract\n");
		ret = -1;
	}
	if (cpu_accel_exit(&handle) < 0) {
		perror("exit user accelerator");
		ret = -1;
	}
	cpu_accel_close(&handle);
	munmap(stack, (size_t)page_size * 16);
	return ret < 0 ? 1 : 0;
}

static int run_workload(const char *program, int argc, char **argv)
{
	struct cpu_accel_config config = {
		.cpu = 1,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.shared_entry = 0,
		.duration_ns = CPU_ACCEL_DEFAULT_DURATION_NS,
		.period_ns = CPU_ACCEL_DEFAULT_PERIOD_NS,
	};
	struct sigaction action = {
		.sa_handler = handle_signal,
	};
	struct cpu_accel_handle handle;
	uint64_t value;
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
			else if (!strcmp(workload, "shared-memmove"))
				config.workload = CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE;
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
		} else if (!strcmp(argv[index], "--shared-entry") &&
			   index + 1 < argc) {
			if (parse_u64(argv[++index], &value) || value > UINT_MAX) {
				fprintf(stderr, "%s: invalid shared entry\n", program);
				return 2;
			}
			config.shared_entry = value;
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
	if (config.workload == CPU_ACCEL_WORKLOAD_USER_OSLAT)
		return run_user_oslat(&config);

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
	if (config.workload == CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE) {
		struct cpu_accel_shared_entry *entry =
			&handle.shared_region->entries[config.shared_entry];
		void *data = (void *)entry->data;

		memset(data, 0xa5, config.work_bytes);
		memset((char *)data + config.work_bytes, 0x5a,
		       config.work_bytes);
		if (cpu_accel_shared_ready(&handle, config.shared_entry,
					   config.work_bytes) < 0) {
			perror("shared ready");
			cpu_accel_close(&handle);
			return 1;
		}
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
	print_status(handle.shared);
	if (config.workload == CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE &&
	    handle.shared->work_iterations &&
	    memcmp(handle.shared_region->entries[config.shared_entry].data,
		   handle.shared_region->entries[config.shared_entry].data +
		   config.work_bytes, config.work_bytes)) {
		fprintf(stderr, "shared memmove data validation failed\n");
		ret = -1;
	}
	if (config.workload == CPU_ACCEL_WORKLOAD_SHARED_MEMMOVE &&
	    cpu_accel_shared_reclaim(&handle, config.shared_entry) < 0) {
		perror("shared reclaim");
		ret = -1;
	}
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
	if (!strcmp(argv[1], "run"))
		return run_workload(argv[0], argc - 2, argv + 2);
	if (strcmp(argv[1], "exit") && strcmp(argv[1], "status") &&
	    strcmp(argv[1], "reset")) {
		usage(stderr, argv[0]);
		return 2;
	}

	if (cpu_accel_open(&handle) < 0) {
		perror("open /dev/cpu_accel");
		return 1;
	}
	if (!strcmp(argv[1], "exit")) {
		ret = cpu_accel_exit(&handle);
		if (ret < 0)
			perror("exit");
	} else if (!strcmp(argv[1], "reset")) {
		ret = cpu_accel_reset(&handle);
		if (ret < 0)
			perror("reset");
	} else {
		print_status(handle.shared);
	}
	cpu_accel_close(&handle);
	return ret < 0 ? 1 : 0;
}
