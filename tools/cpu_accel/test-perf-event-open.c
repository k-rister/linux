// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <limits.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long parse_number(const char *arg, const char *name, long min, long max)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(arg, &end, 10);
	if (errno || !*arg || *end || value < min || value > max) {
		fprintf(stderr, "invalid %s: %s\n", name, arg);
		exit(2);
	}

	return value;
}

int main(int argc, char **argv)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_HW_CPU_CYCLES,
		.exclude_kernel = 1,
		.exclude_hv = 1,
	};
	struct timespec delay;
	unsigned long long cycles;
	long cpu, duration_ms;
	ssize_t bytes;
	int fd;

	if (argc != 3) {
		fprintf(stderr, "usage: %s CPU DURATION_MS\n", argv[0]);
		return 2;
	}

	cpu = parse_number(argv[1], "CPU", 0, INT_MAX);
	duration_ms = parse_number(argv[2], "duration", 1, 60000);
	fd = syscall(__NR_perf_event_open, &attr, -1, cpu, -1,
		     PERF_FLAG_FD_CLOEXEC);
	if (fd < 0) {
		perror("perf_event_open");
		return 1;
	}

	delay.tv_sec = duration_ms / 1000;
	delay.tv_nsec = (duration_ms % 1000) * 1000000L;
	while (nanosleep(&delay, &delay) && errno == EINTR)
		;

	bytes = read(fd, &cycles, sizeof(cycles));
	if (bytes != sizeof(cycles)) {
		if (bytes < 0)
			perror("read perf event");
		else
			fprintf(stderr, "short perf event read: %zd bytes\n", bytes);
		close(fd);
		return 1;
	}
	if (close(fd)) {
		perror("close perf event");
		return 1;
	}

	printf("cpu=%ld cycles=%llu\n", cpu, cycles);
	return 0;
}
