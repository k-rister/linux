// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/cpu_accel.h>

static void sleep_ms(unsigned int milliseconds)
{
	struct timespec request = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000L,
	};

	while (nanosleep(&request, &request) && errno == EINTR)
		;
}

int main(int argc, char **argv)
{
	unsigned long delay_ms = 5000;
	char *end;
	int fd;

	if (argc == 2) {
		errno = 0;
		delay_ms = strtoul(argv[1], &end, 10);
		if (errno || *end || delay_ms > 30000) {
			fprintf(stderr, "invalid rescue delay\n");
			return 2;
		}
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [delay-ms]\n", argv[0]);
		return 2;
	}
	sleep_ms(delay_ms);

	fd = open("/dev/cpu_accel", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/cpu_accel for rescue");
		return 1;
	}
	for (unsigned int attempt = 1; attempt <= 3; attempt++) {
		if (!ioctl(fd, CPU_ACCEL_IOC_USER_ESCAPE, 0)) {
			printf("rescue_escape_attempt=%u result=sent\n", attempt);
			close(fd);
			return 0;
		}
		if (errno == EINVAL || errno == EALREADY) {
			printf("rescue_escape_attempt=%u result=owner-already-stopped\n",
			       attempt);
			close(fd);
			return 0;
		}
		fprintf(stderr, "rescue_escape_attempt=%u: %s\n", attempt,
			strerror(errno));
		if (attempt < 3)
			sleep_ms(100);
	}
	close(fd);
	return 1;
}
