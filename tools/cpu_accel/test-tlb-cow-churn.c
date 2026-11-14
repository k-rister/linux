// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <pthread.h>
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

struct cow_peer {
	unsigned char *mapping;
	size_t page_size;
	bool stop;
};

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

static void *touch_cow_mapping(void *data)
{
	struct cow_peer *peer = data;
	size_t i;

	while (!__atomic_load_n(&peer->stop, __ATOMIC_RELAXED)) {
		for (i = 0; i < CHURN_PAGES; i++)
			(void)__atomic_load_n(&peer->mapping[i * peer->page_size],
					      __ATOMIC_RELAXED);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned int cpu;
	unsigned int duration_ms;
	unsigned int peer_cpu = 0;
	cpu_set_t cpus;
	unsigned char *mapping;
	struct cow_peer peer = { 0 };
	pthread_t peer_thread;
	bool use_peer = argc == 4;
	bool peer_started = false;
	int child_ready[2] = { -1, -1 };
	int parent_go[2] = { -1, -1 };
	uint64_t deadline;
	unsigned long forks = 0;
	size_t i;
	int status;
	char byte = 'x';

	if ((argc != 3 && argc != 4) || parse_uint(argv[1], &cpu) ||
	    parse_uint(argv[2], &duration_ms) || !duration_ms ||
	    (use_peer && parse_uint(argv[3], &peer_cpu)) ||
	    duration_ms > MAX_DURATION_MS || cpu >= CPU_SETSIZE ||
	    (use_peer && (peer_cpu >= CPU_SETSIZE || peer_cpu == cpu)) ||
	    page_size <= 0) {
		fprintf(stderr, "usage: %s CPU DURATION_MS [PEER_CPU] (1..%u)\n", argv[0],
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
	if (use_peer) {
		pthread_attr_t attr;
		int ret;

		CPU_ZERO(&cpus);
		CPU_SET(peer_cpu, &cpus);
		ret = pthread_attr_init(&attr);
		if (ret) {
			errno = ret;
			perror("pthread_attr_init");
			return EXIT_FAILURE;
		}
		ret = pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
		if (ret) {
			pthread_attr_destroy(&attr);
			errno = ret;
			perror("pthread_attr_setaffinity_np");
			return EXIT_FAILURE;
		}
		peer.mapping = mapping;
		peer.page_size = (size_t)page_size;
		ret = pthread_create(&peer_thread, &attr, touch_cow_mapping, &peer);
		pthread_attr_destroy(&attr);
		if (ret) {
			errno = ret;
			perror("pthread_create");
			return EXIT_FAILURE;
		}
		peer_started = true;
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
	if (peer_started) {
		int ret;

		__atomic_store_n(&peer.stop, true, __ATOMIC_RELAXED);
		ret = pthread_join(peer_thread, NULL);
		if (ret) {
			errno = ret;
			perror("pthread_join");
			return EXIT_FAILURE;
		}
	}
	munmap((void *)mapping, CHURN_PAGES * (size_t)page_size);
	if (!forks) {
		fprintf(stderr, "duration elapsed before a fork completed\n");
		return EXIT_FAILURE;
	}
	printf("PASS: forks=%lu cow_write_faults=%lu cpu=%u",
	       forks, forks * 2, cpu);
	if (use_peer)
		printf(" peer_cpu=%u", peer_cpu);
	printf(" duration_ms=%u\n", duration_ms);
	return EXIT_SUCCESS;
}
