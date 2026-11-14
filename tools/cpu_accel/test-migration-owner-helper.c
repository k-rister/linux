// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define MAX_DURATION_MS 60000

struct reader_state {
	unsigned char *page;
	unsigned long reads;
	bool stop;
};

static volatile unsigned long reader_sink;

static int parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !*text || !end || *end || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static int parse_int(const char *text, int *value)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || !*text || !end || *end || parsed < INT_MIN || parsed > INT_MAX)
		return -1;
	*value = (int)parsed;
	return 0;
}

static uint64_t monotonic_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * 1000U +
	       (uint64_t)now.tv_nsec / 1000000U;
}

static int pin_thread(pthread_t thread, unsigned int cpu)
{
	cpu_set_t cpus;

	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);
	return pthread_setaffinity_np(thread, sizeof(cpus), &cpus);
}

static void *read_page(void *arg)
{
	struct reader_state *state = arg;

	while (!__atomic_load_n(&state->stop, __ATOMIC_RELAXED)) {
		reader_sink += __atomic_load_n(&state->page[0],
					       __ATOMIC_RELAXED);
		__atomic_fetch_add(&state->reads, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	unsigned int work_cpu;
	unsigned int reader_cpu;
	unsigned int duration_ms;
	int node1;
	int node2;
	int current_node = -1;
	int target_node = -1;
	int status;
	long syscall_ret;
	long page_size;
	void *pages[1];
	unsigned char *page;
	pthread_t reader_thread;
	struct reader_state reader = { 0 };
	uint64_t deadline;
	uint64_t start_ms;
	unsigned long attempts = 0;
	unsigned long migrations = 0;
	unsigned long moves_to_node1 = 0;
	unsigned long moves_to_node2 = 0;
	unsigned long failures = 0;
	bool reader_started = false;
	bool join_failed = false;
	int pthread_ret;

	if (argc != 6 || parse_uint(argv[1], &work_cpu) ||
	    parse_uint(argv[2], &reader_cpu) ||
	    parse_int(argv[3], &node1) || parse_int(argv[4], &node2) ||
	    parse_uint(argv[5], &duration_ms) || !duration_ms ||
	    duration_ms > MAX_DURATION_MS || work_cpu >= CPU_SETSIZE ||
	    reader_cpu >= CPU_SETSIZE || work_cpu == reader_cpu ||
	    node1 < 0 || node2 < 0 || node1 == node2) {
		fprintf(stderr,
			"usage: %s WORK_CPU READER_CPU NODE1 NODE2 DURATION_MS (1..%u)\n",
			argv[0], MAX_DURATION_MS);
		return EXIT_FAILURE;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		perror("sysconf(_SC_PAGESIZE)");
		return EXIT_FAILURE;
	}

	if (pin_thread(pthread_self(), work_cpu)) {
		perror("pthread_setaffinity_np(work)");
		return EXIT_FAILURE;
	}

	page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	memset(page, 0xa5, (size_t)page_size);
	pages[0] = page;
	reader.page = page;

	pthread_ret = pthread_create(&reader_thread, NULL, read_page, &reader);
	if (pthread_ret) {
		errno = pthread_ret;
		perror("pthread_create");
		munmap(page, (size_t)page_size);
		return EXIT_FAILURE;
	}
	reader_started = true;
	pthread_ret = pin_thread(reader_thread, reader_cpu);
	if (pthread_ret) {
		errno = pthread_ret;
		perror("pthread_setaffinity_np(reader)");
		goto out;
	}

	status = -1;
	syscall_ret = syscall(SYS_move_pages, 0, 1, pages, NULL, &status, 0);
	if (syscall_ret < 0) {
		perror("move_pages(query)");
		goto out;
	}
	if (status < 0) {
		fprintf(stderr, "move_pages(query) returned status %d\n", status);
		goto out;
	}
	current_node = status;
	target_node = current_node == node1 ? node2 : node1;

	start_ms = monotonic_ms();
	if (!start_ms) {
		fprintf(stderr, "clock_gettime returned an invalid time\n");
		goto out;
	}
	deadline = start_ms + duration_ms;

	while (monotonic_ms() < deadline) {
		status = -1;
		attempts++;
		syscall_ret = syscall(SYS_move_pages, 0, 1, pages,
				      &target_node, &status,
				      MPOL_MF_MOVE_ALL);
		if (syscall_ret == 0 && status == target_node) {
			migrations++;
			if (target_node == node1)
				moves_to_node1++;
			else
				moves_to_node2++;
			current_node = target_node;
			target_node = current_node == node1 ? node2 : node1;
		} else {
			failures++;
		}
	}

out:
	if (reader_started) {
		__atomic_store_n(&reader.stop, true, __ATOMIC_RELAXED);
		pthread_ret = pthread_join(reader_thread, NULL);
		if (pthread_ret) {
			errno = pthread_ret;
			perror("pthread_join");
			join_failed = true;
		}
	}
	munmap(page, (size_t)page_size);

	if (join_failed || migrations < 2 || moves_to_node1 == 0 ||
	    moves_to_node2 == 0 ||
	    reader.reads == 0) {
		fprintf(stderr,
			"FAIL: migrations=%lu moves_to_node%d=%lu moves_to_node%d=%lu reads=%lu attempts=%lu failures=%lu\n",
			migrations, node1, moves_to_node1, node2,
			moves_to_node2, reader.reads, attempts, failures);
		return EXIT_FAILURE;
	}

	printf("PASS: migrations=%lu moves_to_node%d=%lu moves_to_node%d=%lu reads=%lu attempts=%lu failures=%lu work_cpu=%u reader_cpu=%u node1=%d node2=%d final_node=%d duration_ms=%u\n",
	       migrations, node1, moves_to_node1, node2, moves_to_node2,
	       reader.reads, attempts, failures, work_cpu, reader_cpu, node1,
	       node2, current_node, duration_ms);
	return EXIT_SUCCESS;
}
