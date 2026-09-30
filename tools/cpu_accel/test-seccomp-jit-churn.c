// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_WORKERS 16
#define MAX_DURATION_MS 30000

static int parse_count(const char *arg, unsigned int max, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &end, 10);
	if (errno || !*arg || *end || !parsed || parsed > max)
		return -1;
	*value = parsed;
	return 0;
}

static int install_filter(void)
{
	struct sock_filter instructions[] = {
		BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
			 offsetof(struct seccomp_data, nr)),
		BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	};
	struct sock_fprog program = {
		.len = sizeof(instructions) / sizeof(instructions[0]),
		.filter = instructions,
	};

	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0))
		return -1;
	return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
		     (unsigned long)&program, 0, 0);
}

static int before_deadline(const struct timespec *deadline)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -1;
	return now.tv_sec < deadline->tv_sec ||
	       (now.tv_sec == deadline->tv_sec &&
		now.tv_nsec < deadline->tv_nsec);
}

static int run_worker(unsigned int duration_ms, unsigned long *completed)
{
	struct timespec deadline;

	*completed = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &deadline))
		return -1;
	deadline.tv_sec += duration_ms / 1000;
	deadline.tv_nsec += (duration_ms % 1000) * 1000000UL;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	for (;;) {
		int remaining = before_deadline(&deadline);
		pid_t pid;
		int status;

		if (remaining < 0)
			return -1;
		if (!remaining)
			break;
		pid = fork();
		if (pid < 0)
			return -1;
		if (!pid) {
			if (install_filter())
				_exit(1);
			_exit(0);
		}
		while (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			errno = ECHILD;
			return -1;
		}
		(*completed)++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	pid_t workers[MAX_WORKERS];
	unsigned int nr_workers = 4;
	unsigned int duration_ms = 5000;
	unsigned int i, started = 0;
	int failed = 0;

	if (argc > 3 ||
	    (argc > 1 && parse_count(argv[1], MAX_WORKERS, &nr_workers)) ||
	    (argc > 2 && parse_count(argv[2], MAX_DURATION_MS,
				     &duration_ms))) {
		fprintf(stderr, "usage: %s [WORKERS [DURATION_MS]]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	for (i = 0; i < nr_workers; i++) {
		pid_t pid = fork();

		if (pid < 0) {
			perror("fork worker");
			failed = 1;
			break;
		}
		if (!pid) {
			unsigned long completed;

			if (run_worker(duration_ms, &completed)) {
				dprintf(STDERR_FILENO, "worker=%u failed\n", i);
				_exit(1);
			}
			dprintf(STDOUT_FILENO, "worker=%u filters=%lu\n", i,
				completed);
			_exit(completed ? 0 : 1);
		}
		workers[started++] = pid;
	}

	for (i = 0; i < started; i++) {
		int status;
		pid_t ret;

		do {
			ret = waitpid(workers[i], &status, 0);
		} while (ret < 0 && errno == EINTR);
		if (ret < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
			failed = 1;
	}
	if (failed) {
		fprintf(stderr, "seccomp filter churn worker failed\n");
		return EXIT_FAILURE;
	}

	printf("PASS: workers=%u duration_ms=%u\n", nr_workers, duration_ms);
	return EXIT_SUCCESS;
}
