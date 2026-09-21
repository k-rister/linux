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

static volatile sig_atomic_t interrupted;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	interrupted = 1;
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
	       " period_ns=%" PRIu64 " lifecycle_entry_ns=%" PRIu64
	       " lifecycle_exit_ns=%" PRIu64 " irq_count=%" PRIu64
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
	       (uint64_t)shared->lifecycle_entry_ns,
	       (uint64_t)shared->lifecycle_exit_ns,
	       (uint64_t)shared->irq_count,
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
		"  %s run [--cpu N] [--duration-ms N] [--period-us N]"
		" [--persistent] [--require-quiescent]\n"
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

static int run_workload(const char *program, int argc, char **argv)
{
	struct cpu_accel_config config = {
		.cpu = 1,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
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
		} else if (!strcmp(argv[index], "--persistent")) {
			persistent = 1;
		} else if (!strcmp(argv[index], "--require-quiescent")) {
			require_quiescent = 1;
		} else {
			usage(stderr, program);
			return 2;
		}
	}
	if (persistent)
		config.flags |= CPU_ACCEL_FLAG_PERSISTENT;
	if (require_quiescent)
		config.flags |= CPU_ACCEL_FLAG_REQUIRE_QUIESCENT;

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
	print_status(handle.shared);
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
