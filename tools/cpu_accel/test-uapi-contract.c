// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

static void expect_ioctl_failure(const char *message, int result, int expected)
{
	if (!result) {
		fprintf(stderr, "%s unexpectedly succeeded\n", message);
		exit(1);
	}
	if (errno != expected) {
		fprintf(stderr, "%s returned errno %d, expected %d\n",
			message, errno, expected);
		exit(1);
	}
}

static unsigned int test_target_cpu(void)
{
	cpu_set_t allowed;
	const char *target_env = getenv("CPU_ACCEL_UAPI_CPU");
	unsigned long target;
	char *end = NULL;
	int current_cpu = sched_getcpu();

	if (target_env && *target_env) {
		errno = 0;
		target = strtoul(target_env, &end, 10);
		if (errno || !end || *end || target >= CPU_SETSIZE) {
			errno = EINVAL;
			fail("parse CPU_ACCEL_UAPI_CPU");
		}
		return target;
	}
	if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0)
		fail("get test CPU affinity");
	for (unsigned int cpu = 1; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &allowed) && (int)cpu != current_cpu)
			return cpu;
	}
	return 0;
}

static void test_abi_negotiation(struct cpu_accel_handle *handle)
{
	struct cpu_accel_abi_info info = {};
	struct cpu_accel_abi_select select = {
		.requested_abi = CPU_ACCEL_ABI_MIN_VERSION,
	};

	if (cpu_accel_get_abi_info(handle, &info) < 0)
		fail("get ABI info");
	if (info.current_abi != CPU_ACCEL_ABI_VERSION ||
	    info.min_abi != CPU_ACCEL_ABI_MIN_VERSION ||
	    info.max_abi != CPU_ACCEL_ABI_VERSION ||
	    info.selected_abi != CPU_ACCEL_ABI_VERSION) {
		fprintf(stderr, "invalid ABI info\n");
		exit(1);
	}
	if (cpu_accel_set_abi_version(handle, &select) < 0)
		fail("select ABI 23");
	if (select.selected_abi != CPU_ACCEL_ABI_MIN_VERSION) {
		fprintf(stderr, "ABI 23 was not selected\n");
		exit(1);
	}
	if (handle->shared->abi_version != CPU_ACCEL_ABI_MIN_VERSION) {
		fprintf(stderr, "shared mapping did not report ABI 23\n");
		exit(1);
	}
	select.requested_abi = CPU_ACCEL_ABI_VERSION;
	if (cpu_accel_set_abi_version(handle, &select) < 0)
		fail("select ABI 24");
	if (select.selected_abi != CPU_ACCEL_ABI_VERSION) {
		fprintf(stderr, "ABI 24 was not selected\n");
		exit(1);
	}
	if (handle->shared->abi_version != CPU_ACCEL_ABI_VERSION) {
		fprintf(stderr, "shared mapping did not report ABI 24\n");
		exit(1);
	}
}

static void test_invalid_abi_selection(struct cpu_accel_handle *handle)
{
	struct cpu_accel_abi_select select = {
		.requested_abi = CPU_ACCEL_ABI_MIN_VERSION - 1,
	};

	errno = 0;
	expect_ioctl_failure("select unsupported ABI",
			     cpu_accel_set_abi_version(handle, &select), EINVAL);
	select.requested_abi = CPU_ACCEL_ABI_VERSION;
	select.reserved = 1;
	errno = 0;
	expect_ioctl_failure("select ABI with reserved word",
			     cpu_accel_set_abi_version(handle, &select), EINVAL);
}

static void test_invalid_config(struct cpu_accel_handle *handle,
				unsigned int cpu)
{
	struct cpu_accel_config config = {
		.cpu = cpu,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 100000000,
		.period_ns = 1000000,
	};

	if (!cpu)
		return;
	config.flags = CPU_ACCEL_FLAG_IRQS_OFF | (1U << 31);
	errno = 0;
	expect_ioctl_failure("configure unknown flag",
			     cpu_accel_configure(handle, &config), EINVAL);
	config.flags = CPU_ACCEL_FLAG_IRQS_OFF;
	config.reserved = 1;
	errno = 0;
	expect_ioctl_failure("configure reserved word",
			     cpu_accel_configure(handle, &config), EINVAL);
	config.reserved = 0;
}

static void test_invalid_region(struct cpu_accel_handle *handle)
{
	struct cpu_accel_region_create create = {
		.size_bytes = 4096,
		.numa_node = -1,
	};

	create.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL |
		(1U << 31);
	errno = 0;
	expect_ioctl_failure("create region with unknown permission",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL;
	create.reserved = 1;
	errno = 0;
	expect_ioctl_failure("create region with reserved word",
			     cpu_accel_region_create(handle, &create), EINVAL);
}

static void test_unknown_ioctl(struct cpu_accel_handle *handle)
{
	unsigned long command = _IO(CPU_ACCEL_IOC_MAGIC, 0xff);

	errno = 0;
	expect_ioctl_failure("unknown ioctl", ioctl(handle->fd, command, NULL),
			     ENOTTY);
}

static void test_arch_telemetry(struct cpu_accel_handle *handle)
{
	struct cpu_accel_arch_telemetry telemetry = {};

	if (cpu_accel_get_arch_telemetry(handle, &telemetry) < 0)
		fail("get architecture telemetry");
	if (telemetry.reserved) {
		fprintf(stderr, "architecture telemetry reserved word was set\n");
		exit(1);
	}
}

static void test_x86_image_admission(struct cpu_accel_handle *handle)
{
	struct cpu_accel_x86_image_admission image = {
		.user_entry_ip = 1,
		.user_stack_top = 2,
		.user_stack_bytes = 3,
		.user_image_start = 4,
		.user_image_bytes = 5,
		.user_arg = 6,
		.user_escape_ip = 7,
	};

	image.reserved = 1;
	errno = 0;
	expect_ioctl_failure("admit x86 image with reserved word",
			     cpu_accel_x86_image_admission(handle, &image), EINVAL);
	image.reserved = 0;
	image.user_entry_ip = 0;
	errno = 0;
	expect_ioctl_failure("admit x86 image without entry IP",
			     cpu_accel_x86_image_admission(handle, &image), EINVAL);
	image.user_entry_ip = 1;
	if (cpu_accel_x86_image_admission(handle, &image) < 0)
		fail("admit x86 image");
	if (!image.image_handle) {
		fprintf(stderr, "x86 image handle was not returned\n");
		exit(1);
	}
}

int main(void)
{
	struct cpu_accel_handle handle;
	unsigned int cpu = test_target_cpu();

	if (cpu_accel_open(&handle) < 0)
		fail("open cpu accelerator");
	test_abi_negotiation(&handle);
	test_invalid_abi_selection(&handle);
	test_invalid_config(&handle, cpu);
	test_invalid_region(&handle);
	test_unknown_ioctl(&handle);
	test_arch_telemetry(&handle);
	test_x86_image_admission(&handle);
	cpu_accel_close(&handle);
	return 0;
}
