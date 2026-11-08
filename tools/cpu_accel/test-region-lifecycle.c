// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"
#include "cpu_accel_workload.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t page_size;
enum {
	CPU_ACCEL_MAX_TEST_REGIONS = 64,
};

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

static void expect_mmap_failure(const char *message, size_t size, int prot,
				int flags, int fd, off_t offset)
{
	void *mapping = mmap(NULL, size, prot, flags, fd, offset);

	if (mapping != MAP_FAILED) {
		munmap(mapping, size);
		fprintf(stderr, "%s unexpectedly succeeded\n", message);
		exit(1);
	}
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

static void create_region(struct cpu_accel_handle *handle,
			  struct cpu_accel_region_create *create,
			  uint64_t size)
{
	create->size_bytes = size;
	create->permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL;
	create->numa_node = -1;
	if (cpu_accel_region_create(handle, create) < 0)
		fail("create region");
}

static void destroy_unmapped(struct cpu_accel_handle *handle,
			     struct cpu_accel_region_create *create)
{
	struct cpu_accel_region_info info = {
		.region_id = create->region_id,
	};
	struct cpu_accel_region_ref ref = {
		.region_id = create->region_id,
	};

	if (cpu_accel_region_get_info(handle, &info) < 0)
		fail("get destroy epoch");
	ref.epoch = info.epoch;
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy unmapped region");
}

static void test_invalid_create(struct cpu_accel_handle *handle,
				const struct cpu_accel_capabilities *capabilities)
{
	struct cpu_accel_region_create create = {
		.size_bytes = 0,
		.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL,
		.numa_node = -1,
	};

	errno = 0;
	expect_ioctl_failure("create zero-sized region",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.size_bytes = page_size + 1;
	errno = 0;
	expect_ioctl_failure("create unaligned region",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.size_bytes = capabilities->max_region_bytes + page_size;
	errno = 0;
	expect_ioctl_failure("create oversized region",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.size_bytes = page_size;
	create.permissions = CPU_ACCEL_REGION_PERM_OWNER_WRITE;
	errno = 0;
	expect_ioctl_failure("create invalid permissions",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL | (1U << 31);
	errno = 0;
	expect_ioctl_failure("create reserved permissions",
			     cpu_accel_region_create(handle, &create), EINVAL);
	create.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL;
	create.numa_node = INT32_MAX;
	errno = 0;
	expect_ioctl_failure("create invalid NUMA node",
			     cpu_accel_region_create(handle, &create), EINVAL);
}

static void test_sizes(struct cpu_accel_handle *handle)
{
	const uint64_t sizes[] = { 64 * 1024, 1024 * 1024,
				   1ULL << 30 };
	struct cpu_accel_region_create create = {};

	for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]);
	     index++) {
		create_region(handle, &create, sizes[index]);
		if (create.size_bytes != sizes[index]) {
			fprintf(stderr, "region size was not exact\n");
			exit(1);
		}
		destroy_unmapped(handle, &create);
	}
}

static void test_detach(struct cpu_accel_handle *handle)
{
	struct cpu_accel_region_create create = {};
	struct cpu_accel_region_info info = {};
	struct cpu_accel_region_ref ref = {};
	uint8_t *manager;

	create_region(handle, &create, page_size);
	manager = cpu_accel_region_mmap(handle, &create, true);
	if (manager == MAP_FAILED)
		fail("map detach-test region");
	ref.region_id = create.region_id;
	ref.epoch = create.epoch;
	if (cpu_accel_region_attach(handle, &ref) < 0)
		fail("attach detach-test region");
	if (cpu_accel_region_detach(handle, &ref) < 0)
		fail("detach detach-test region");
	info.region_id = create.region_id;
	if (cpu_accel_region_get_info(handle, &info) < 0)
		fail("get detached region info");
	if (info.state != CPU_ACCEL_REGION_CREATED ||
	    info.epoch == create.epoch) {
		fprintf(stderr, "detach did not reset region epoch\n");
		exit(1);
	}
	if (munmap(manager, create.size_bytes) < 0)
		fail("unmap detach-test region");
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy detach-test region");
}

static void test_count_limit(struct cpu_accel_handle *handle,
			     const struct cpu_accel_capabilities *capabilities)
{
	struct cpu_accel_region_create creates[CPU_ACCEL_MAX_TEST_REGIONS] = {};
	struct cpu_accel_region_create create = {
		.size_bytes = page_size,
		.permissions = CPU_ACCEL_REGION_PERM_BIDIRECTIONAL,
		.numa_node = -1,
	};

	for (uint32_t index = 0; index < capabilities->max_region_count;
	     index++) {
		create_region(handle, &creates[index], page_size);
		if (creates[index].region_id != index + 1) {
			fprintf(stderr, "unexpected region ID allocation\n");
			exit(1);
		}
	}
	errno = 0;
	expect_ioctl_failure("create beyond region count limit",
			     cpu_accel_region_create(handle, &create),
			     ENOSPC);
	for (uint32_t index = 0; index < capabilities->max_region_count;
	     index++)
		destroy_unmapped(handle, &creates[index]);
}

static unsigned int test_target_cpu(void)
{
	cpu_set_t allowed;
	int current_cpu = sched_getcpu();

	if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0)
		fail("get test CPU affinity");
	for (unsigned int cpu = 1; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &allowed) && (int)cpu != current_cpu)
			return cpu;
	}
	return 0;
}

static void test_destroy_while_configured(struct cpu_accel_handle *handle)
{
	struct cpu_accel_region_create create = {};
	struct cpu_accel_region_ref ref = {};
	struct cpu_accel_config config = {
		.cpu = test_target_cpu(),
		.flags = CPU_ACCEL_FLAG_IRQS_OFF,
		.workload = CPU_ACCEL_WORKLOAD_TIMESTAMP,
		.duration_ns = 20 * 1000 * 1000,
		.period_ns = 1000 * 1000,
	};

	if (!config.cpu)
		return;
	create_region(handle, &create, page_size);
	ref.region_id = create.region_id;
	ref.epoch = create.epoch;
	if (cpu_accel_configure(handle, &config) < 0)
		fail("configure destroy-test workload");
	errno = 0;
	expect_ioctl_failure("destroy configured region",
			     cpu_accel_region_destroy(handle, &ref), EBUSY);
	if (cpu_accel_reset(handle) < 0)
		fail("reset destroy-test workload");
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy region after reset");
}

static void test_abi_mismatch(void)
{
	struct cpu_accel_handle handle;
	struct cpu_accel_shared *shared;
	int fd;
	void *mapping;

	fd = memfd_create("cpu-accel-old-abi", 0);
	if (fd < 0)
		fail("create old-ABI test file");
	if (ftruncate(fd, CPU_ACCEL_MAP_SIZE) < 0)
		fail("size old-ABI test file");
	mapping = mmap(NULL, CPU_ACCEL_MAP_SIZE, PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
		fail("map old-ABI test file");
	shared = mapping;
	shared->abi_version = CPU_ACCEL_ABI_VERSION - 1;
	shared->struct_size = sizeof(*shared);
	shared->layout_fingerprint = CPU_ACCEL_LAYOUT_FINGERPRINT();
	if (munmap(mapping, CPU_ACCEL_MAP_SIZE) < 0)
		fail("unmap old-ABI test file");
	errno = 0;
	expect_ioctl_failure("attach old-ABI control mapping",
			     cpu_accel_attach_fd(&handle, fd), EPROTO);
	close(fd);
}

static void test_mapping_and_roles(struct cpu_accel_handle *handle)
{
	struct cpu_accel_region_create create = {};
	struct cpu_accel_region_info info = {};
	struct cpu_accel_region_ref ref = {};
	uint8_t *manager;
	int pipes[2];
	pid_t child;
	uint8_t status = 1;
	ssize_t length;

	create_region(handle, &create, page_size);
	manager = cpu_accel_region_mmap(handle, &create, true);
	if (manager == MAP_FAILED)
		fail("map manager region");
	expect_mmap_failure("map wrong token", create.size_bytes,
			    PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd,
			    create.mmap_token + page_size);
	expect_mmap_failure("map short region", create.size_bytes - page_size,
			    PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd,
			    create.mmap_token);
	expect_mmap_failure("map long region", create.size_bytes + page_size,
			    PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd,
			    create.mmap_token);
	expect_mmap_failure("map read-only region", create.size_bytes,
			    PROT_READ, MAP_SHARED, handle->fd,
			    create.mmap_token);
	expect_mmap_failure("map executable region", create.size_bytes,
			    PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED,
			    handle->fd, create.mmap_token);
	expect_mmap_failure("map private region", create.size_bytes,
			    PROT_READ | PROT_WRITE, MAP_PRIVATE, handle->fd,
			    create.mmap_token);
	expect_mmap_failure("duplicate manager mapping", create.size_bytes,
			    PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd,
			    create.mmap_token);

	ref.region_id = create.region_id;
	ref.epoch = create.epoch + 1;
	errno = 0;
	expect_ioctl_failure("attach stale region",
			     cpu_accel_region_attach(handle, &ref), EINVAL);
	ref.epoch = create.epoch;
	if (cpu_accel_region_attach(handle, &ref) < 0)
		fail("attach region");

	info.region_id = create.region_id;
	if (cpu_accel_region_get_info(handle, &info) < 0)
		fail("get attached region info");
	if (info.state != CPU_ACCEL_REGION_ATTACHED ||
	    info.epoch != ref.epoch || info.manager_mappings != 1 ||
	    info.owner_mappings != 0) {
		fprintf(stderr, "invalid attached region info\n");
		exit(1);
	}

	errno = 0;
	expect_ioctl_failure("destroy attached region",
			     cpu_accel_region_destroy(handle, &ref), EBUSY);

	if (pipe(pipes) < 0)
		fail("create region test pipe");
	child = fork();
	if (child < 0)
		fail("fork region owner");
	if (!child) {
		uint8_t *owner;

		close(pipes[0]);
		if (munmap(manager, create.size_bytes) < 0) {
			perror("unmap inherited manager region");
			_exit(1);
		}
		errno = 0;
		owner = mmap(NULL, create.size_bytes, PROT_READ, MAP_SHARED,
			     handle->fd, create.mmap_token);
		if (owner != MAP_FAILED) {
			munmap(owner, create.size_bytes);
			fprintf(stderr, "read-only owner mapping succeeded\n");
			_exit(1);
		}
		if (errno != EACCES) {
			fprintf(stderr,
				"read-only owner mapping returned errno %d\n",
				errno);
			_exit(1);
		}
		owner = mmap(NULL, create.size_bytes, PROT_READ | PROT_WRITE,
			     MAP_SHARED, handle->fd, create.mmap_token);
		if (owner == MAP_FAILED) {
			perror("map owner region");
			_exit(1);
		}
		owner[0] = 0xa5;
		status = 0;
		if (write(pipes[1], &status, sizeof(status)) != sizeof(status))
			_exit(1);
		_exit(0);
	}
	close(pipes[1]);
	length = read(pipes[0], &status, sizeof(status));
	close(pipes[0]);
	if (length != sizeof(status))
		fail("read region owner status");
	if (waitpid(child, NULL, 0) < 0)
		fail("wait region owner");
	if (status || manager[0] != 0xa5) {
		fprintf(stderr, "owner region role test failed\n");
		exit(1);
	}

	if (cpu_accel_reset(handle) < 0)
		fail("reset attached region");
	if (cpu_accel_region_get_info(handle, &info) < 0)
		fail("get reset region info");
	if (info.state != CPU_ACCEL_REGION_CREATED || info.epoch == ref.epoch) {
		fprintf(stderr, "reset did not detach region\n");
		exit(1);
	}
	errno = 0;
	expect_ioctl_failure("destroy mapped region",
			     cpu_accel_region_destroy(handle, &ref), EBUSY);
	if (munmap(manager, create.size_bytes) < 0)
		fail("unmap manager region");
	errno = 0;
	expect_ioctl_failure("destroy stale region",
			     cpu_accel_region_destroy(handle, &ref), EINVAL);
	ref.epoch = info.epoch;
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy reset region");
}

static void test_workload_header_validation(struct cpu_accel_handle *handle)
{
	struct cpu_accel_workload_region *region;
	struct cpu_accel_region_create create = {};
	struct cpu_accel_region_ref ref = {};

	create_region(handle, &create, page_size);
	region = cpu_accel_region_mmap(handle, &create, true);
	if (region == MAP_FAILED)
		fail("map workload header region");
	region->magic = CPU_ACCEL_WORKLOAD_MAGIC;
	region->abi_version = CPU_ACCEL_WORKLOAD_ABI_VERSION;
	region->header_size = CPU_ACCEL_WORKLOAD_HEADER_BYTES;
	region->region_size = create.size_bytes;
	region->region_epoch = create.epoch;
	region->mmap_token = create.mmap_token;
	if (!cpu_accel_workload_header_valid(region, create.size_bytes,
					     create.epoch,
					     create.mmap_token)) {
		fprintf(stderr, "valid workload header was rejected\n");
		exit(1);
	}
	region->abi_version++;
	if (cpu_accel_workload_header_valid(region, create.size_bytes,
					    create.epoch,
					    create.mmap_token)) {
		fprintf(stderr, "stale workload ABI was accepted\n");
		exit(1);
	}
	region->abi_version--;
	region->region_epoch++;
	if (cpu_accel_workload_header_valid(region, create.size_bytes,
					    create.epoch,
					    create.mmap_token)) {
		fprintf(stderr, "stale workload epoch was accepted\n");
		exit(1);
	}
	if (munmap(region, create.size_bytes) < 0)
		fail("unmap workload header region");
	ref.region_id = create.region_id;
	ref.epoch = create.epoch;
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy workload header region");
}

static void test_directional_region(struct cpu_accel_handle *handle,
				     uint32_t permissions, bool owner_write)
{
	struct cpu_accel_region_create create = {
		.permissions = permissions,
		.numa_node = -1,
	};
	struct cpu_accel_region_info info = {};
	struct cpu_accel_region_ref ref = {};
	uint8_t *manager;
	uint8_t status = 1;
	int pipes[2];
	pid_t child;
	ssize_t length;

	create.size_bytes = page_size;
	if (cpu_accel_region_create(handle, &create) < 0)
		fail("create directional region");
	if (!(permissions & CPU_ACCEL_REGION_PERM_MANAGER_WRITE)) {
		errno = 0;
		expect_mmap_failure("map writable read-only manager region",
				    create.size_bytes, PROT_READ | PROT_WRITE,
				    MAP_SHARED, handle->fd,
				    create.mmap_token);
		if (errno != EACCES) {
			fprintf(stderr,
				"writable read-only manager mapping returned errno %d\n",
				errno);
			exit(1);
		}
	}
	manager = cpu_accel_region_mmap(handle, &create, true);
	if (manager == MAP_FAILED)
		fail("map directional manager region");
	if (!owner_write)
		manager[0] = 0xa5;
	ref.region_id = create.region_id;
	ref.epoch = create.epoch;
	if (cpu_accel_region_attach(handle, &ref) < 0)
		fail("attach directional region");
	if (pipe(pipes) < 0)
		fail("create directional region pipe");
	child = fork();
	if (child < 0)
		fail("fork directional region owner");
	if (!child) {
		uint8_t *owner;
		int wrong_prot = owner_write ? PROT_READ :
			PROT_READ | PROT_WRITE;
		int owner_prot = owner_write ? PROT_READ | PROT_WRITE :
			PROT_READ;

		close(pipes[0]);
		if (munmap(manager, create.size_bytes) < 0)
			_exit(1);
		errno = 0;
		owner = mmap(NULL, create.size_bytes, wrong_prot, MAP_SHARED,
			     handle->fd, create.mmap_token);
		if (owner != MAP_FAILED || errno != EACCES)
			_exit(1);
		owner = mmap(NULL, create.size_bytes, owner_prot, MAP_SHARED,
			     handle->fd, create.mmap_token);
		if (owner == MAP_FAILED)
			_exit(1);
		if (owner_write) {
			owner[0] = 0x5a;
			status = 0;
		} else {
			status = owner[0] == 0xa5 ? 0 : 1;
		}
		if (write(pipes[1], &status, sizeof(status)) != sizeof(status))
			_exit(1);
		_exit(0);
	}
	close(pipes[1]);
	length = read(pipes[0], &status, sizeof(status));
	close(pipes[0]);
	if (length != sizeof(status))
		fail("read directional region status");
	if (waitpid(child, NULL, 0) < 0)
		fail("wait directional region owner");
	if (status || manager[0] != (owner_write ? 0x5a : 0xa5)) {
		fprintf(stderr, "directional region role test failed\n");
		exit(1);
	}
	if (cpu_accel_reset(handle) < 0)
		fail("reset directional region");
	info.region_id = create.region_id;
	if (cpu_accel_region_get_info(handle, &info) < 0)
		fail("get directional region info");
	if (info.state != CPU_ACCEL_REGION_CREATED) {
		fprintf(stderr, "directional region was not detached\n");
		exit(1);
	}
	if (munmap(manager, create.size_bytes) < 0)
		fail("unmap directional manager region");
	ref.epoch = info.epoch;
	if (cpu_accel_region_destroy(handle, &ref) < 0)
		fail("destroy directional region");
}

int main(void)
{
	struct cpu_accel_capabilities capabilities = {};
	struct cpu_accel_handle handle;

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size == (size_t)-1)
		fail("get page size");
	if (cpu_accel_open(&handle) < 0)
		fail("open cpu accelerator");
	if (cpu_accel_get_capabilities(&handle, &capabilities) < 0)
		fail("get region capabilities");
	if (!capabilities.max_region_count ||
	    capabilities.max_region_count > 64 ||
	    capabilities.max_region_bytes != 1ULL << 30 ||
	    capabilities.max_total_region_bytes != 4ULL << 30 ||
	    capabilities.supported_region_permissions !=
		    (CPU_ACCEL_REGION_PERM_BIDIRECTIONAL |
		     CPU_ACCEL_REGION_PERM_MANAGER_TO_OWNER |
		     CPU_ACCEL_REGION_PERM_OWNER_TO_MANAGER)) {
		fprintf(stderr, "invalid region capabilities\n");
		return 1;
	}

	test_invalid_create(&handle, &capabilities);
	test_sizes(&handle);
	test_detach(&handle);
	test_count_limit(&handle, &capabilities);
	test_destroy_while_configured(&handle);
	test_mapping_and_roles(&handle);
	test_workload_header_validation(&handle);
	test_directional_region(&handle,
				CPU_ACCEL_REGION_PERM_OWNER_TO_MANAGER, true);
	test_directional_region(&handle,
				CPU_ACCEL_REGION_PERM_MANAGER_TO_OWNER, false);
	cpu_accel_close(&handle);
	test_abi_mismatch();
	return 0;
}
