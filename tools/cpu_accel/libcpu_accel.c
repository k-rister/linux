// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
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
	struct cpu_accel_abi_info info;
	struct cpu_accel_abi_select select;
	const char *requested_env = getenv("CPU_ACCEL_ABI_VERSION");
	unsigned long requested_abi = CPU_ACCEL_ABI_VERSION;
	bool explicit_abi = false;
	bool have_abi_info = false;
	char *end = NULL;

	fd = open(CPU_ACCEL_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (requested_env && *requested_env) {
		explicit_abi = true;
		errno = 0;
		requested_abi = strtoul(requested_env, &end, 10);
		if (errno || !end || *end ||
		    requested_abi < CPU_ACCEL_ABI_MIN_VERSION ||
		    requested_abi > CPU_ACCEL_ABI_VERSION) {
			close(fd);
			errno = EINVAL;
			return -1;
		}
	}
	if (ioctl(fd, CPU_ACCEL_IOC_GET_ABI_INFO, &info) == 0) {
		have_abi_info = true;
		if (requested_abi < info.min_abi ||
		    requested_abi > info.max_abi) {
			if (!explicit_abi &&
			    requested_abi == CPU_ACCEL_ABI_VERSION &&
			    info.max_abi < requested_abi) {
				requested_abi = info.max_abi;
			} else {
				close(fd);
				errno = ENOTSUP;
				return -1;
			}
		}
	} else if (!explicit_abi && errno == ENOTTY) {
		requested_abi = CPU_ACCEL_ABI_MIN_VERSION;
	} else {
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (have_abi_info) {
		select = (struct cpu_accel_abi_select){
			.requested_abi = requested_abi,
		};
		if (ioctl(fd, CPU_ACCEL_IOC_SET_ABI_VERSION, &select) < 0) {
			saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return -1;
		}
		if (select.selected_abi != requested_abi) {
			close(fd);
			errno = EPROTO;
			return -1;
		}
	}
	ret = cpu_accel_attach_fd(handle, fd);
	if (!ret)
		return 0;
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

int cpu_accel_open_control(struct cpu_accel_handle *handle)
{
	int fd;

	fd = open(CPU_ACCEL_DEVICE, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	memset(handle, 0, sizeof(*handle));
	handle->fd = fd;
	handle->shared = NULL;
	return 0;
}

int cpu_accel_attach_fd(struct cpu_accel_handle *handle, int fd)
{
	void *mapping;

	memset(handle, 0, sizeof(*handle));
	handle->fd = -1;
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}

	mapping = mmap(NULL, CPU_ACCEL_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		return -1;

	handle->fd = fd;
	handle->shared = mapping;
	if ((handle->shared->abi_version != CPU_ACCEL_ABI_VERSION &&
	     handle->shared->abi_version != CPU_ACCEL_ABI_MIN_VERSION) ||
	    handle->shared->struct_size != sizeof(struct cpu_accel_shared) ||
	    handle->shared->layout_fingerprint !=
		    CPU_ACCEL_LAYOUT_FINGERPRINT()) {
		munmap((void *)handle->shared, CPU_ACCEL_MAP_SIZE);
		memset(handle, 0, sizeof(*handle));
		handle->fd = -1;
		errno = EPROTO;
		return -1;
	}

	return 0;
}

int cpu_accel_get_abi_info(struct cpu_accel_handle *handle,
			   struct cpu_accel_abi_info *info)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_GET_ABI_INFO, info);
}

int cpu_accel_set_abi_version(struct cpu_accel_handle *handle,
			      struct cpu_accel_abi_select *select)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_SET_ABI_VERSION,
			       select);
}

int cpu_accel_get_arch_telemetry(struct cpu_accel_handle *handle,
				 struct cpu_accel_arch_telemetry *telemetry)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_GET_ARCH_TELEMETRY,
			       telemetry);
}

int cpu_accel_x86_image_admission(struct cpu_accel_handle *handle,
				  struct cpu_accel_x86_image_admission *image)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_X86_IMAGE_ADMISSION,
			       image);
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

int cpu_accel_user_escape(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_USER_ESCAPE, NULL);
}

int cpu_accel_reset(struct cpu_accel_handle *handle)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_RESET, NULL);
}

int cpu_accel_get_capabilities(struct cpu_accel_handle *handle,
			       struct cpu_accel_capabilities *capabilities)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_GET_CAPABILITIES,
			       capabilities);
}

int cpu_accel_region_create(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_create *create)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_REGION_CREATE, create);
}

int cpu_accel_region_attach(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_ref *ref)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_REGION_ATTACH, ref);
}

int cpu_accel_region_detach(struct cpu_accel_handle *handle,
			    struct cpu_accel_region_ref *ref)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_REGION_DETACH, ref);
}

int cpu_accel_region_destroy(struct cpu_accel_handle *handle,
			     struct cpu_accel_region_ref *ref)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_REGION_DESTROY, ref);
}

int cpu_accel_region_get_info(struct cpu_accel_handle *handle,
			      struct cpu_accel_region_info *info)
{
	return cpu_accel_ioctl(handle->fd, CPU_ACCEL_IOC_REGION_GET_INFO, info);
}

void *cpu_accel_region_mmap(struct cpu_accel_handle *handle,
			    const struct cpu_accel_region_create *create,
			    bool manager)
{
	int prot = PROT_READ;

	if (manager && (create->permissions &
			CPU_ACCEL_REGION_PERM_MANAGER_WRITE))
		prot |= PROT_WRITE;
	if (!manager && (create->permissions &
			 CPU_ACCEL_REGION_PERM_OWNER_WRITE))
		prot |= PROT_WRITE;

	return mmap(NULL, create->size_bytes, prot, MAP_SHARED, handle->fd,
		    create->mmap_token);
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
	struct pollfd pfd = {
		.fd = handle->fd,
		.events = POLLIN,
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
		uint64_t remaining_ns = deadline - cpu_accel_now_ns();
		int remaining_ms = (int)((remaining_ns + 999999) / 1000000);
		int poll_ret = poll(&pfd, 1, remaining_ms);

		if (poll_ret < 0 && errno == EINTR) {
			errno = EINTR;
			return -1;
		}
	}
}
