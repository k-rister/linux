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
	int ret;
	int saved_errno;

	fd = open(CPU_ACCEL_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = cpu_accel_attach_fd(handle, fd);
	if (!ret)
		return 0;
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

int cpu_accel_attach_fd(struct cpu_accel_handle *handle, int fd)
{
	void *mapping;
	void *shared_mapping;
	int saved_errno;

	memset(handle, 0, sizeof(*handle));
	handle->fd = -1;
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}

	mapping = mmap(NULL, CPU_ACCEL_MAP_SIZE, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return -1;
	shared_mapping = mmap(NULL, CPU_ACCEL_SHARED_MAP_SIZE,
			      PROT_READ | PROT_WRITE, MAP_SHARED, fd,
			      CPU_ACCEL_SHARED_MAP_OFFSET);
	if (shared_mapping == MAP_FAILED) {
		saved_errno = errno;
		munmap(mapping, CPU_ACCEL_MAP_SIZE);
		errno = saved_errno;
		return -1;
	}

	handle->fd = fd;
	handle->shared = mapping;
	handle->shared_region = shared_mapping;
	if (handle->shared->abi_version != CPU_ACCEL_ABI_VERSION ||
	    handle->shared->struct_size < sizeof(struct cpu_accel_shared) ||
	    handle->shared_region->abi_version != CPU_ACCEL_ABI_VERSION ||
	    handle->shared_region->struct_size <
		    sizeof(struct cpu_accel_shared_region) ||
	    handle->shared_region->entry_count != CPU_ACCEL_SHARED_ENTRY_COUNT ||
		    handle->shared_region->entry_size !=
			    sizeof(struct cpu_accel_shared_entry)) {
		munmap((void *)handle->shared, CPU_ACCEL_MAP_SIZE);
		munmap((void *)handle->shared_region,
		       CPU_ACCEL_SHARED_MAP_SIZE);
		memset(handle, 0, sizeof(*handle));
		handle->fd = -1;
		errno = EPROTO;
		return -1;
	}

	return 0;
}

void cpu_accel_close(struct cpu_accel_handle *handle)
{
	if (handle->shared && handle->shared != MAP_FAILED)
		munmap((void *)handle->shared, CPU_ACCEL_MAP_SIZE);
	if (handle->shared_region && handle->shared_region != MAP_FAILED)
		munmap((void *)handle->shared_region, CPU_ACCEL_SHARED_MAP_SIZE);
	if (handle->fd >= 0)
		close(handle->fd);
	handle->shared = NULL;
	handle->shared_region = NULL;
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

int cpu_accel_user_escape(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_USER_ESCAPE, NULL);
}

int cpu_accel_reset(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_RESET, NULL);
}

int cpu_accel_shared_ready(struct cpu_accel_handle *handle,
				   uint32_t entry, uint64_t bytes)
{
	struct cpu_accel_shared_handoff handoff = {
		.entry = entry,
		.bytes = bytes,
	};

	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_SHARED_READY,
			       &handoff);
}

int cpu_accel_shared_reclaim(struct cpu_accel_handle *handle,
				     uint32_t entry)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_SHARED_RECLAIM,
			       &entry);
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
		case CPU_ACCEL_STATE_WATCHDOG:
		case CPU_ACCEL_STATE_ESCAPED:
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
