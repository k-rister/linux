// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

static bool terminal_state(uint32_t state)
{
	return state == CPU_ACCEL_STATE_COMPLETE ||
		state == CPU_ACCEL_STATE_STOPPED ||
		state == CPU_ACCEL_STATE_ERROR ||
		state == CPU_ACCEL_STATE_WATCHDOG ||
		state == CPU_ACCEL_STATE_ESCAPED;
}

static void set_activation_fail_worker(uint32_t worker_index)
{
	const char *path =
		"/sys/module/cpu_accel/parameters/domain_activation_fail_worker";
	FILE *parameter = fopen(path, "w");
	int write_result;
	int close_result;

	if (!parameter)
		fail("set domain activation failure worker");
	write_result = fprintf(parameter, "%u\n", worker_index);
	close_result = fclose(parameter);
	if (write_result < 0 || close_result) {
		errno = errno ? errno : EIO;
		fail("set domain activation failure worker");
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

static unsigned int parse_smp_cpus(unsigned int cpus[CPU_ACCEL_MAX_WORKERS])
{
	const char *list = getenv("CPU_ACCEL_UAPI_SMP_CPUS");
	unsigned int count = 0;

	if (!list || !*list)
		return 0;
	while (*list) {
		char *end = NULL;
		unsigned long cpu;

		errno = 0;
		cpu = strtoul(list, &end, 10);
		if (errno || end == list || (*end != ',' && *end != '\0') ||
		    cpu >= CPU_SETSIZE || !cpu || count >= CPU_ACCEL_MAX_WORKERS)
			fail("parse CPU_ACCEL_UAPI_SMP_CPUS");
		for (unsigned int index = 0; index < count; index++) {
			if (cpus[index] == cpu)
				fail("parse CPU_ACCEL_UAPI_SMP_CPUS");
		}
		cpus[count++] = cpu;
		list = end;
		if (!*list)
			break;
		if (*list++ != ',' || !*list)
			fail("parse CPU_ACCEL_UAPI_SMP_CPUS");
	}
	if (count < 2)
		fail("parse CPU_ACCEL_UAPI_SMP_CPUS");
	return count;
}

static void copy_smp_cpus(struct cpu_accel_domain_config *config,
			  const unsigned int *cpus, unsigned int cpu_count)
{
	config->cpu_count = cpu_count;
	for (unsigned int index = 0; index < cpu_count; index++)
		config->cpus[index] = cpus[index];
}

static int send_fd(int socket, int fd)
{
	struct msghdr message = {};
	struct iovec iov = {
		.iov_base = (void *)"f",
		.iov_len = 1,
	};
	char control[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;

	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&message);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	return sendmsg(socket, &message, 0);
}

static int recv_fd(int socket, int *fd)
{
	struct msghdr message = {};
	struct iovec iov = {
		.iov_base = (void *)malloc(1),
		.iov_len = 1,
	};
	char control[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	int ret;

	if (!iov.iov_base)
		fail("allocate receive buffer");
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	ret = recvmsg(socket, &message, 0);
	free(iov.iov_base);
	if (ret < 0)
		return ret;
	cmsg = CMSG_FIRSTHDR(&message);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
		return -1;
	memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
	return 0;
}

static void test_manager_crash(unsigned int cpu)
{
	struct cpu_accel_config config = {
		.cpu = cpu,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF |
			 CPU_ACCEL_FLAG_PERSISTENT,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 5000000000ULL,
		.period_ns = 1000000,
	};
	int pipes[2];
	pid_t child;
	char marker;
	struct cpu_accel_handle handle;
	uint32_t state;
	uint32_t mode;

	if (!cpu)
		return;
	if (pipe(pipes) < 0)
		fail("create manager crash pipe");
	child = fork();
	if (child < 0)
		fail("fork manager crash child");
	if (!child) {
		cpu_set_t manager_affinity;

		close(pipes[0]);
		CPU_ZERO(&manager_affinity);
		CPU_SET(cpu ? 0 : 1, &manager_affinity);
		if (sched_setaffinity(0, sizeof(manager_affinity),
				      &manager_affinity) < 0)
			fail("pin manager crash child");
		if (cpu_accel_open(&handle) < 0)
			fail("open manager crash device");
		if (cpu_accel_configure(&handle, &config) < 0)
			fail("configure manager crash workload");
		if (cpu_accel_start(&handle) < 0)
			fail("start manager crash workload");
		if (write(pipes[1], "R", 1) != 1)
			fail("signal manager crash start");
		pause();
		_exit(0);
	}
	close(pipes[1]);
	if (read(pipes[0], &marker, 1) != 1 || marker != 'R')
		fail("wait for manager crash start");
	close(pipes[0]);
	if (kill(child, SIGKILL) < 0)
		fail("kill manager crash child");
	if (waitpid(child, NULL, 0) < 0)
		fail("reap manager crash child");
	if (cpu_accel_open(&handle) < 0)
		fail("reopen after manager crash");
	state = handle.shared->state;
	mode = handle.shared->mode;
	if (mode != CPU_ACCEL_MODE_LINUX ||
	    (state != CPU_ACCEL_STATE_COMPLETE &&
	     state != CPU_ACCEL_STATE_STOPPED &&
	     state != CPU_ACCEL_STATE_ERROR)) {
		fprintf(stderr, "manager crash left invalid state %u mode %u\n",
			state, mode);
		exit(1);
	}
	if (cpu_accel_reset(&handle) < 0)
		fail("reset after manager crash");
	cpu_accel_close(&handle);
}

static void test_smp_manager_crash(const unsigned int *cpus,
				   unsigned int cpu_count)
{
	struct cpu_accel_domain_config config = {
		.cpu_count = cpu_count,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF |
			 CPU_ACCEL_FLAG_PERSISTENT,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 5000000000ULL,
		.period_ns = 1000000,
	};
	struct cpu_accel_domain_status status = {};
	int pipes[2];
	pid_t child;
	char marker;
	struct cpu_accel_handle handle;

	copy_smp_cpus(&config, cpus, cpu_count);
	if (pipe(pipes) < 0)
		fail("create SMP manager crash pipe");
	child = fork();
	if (child < 0)
		fail("fork SMP manager crash child");
	if (!child) {
		cpu_set_t manager_affinity;

		close(pipes[0]);
		CPU_ZERO(&manager_affinity);
		CPU_SET(0, &manager_affinity);
		if (sched_setaffinity(0, sizeof(manager_affinity),
				      &manager_affinity) < 0)
			fail("pin SMP manager crash child");
		if (cpu_accel_open(&handle) < 0)
			fail("open SMP manager crash device");
		if (cpu_accel_domain_config(&handle, &config) < 0)
			fail("configure SMP manager crash domain");
		if (cpu_accel_domain_start(&handle) < 0)
			fail("start SMP manager crash domain");
		if (write(pipes[1], "R", 1) != 1)
			fail("signal SMP manager crash start");
		pause();
		_exit(0);
	}
	close(pipes[1]);
	if (read(pipes[0], &marker, 1) != 1 || marker != 'R')
		fail("wait for SMP manager crash start");
	close(pipes[0]);
	if (kill(child, SIGKILL) < 0)
		fail("kill SMP manager crash child");
	if (waitpid(child, NULL, 0) < 0)
		fail("reap SMP manager crash child");
	if (cpu_accel_open(&handle) < 0)
		fail("reopen after SMP manager crash");
	if (cpu_accel_domain_status(&handle, &status) < 0)
		fail("get SMP manager crash status");
	if (status.worker_count != cpu_count ||
	    status.joined_workers != cpu_count ||
	    status.terminal_workers != cpu_count ||
	    !terminal_state(status.state) ||
	    handle.shared->mode != CPU_ACCEL_MODE_LINUX) {
		fprintf(stderr,
			"SMP manager crash left invalid state %u mode %u workers %u/%u\n",
			status.state, handle.shared->mode,
			status.terminal_workers, status.worker_count);
		exit(1);
	}
	if (cpu_accel_reset(&handle) < 0)
		fail("reset after SMP manager crash");
	cpu_accel_close(&handle);
}

static void test_smp_activation_rollback(const unsigned int *cpus,
					 unsigned int cpu_count)
{
	struct cpu_accel_domain_config config = {
		.cpu_count = cpu_count,
		.flags = CPU_ACCEL_FLAG_IRQS_OFF |
			 CPU_ACCEL_FLAG_PERSISTENT,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 5000000000ULL,
		.period_ns = 1000000,
	};
	struct cpu_accel_domain_queue_config queue_config = {
		.queue_count = 1,
		.region_ids[0] = 1,
		.capacities[0] = 16,
	};
	struct cpu_accel_domain_status status = {};
	struct cpu_accel_handle handle;
	int start_result;
	int start_errno;

	copy_smp_cpus(&config, cpus, cpu_count);
	if (cpu_accel_open(&handle) < 0)
		fail("open SMP activation rollback device");
	if (cpu_accel_domain_config(&handle, &config) < 0) {
		cpu_accel_close(&handle);
		fail("configure SMP activation rollback domain");
	}
	errno = 0;
	if (cpu_accel_domain_queue_config(&handle, &queue_config) == 0 ||
	    errno != EINVAL) {
		cpu_accel_close(&handle);
		fail("reject SPSC queue topology for non-pipeline domain");
	}
	set_activation_fail_worker(1);
	errno = 0;
	start_result = cpu_accel_domain_start(&handle);
	start_errno = errno;
	set_activation_fail_worker(UINT32_MAX);
	if (start_result == 0)
		(void)cpu_accel_stop(&handle);
	if (cpu_accel_domain_status(&handle, &status) < 0) {
		cpu_accel_close(&handle);
		fail("get SMP activation rollback status");
	}
	if (start_result == 0 || start_errno != ENOMEM ||
	    status.state != CPU_ACCEL_STATE_ERROR ||
	    status.worker_count != cpu_count ||
	    status.terminal_workers != cpu_count ||
	    handle.shared->mode != CPU_ACCEL_MODE_LINUX) {
		fprintf(stderr,
			"SMP activation rollback failed with errno %d state %u mode %u\n",
			start_errno, status.state, handle.shared->mode);
		exit(1);
	}
	if (cpu_accel_exit(&handle) < 0) {
		cpu_accel_close(&handle);
		fail("exit SMP activation rollback domain");
	}
	if (cpu_accel_reset(&handle) < 0) {
		cpu_accel_close(&handle);
		fail("reset SMP activation rollback domain");
	}
	cpu_accel_close(&handle);
}

static void test_descriptor_passing(void)
{
	struct cpu_accel_handle handle;
	int sockets[2];
	pid_t child;
	int status;

	if (cpu_accel_open(&handle) < 0)
		fail("open descriptor passing device");
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0)
		fail("create descriptor passing socketpair");
	child = fork();
	if (child < 0)
		fail("fork descriptor passing child");
	if (!child) {
		struct cpu_accel_handle handle;
		int fd;

		close(sockets[0]);
		if (recv_fd(sockets[1], &fd) < 0)
			fail("receive accelerator descriptor");
		close(sockets[1]);
		if (cpu_accel_attach_fd(&handle, fd) < 0)
			fail("attach passed accelerator descriptor");
		if (handle.shared->abi_version < CPU_ACCEL_ABI_MIN_VERSION ||
		    handle.shared->abi_version > CPU_ACCEL_ABI_VERSION) {
			fprintf(stderr, "passed descriptor used invalid ABI\n");
			_exit(1);
		}
		cpu_accel_close(&handle);
		_exit(0);
	}
	close(sockets[1]);
	if (send_fd(sockets[0], handle.fd) < 0)
		fail("send accelerator descriptor");
	close(sockets[0]);
	if (waitpid(child, &status, 0) < 0)
		fail("reap descriptor passing child");
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "descriptor passing child failed\n");
		exit(1);
	}
	cpu_accel_close(&handle);
}

static void test_exec_cloexec(bool clear_cloexec)
{
	struct cpu_accel_handle handle;
	pid_t child;
	int status;
	char fd_arg[16];
	char clear_arg[2] = { clear_cloexec ? '1' : '0', '\0' };
	char *const argv[] = {
		"/proc/self/exe",
		"--cpu-accel-exec-child",
		fd_arg,
		clear_arg,
		NULL,
	};

	if (cpu_accel_open(&handle) < 0)
		fail("open exec test device");
	snprintf(fd_arg, sizeof(fd_arg), "%d", handle.fd);
	child = fork();
	if (child < 0)
		fail("fork exec test child");
	if (!child) {
		int fd_flags;

		if (clear_cloexec) {
			fd_flags = fcntl(handle.fd, F_GETFD);
			if (fd_flags < 0 ||
			    fcntl(handle.fd, F_SETFD,
				  fd_flags & ~FD_CLOEXEC) < 0)
				fail("clear FD_CLOEXEC");
		}
		execv("/proc/self/exe", argv);
		fail("exec cpu-accel lifecycle child");
	}
	if (waitpid(child, &status, 0) < 0)
		fail("reap exec test child");
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "exec test child failed\n");
		exit(1);
	}
	cpu_accel_close(&handle);
}

static void test_fork_without_exec(void)
{
	struct cpu_accel_handle handle;
	pid_t child;
	int status;

	if (cpu_accel_open(&handle) < 0)
		fail("open fork test device");
	child = fork();
	if (child < 0)
		fail("fork without exec child");
	if (!child) {
		struct cpu_accel_handle child_handle;

		memset(&child_handle, 0, sizeof(child_handle));
		child_handle.fd = handle.fd;
		child_handle.shared = handle.shared;
		if (child_handle.shared->abi_version <
			    CPU_ACCEL_ABI_MIN_VERSION ||
		    child_handle.shared->abi_version > CPU_ACCEL_ABI_VERSION) {
			fprintf(stderr, "forked descriptor used invalid ABI\n");
			_exit(1);
		}
		_exit(0);
	}
	if (waitpid(child, &status, 0) < 0)
		fail("reap fork without exec child");
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "fork without exec child failed\n");
		exit(1);
	}
	cpu_accel_close(&handle);
}

int main(int argc, char **argv)
{
	unsigned int cpu = test_target_cpu();
	unsigned int smp_cpus[CPU_ACCEL_MAX_WORKERS];
	unsigned int smp_cpu_count;

	if (argc == 4 && !strcmp(argv[1], "--cpu-accel-exec-child")) {
		struct cpu_accel_handle handle;
		int fd = atoi(argv[2]);
		bool clear_cloexec = argv[3][0] == '1';

		if (cpu_accel_attach_fd(&handle, fd) < 0) {
			if (!clear_cloexec && errno == EBADF)
				_exit(0);
			fail("attach exec child descriptor");
		}
		if (!clear_cloexec) {
			fprintf(stderr, "accidental exec retained descriptor\n");
			_exit(1);
		}
		cpu_accel_close(&handle);
		_exit(0);
	}
	if (argc != 1) {
		fprintf(stderr, "unexpected arguments\n");
		return 1;
	}
	smp_cpu_count = parse_smp_cpus(smp_cpus);
	if (smp_cpu_count) {
		test_smp_activation_rollback(smp_cpus, smp_cpu_count);
		test_smp_manager_crash(smp_cpus, smp_cpu_count);
		return 0;
	}
	test_manager_crash(cpu);
	test_descriptor_passing();
	test_exec_cloexec(false);
	test_exec_cloexec(true);
	test_fork_without_exec();
	return 0;
}
