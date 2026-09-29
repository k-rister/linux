// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHURN_PAGES 64
#define MAX_DURATION_MS 30000

static int parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !*text || *end || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 0;
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int transfer_byte(int fd, char *byte, bool write_byte)
{
	ssize_t ret;

	do {
		ret = write_byte ? write(fd, byte, 1) : read(fd, byte, 1);
	} while (ret < 0 && errno == EINTR);
	return ret == 1 ? 0 : -1;
}

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned int cpu;
	unsigned int duration_ms;
	cpu_set_t cpus;
	unsigned char *mapping;
	int child_ready[2] = { -1, -1 };
	int parent_go[2] = { -1, -1 };
	uint64_t deadline;
	unsigned long forks = 0;
	size_t i;
	int status;
	char byte = 'x';

	if (argc != 3 || parse_uint(argv[1], &cpu) ||
	    parse_uint(argv[2], &duration_ms) || !duration_ms ||
	    duration_ms > MAX_DURATION_MS || cpu >= CPU_SETSIZE || page_size <= 0) {
		fprintf(stderr, "usage: %s CPU DURATION_MS (1..%u)\n", argv[0],
			MAX_DURATION_MS);
		return EXIT_FAILURE;
	}
	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus)) {
		perror("sched_setaffinity");
		return EXIT_FAILURE;
	}
	mapping = mmap(NULL, CHURN_PAGES * (size_t)page_size,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		       -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	for (i = 0; i < CHURN_PAGES; i++)
		__atomic_store_n(&mapping[i * (size_t)page_size], i,
				 __ATOMIC_RELAXED);
	if (pipe(child_ready) || pipe(parent_go)) {
		perror("pipe");
		return EXIT_FAILURE;
	}
	deadline = monotonic_ns() + (uint64_t)duration_ms * 1000000ULL;
	while (monotonic_ns() < deadline) {
		pid_t child = fork();
		unsigned char *page;

		if (child < 0) {
			perror("fork");
			return EXIT_FAILURE;
		}
		page = &mapping[(forks % CHURN_PAGES) * (size_t)page_size];
		if (!child) {
			close(child_ready[0]);
			close(parent_go[1]);
			__atomic_fetch_add(page, 1, __ATOMIC_RELAXED);
			if (transfer_byte(child_ready[1], &byte, true) ||
			    transfer_byte(parent_go[0], &byte, false))
				_exit(EXIT_FAILURE);
			_exit(EXIT_SUCCESS);
		}
		if (transfer_byte(child_ready[0], &byte, false)) {
			perror("read child-ready pipe");
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return EXIT_FAILURE;
		}
		__atomic_fetch_add(page, 1, __ATOMIC_RELAXED);
		if (transfer_byte(parent_go[1], &byte, true)) {
			perror("write parent-go pipe");
			kill(child, SIGKILL);
			waitpid(child, NULL, 0);
			return EXIT_FAILURE;
		}
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status)) {
			fprintf(stderr, "COW child failed\n");
			return EXIT_FAILURE;
		}
		forks++;
	}
	close(child_ready[0]);
	close(child_ready[1]);
	close(parent_go[0]);
	close(parent_go[1]);
	munmap((void *)mapping, CHURN_PAGES * (size_t)page_size);
	if (!forks) {
		fprintf(stderr, "duration elapsed before a fork completed\n");
		return EXIT_FAILURE;
	}
	printf("PASS: forks=%lu cow_write_faults=%lu cpu=%u duration_ms=%u\n",
	       forks, forks * 2, cpu, duration_ms);
	return EXIT_SUCCESS;
}
