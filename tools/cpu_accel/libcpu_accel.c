// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CPU_ACCEL_DEVICE "/dev/cpu_accel"

static int cpu_accel_ioctl(int fd, unsigned long command, void *argument)
{
	int ret;

	do {
		ret = ioctl(fd, command, argument);
	} while (ret < 0 && errno == EINTR);

	return ret;
}

int cpu_accel_open(struct cpu_accel_handle *handle)
{
	int fd;
	void *mapping;

	memset(handle, 0, sizeof(*handle));
	handle->fd = -1;
	fd = open(CPU_ACCEL_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	mapping = mmap(NULL, CPU_ACCEL_MAP_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}

	handle->fd = fd;
	handle->shared = mapping;
	if (handle->shared->abi_version != CPU_ACCEL_ABI_VERSION ||
	    handle->shared->struct_size < sizeof(struct cpu_accel_shared)) {
		cpu_accel_close(handle);
		errno = EPROTO;
		return -1;
	}

	return 0;
}

void cpu_accel_close(struct cpu_accel_handle *handle)
{
	if (handle->shared && handle->shared != MAP_FAILED)
		munmap((void *)handle->shared, CPU_ACCEL_MAP_SIZE);
	if (handle->fd >= 0)
		close(handle->fd);
	handle->shared = NULL;
	handle->fd = -1;
}

int cpu_accel_configure(struct cpu_accel_handle *handle,
			const struct cpu_accel_config *config)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_CONFIG,
			       (void *)config);
}

int cpu_accel_start(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_START, NULL);
}

int cpu_accel_stop(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_STOP, NULL);
}

int cpu_accel_exit(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_EXIT, NULL);
}

int cpu_accel_reset(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_RESET, NULL);
}

static uint64_t cpu_accel_now_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

int cpu_accel_wait(struct cpu_accel_handle *handle, unsigned int timeout_ms)
{
	uint64_t deadline = cpu_accel_now_ns() +
		(uint64_t)timeout_ms * 1000000ULL;
	struct timespec sleep_for = {
		.tv_sec = 0,
		.tv_nsec = 1000000,
	};

	for (;;) {
		switch (handle->shared->state) {
		case CPU_ACCEL_STATE_COMPLETE:
		case CPU_ACCEL_STATE_STOPPED:
			return 0;
		case CPU_ACCEL_STATE_ERROR:
			errno = EIO;
			return -1;
		default:
			break;
		}

		if (cpu_accel_now_ns() >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		if (nanosleep(&sleep_for, NULL) < 0 && errno == EINTR) {
			errno = EINTR;
			return -1;
		}
	}
}
