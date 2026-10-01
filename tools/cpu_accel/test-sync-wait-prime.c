// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_PAGES 64

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

int main(int argc, char **argv)
{
	sigset_t signals;
	long page_size = sysconf(_SC_PAGESIZE);
	volatile unsigned char *mapping;
	unsigned int cpu, pages, i;
	int signal, rc;
	cpu_set_t cpus;
	size_t length;

	if (argc != 3 || parse_uint(argv[1], &cpu) ||
	    parse_uint(argv[2], &pages) || !pages || pages > MAX_PAGES ||
	    cpu >= CPU_SETSIZE || page_size <= 0) {
		fprintf(stderr, "usage: %s CPU PAGES (1..%u)\n", argv[0],
			MAX_PAGES);
		return EXIT_FAILURE;
	}

	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus)) {
		perror("sched_setaffinity");
		return EXIT_FAILURE;
	}

	if (sigemptyset(&signals) || sigaddset(&signals, SIGINT) ||
	    sigaddset(&signals, SIGTERM) ||
	    sigprocmask(SIG_BLOCK, &signals, NULL)) {
		perror("signal mask");
		return EXIT_FAILURE;
	}

	length = pages * (size_t)page_size;
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}

	for (i = 0; i < pages; i++)
		mapping[i * (size_t)page_size] = (unsigned char)i + 1;

	printf("ready cpu=%u pages=%u\n", cpu, pages);
	fflush(stdout);
	rc = sigwait(&signals, &signal);
	if (rc) {
		errno = rc;
		perror("sigwait");
		return EXIT_FAILURE;
	}

	if (munmap((void *)mapping, length)) {
		perror("munmap");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
