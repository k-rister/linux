// SPDX-License-Identifier: GPL-2.0-only

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/cpu_accel.h>

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

static unsigned int target_cpu(void)
{
	const char *target_env = getenv("CPU_ACCEL_UAPI_CPU");
	cpu_set_t allowed;
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
		fail("get ABI 23 compatibility affinity");
	for (unsigned int cpu = 1; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &allowed) && (int)cpu != current_cpu)
			return cpu;
	}
	return 0;
}

static int terminal_state(unsigned int state)
{
	return state == CPU_ACCEL_STATE_COMPLETE ||
	       state == CPU_ACCEL_STATE_STOPPED ||
	       state == CPU_ACCEL_STATE_ERROR ||
	       state == CPU_ACCEL_STATE_WATCHDOG ||
	       state == CPU_ACCEL_STATE_ESCAPED;
}

static unsigned int shared_state(struct cpu_accel_shared *shared)
{
	return __atomic_load_n(&shared->state, __ATOMIC_SEQ_CST);
}

int main(void)
{
	struct cpu_accel_config config = {
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 100000000,
		.period_ns = 1000000,
	};
	struct cpu_accel_capabilities capabilities;
	struct timespec delay = { .tv_nsec = 10000000 };
	struct cpu_accel_shared *shared;
	unsigned int cpu = target_cpu();
	int fd;

	if (!cpu) {
		fprintf(stderr, "no ABI 23 compatibility target CPU\n");
		return 1;
	}
	config.cpu = cpu;

	fd = open("/dev/cpu_accel", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fail("open ABI 23 compatibility device");
	shared = mmap(NULL, CPU_ACCEL_MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	if (shared == MAP_FAILED)
		fail("map ABI 23 compatibility control page");

	if (shared->abi_version != CPU_ACCEL_ABI_MIN_VERSION ||
	    shared->struct_size != sizeof(struct cpu_accel_shared) ||
	    shared->layout_fingerprint != CPU_ACCEL_LAYOUT_FINGERPRINT()) {
		errno = EPROTO;
		fail("verify ABI 23 control mapping");
	}
	if (ioctl(fd, CPU_ACCEL_IOC_GET_CAPABILITIES, &capabilities) < 0)
		fail("get ABI 23 capabilities");
	if (ioctl(fd, CPU_ACCEL_IOC_CONFIG, &config) < 0)
		fail("configure ABI 23 workload");
	if (ioctl(fd, CPU_ACCEL_IOC_START, NULL) < 0)
		fail("start ABI 23 workload");

	for (unsigned int attempts = 0; attempts < 200; attempts++) {
		if (terminal_state(shared_state(shared)))
			break;
		nanosleep(&delay, NULL);
	}
	if (!terminal_state(shared_state(shared))) {
		errno = ETIMEDOUT;
		fail("wait for ABI 23 terminal state");
	}
	if (shared->mode != CPU_ACCEL_MODE_LINUX) {
		errno = EPROTO;
		fail("return ABI 23 workload to Linux mode");
	}
	if (ioctl(fd, CPU_ACCEL_IOC_EXIT, NULL) < 0)
		fail("exit ABI 23 lifecycle");
	if (ioctl(fd, CPU_ACCEL_IOC_RESET, NULL) < 0)
		fail("reset ABI 23 session");

	if (munmap((void *)shared, CPU_ACCEL_MAP_SIZE) < 0)
		fail("unmap ABI 23 control page");
	if (close(fd) < 0)
		fail("close ABI 23 device");
	return 0;
}
