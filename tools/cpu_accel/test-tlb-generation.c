// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/cpu_accel.h>
#include <linux/falloc.h>
#include <linux/kernel-page-flags.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PTE_TABLE_SIZE (512UL * 4096)
#define TABLE_BATCH_PROBE_PTE_TABLES 512UL
#define MMU_GATHER_FINAL_PROBE_SIZE (8UL * 1024UL * 1024UL)
#define MMU_GATHER_PROBE_SIZE (64UL * 1024UL * 1024UL)
#define MMU_GATHER_OWNER_TIMEOUT_MS 12000
#define MMU_GATHER_ESCAPE_AFTER_MS "5000"
#define REUSE_PROBE_SIZE (16UL * 1024UL * 1024UL)
#define ACCEL_DURATION_MS "2000"
/* Escape a stuck owner so a broken synchronous flush cannot strand the VM. */
#define ACCEL_ESCAPE_AFTER_MS "1000"
#define ACCEL_ESCAPE_RETRIES "1"
#define CLI_OUTPUT_SIZE 4096
#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define REUSED_PAGE_MARKER 0xa5
#define RECLAIM_PAGE_MARKER 0x6d
#define COW_SHARED_MARKER 0x39
#define COW_PRIVATE_MARKER 0xc7
#define MAX_PGTABLE_CANDIDATES 256
#define PGTABLE_SCAN_CHUNK 1024

static sigjmp_buf fault_env;
static pid_t cli_pid = -1;
static uintptr_t cli_shared_address;
static unsigned long long flush_ms;
static int cli_output_fd = -1;
static char cli_output[CLI_OUTPUT_SIZE];
static size_t cli_output_size;
static atomic_int keeper_ready = ATOMIC_VAR_INIT(0);
static atomic_int keeper_error = ATOMIC_VAR_INIT(0);
static atomic_bool keeper_stop = ATOMIC_VAR_INIT(false);

struct reschedule_worker_arg {
	int cpu;
	int pipe_fd;
};

static void touch_page(uintptr_t address)
{
	__asm__ __volatile__("movb $0x5a, (%0)" : : "r" (address) : "memory");
}

static int test_table_batch_unmap(void)
{
	size_t bytes = TABLE_BATCH_PROBE_PTE_TABLES * PTE_TABLE_SIZE;
	size_t reserve_bytes = bytes + PTE_TABLE_SIZE;
	void *reservation;
	uintptr_t base;
	size_t prefix, suffix;

	reservation = mmap(NULL, reserve_bytes, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (reservation == MAP_FAILED)
		return -1;

	base = ((uintptr_t)reservation + PTE_TABLE_SIZE - 1) &
		~(PTE_TABLE_SIZE - 1);
	prefix = base - (uintptr_t)reservation;
	suffix = reserve_bytes - prefix - bytes;
	if (prefix && munmap(reservation, prefix)) {
		int saved_errno = errno;

		munmap(reservation, reserve_bytes);
		errno = saved_errno;
		return -1;
	}
	if (suffix && munmap((void *)(base + bytes), suffix)) {
		int saved_errno = errno;

		munmap((void *)base, bytes + suffix);
		errno = saved_errno;
		return -1;
	}

	if (madvise((void *)base, bytes, MADV_NOHUGEPAGE)) {
		int saved_errno = errno;

		munmap((void *)base, bytes);
		errno = saved_errno;
		return -1;
	}

	/* One 4 KiB leaf in each 2 MiB span allocates one PTE table. */
	for (size_t offset = 0; offset < bytes; offset += PTE_TABLE_SIZE)
		touch_page(base + offset);

	return munmap((void *)base, bytes);
}

static unsigned char read_page(uintptr_t address)
{
	unsigned char value;

	__asm__ __volatile__("movb (%1), %0" : "=q" (value) : "r" (address) : "memory");
	return value;
}

static void write_page(uintptr_t address, unsigned char value)
{
	*(volatile unsigned char *)address = value;
}

static int pread_exact(int fd, void *buffer, size_t bytes, off_t offset)
{
	char *cursor = buffer;
	size_t read_bytes = 0;
	ssize_t ret;

	while (read_bytes < bytes) {
		ret = pread(fd, cursor + read_bytes, bytes - read_bytes,
			    offset + read_bytes);
		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0) {
			if (!ret)
				errno = EIO;
			return -1;
		}
		read_bytes += ret;
	}
	return 0;
}

static bool ptable_bitmap_test(const uint8_t *bitmap, size_t pfn)
{
	return bitmap[pfn / 8] & (1U << (pfn % 8));
}

static void ptable_bitmap_set(uint8_t *bitmap, size_t pfn)
{
	bitmap[pfn / 8] |= 1U << (pfn % 8);
}

static int scan_pagetable_flags(int kpageflags_fd, size_t nr_pages,
				const uint8_t *baseline,
				uint64_t *new_ptables, size_t capacity,
				size_t *new_count)
{
	uint64_t flags[PGTABLE_SCAN_CHUNK];
	size_t count = 0;

	for (size_t base = 0; base < nr_pages; base += PGTABLE_SCAN_CHUNK) {
		size_t chunk = nr_pages - base;

		if (chunk > PGTABLE_SCAN_CHUNK)
			chunk = PGTABLE_SCAN_CHUNK;
		if (base > INT64_MAX / sizeof(flags[0])) {
			errno = EOVERFLOW;
			return -1;
		}
		if (pread_exact(kpageflags_fd, flags,
				chunk * sizeof(flags[0]),
				(off_t)(base * sizeof(flags[0]))))
			return -1;
		for (size_t index = 0; index < chunk; index++) {
			size_t pfn = base + index;

			if (ptable_bitmap_test(baseline, pfn) ||
			    !(flags[index] & (1ULL << KPF_PGTABLE)))
				continue;
			if (count == capacity) {
				errno = E2BIG;
				return -1;
			}
			new_ptables[count++] = pfn;
		}
	}
	*new_count = count;
	return 0;
}

static int snapshot_pagetable_flags(int kpageflags_fd, size_t nr_pages,
				    uint8_t *bitmap)
{
	uint64_t flags[PGTABLE_SCAN_CHUNK];

	memset(bitmap, 0, (nr_pages + 7) / 8);
	for (size_t base = 0; base < nr_pages; base += PGTABLE_SCAN_CHUNK) {
		size_t chunk = nr_pages - base;

		if (chunk > PGTABLE_SCAN_CHUNK)
			chunk = PGTABLE_SCAN_CHUNK;
		if (base > INT64_MAX / sizeof(flags[0])) {
			errno = EOVERFLOW;
			return -1;
		}
		if (pread_exact(kpageflags_fd, flags,
				chunk * sizeof(flags[0]),
				(off_t)(base * sizeof(flags[0]))))
			return -1;
		for (size_t index = 0; index < chunk; index++) {
			if (flags[index] & (1ULL << KPF_PGTABLE))
				ptable_bitmap_set(bitmap, base + index);
		}
	}
	return 0;
}

static int released_pagetable_pfns(int kpageflags_fd,
				   const uint64_t *candidates,
				   size_t candidate_count,
				   uint64_t *released, size_t *released_count)
{
	uint64_t flags;
	size_t count = 0;

	for (size_t index = 0; index < candidate_count; index++) {
		if (candidates[index] > INT64_MAX / sizeof(flags)) {
			errno = EOVERFLOW;
			return -1;
		}
		if (pread_exact(kpageflags_fd, &flags, sizeof(flags),
				(off_t)(candidates[index] * sizeof(flags))))
			return -1;
		if (!(flags & (1ULL << KPF_PGTABLE)))
			released[count++] = candidates[index];
	}
	*released_count = count;
	return 0;
}

static int read_page_frames(int pagemap_fd, uintptr_t address,
			    unsigned long page_size, size_t page_count,
			    uint64_t *pfns)
{
	uint64_t entries[REUSE_PROBE_SIZE / 4096];
	uint64_t page_index;
	size_t total_bytes;
	off_t offset;

	if (!page_size || !page_count ||
	    page_count > sizeof(entries) / sizeof(entries[0]) ||
	    address % page_size) {
		errno = EINVAL;
		return -1;
	}
	total_bytes = page_count * sizeof(entries[0]);
	page_index = address / page_size;
	if (page_index > (INT64_MAX - total_bytes) / sizeof(entries[0])) {
		errno = EOVERFLOW;
		return -1;
	}
	offset = (off_t)(page_index * sizeof(entries[0]));
	if (pread_exact(pagemap_fd, entries, total_bytes, offset))
		return -1;
	for (size_t index = 0; index < page_count; index++) {
		if (!(entries[index] & PAGEMAP_PRESENT)) {
			errno = ENOENT;
			return -1;
		}
		pfns[index] = entries[index] & PAGEMAP_PFN_MASK;
		/* Linux hides pagemap PFNs unless the caller has CAP_SYS_ADMIN. */
		if (!pfns[index]) {
			errno = EPERM;
			return -1;
		}
	}
	return 0;
}

static int read_page_frame(int pagemap_fd, uintptr_t address,
			   unsigned long page_size, uint64_t *pfn)
{
	return read_page_frames(pagemap_fd, address, page_size, 1, pfn);
}

static int read_process_byte(pid_t pid, uintptr_t address,
			     unsigned char *value)
{
	struct iovec local = {
		.iov_base = value,
		.iov_len = sizeof(*value),
	};
	struct iovec remote = {
		.iov_base = (void *)address,
		.iov_len = sizeof(*value),
	};
	ssize_t bytes = process_vm_readv(pid, &local, 1, &remote, 1, 0);

	if (bytes == sizeof(*value))
		return 0;
	if (bytes >= 0)
		errno = EIO;
	return -1;
}

static int page_is_present(int pagemap_fd, uintptr_t address,
			   unsigned long page_size, bool *present)
{
	uint64_t entry;
	uint64_t page_index;

	if (!page_size || address % page_size) {
		errno = EINVAL;
		return -1;
	}
	page_index = address / page_size;
	if (page_index > INT64_MAX / sizeof(entry)) {
		errno = EOVERFLOW;
		return -1;
	}
	if (pread_exact(pagemap_fd, &entry, sizeof(entry),
			(off_t)(page_index * sizeof(entry))))
		return -1;
	*present = entry & PAGEMAP_PRESENT;
	return 0;
}

static void segv_handler(int signal_number)
{
	(void)signal_number;
	siglongjmp(fault_env, 1);
}

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set);
}

static void *keep_mm_on_target_cpu(void *argument)
{
	int cpu = *(int *)argument;

	if (pin_to_cpu(cpu)) {
		atomic_store(&keeper_error, errno);
		atomic_store(&keeper_ready, 1);
		return NULL;
	}
	if (sched_getcpu() != cpu) {
		atomic_store(&keeper_error, EXDEV);
		atomic_store(&keeper_ready, 1);
		return NULL;
	}
	atomic_store(&keeper_ready, 1);
	while (!atomic_load(&keeper_stop))
		__asm__ __volatile__("pause" ::: "memory");
	return NULL;
}

static void *wake_target_cpu_worker(void *argument)
{
	struct reschedule_worker_arg *worker = argument;
	char token = 0;
	ssize_t count;

	if (pin_to_cpu(worker->cpu))
		return (void *)(uintptr_t)1;
	do {
		count = read(worker->pipe_fd, &token, sizeof(token));
	} while (count < 0 && errno == EINTR);
	return (void *)(uintptr_t)(count == 1 && token == 'r' ? 0 : 1);
}

static int process_running_on_cpu(pid_t pid, int target_cpu)
{
	char path[64];
	char stat[1024];
	char *cursor;
	char *token;
	FILE *file;
	char state = 0;
	int field;
	int cpu = -1;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	file = fopen(path, "r");
	if (!file)
		return -1;
	if (!fgets(stat, sizeof(stat), file))
		goto out;
	/* The comm field is parenthesized and may itself contain spaces. */
	cursor = strrchr(stat, ')');
	if (!cursor)
		goto out;
	token = strtok(cursor + 2, "\n\t ");
	if (!token)
		goto out;
	state = token[0];
	for (field = 3; token && field <= 39; field++) {
		if (field == 39) {
			char *end;

			long value = strtol(token, &end, 10);

			if (*end == '\0')
				cpu = (int)value;
			break;
		}
		token = strtok(NULL, "\n\t ");
	}
out:
	fclose(file);
	return state == 'R' && cpu == target_cpu;
}

static int has_accel_worker_on_cpu(int target_cpu)
{
	char path[96];
	FILE *file;
	long pid;

	if (cli_pid <= 0)
		return 0;
	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", cli_pid,
		 cli_pid);
	file = fopen(path, "r");
	if (!file)
		return 0;
	while (fscanf(file, "%ld", &pid) == 1) {
		if (process_running_on_cpu((pid_t)pid, target_cpu)) {
			fclose(file);
			return 1;
		}
	}
	fclose(file);
	return 0;
}

static void find_cli_status(void)
{
	char path[64];
	char line[512];
	FILE *file;

	if (cli_shared_address)
		return;
	snprintf(path, sizeof(path), "/proc/%d/maps", cli_pid);
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		char permissions[5];
		char device[32];
		char name[128];
		unsigned long start;
		unsigned long end;
		unsigned long offset;
		unsigned long inode;

		if (sscanf(line, "%lx-%lx %4s %lx %31s %lu %127[^\n]",
			   &start, &end, permissions, &offset, device, &inode,
			   name) == 7 && !offset &&
		    end - start == CPU_ACCEL_MAP_SIZE &&
		    strstr(name, "/dev/cpu_accel")) {
			cli_shared_address = start;
			break;
		}
	}
	fclose(file);
}

static int cli_owner_active(void)
{
	struct cpu_accel_shared status;
	struct iovec local = {
		.iov_base = &status,
		.iov_len = offsetof(struct cpu_accel_shared, samples),
	};
	struct iovec remote;

	find_cli_status();
	if (!cli_shared_address)
		return 0;
	remote.iov_base = (void *)cli_shared_address;
	remote.iov_len = local.iov_len;
	if (process_vm_readv(cli_pid, &local, 1, &remote, 1, 0) !=
	    (ssize_t)local.iov_len)
		return 0;
	return status.state == CPU_ACCEL_STATE_RUNNING &&
		status.mode == CPU_ACCEL_MODE_ACCELERATOR &&
		status.user_active_start_ns != 0;
}

static unsigned long long elapsed_ms(const struct timespec *start,
				     const struct timespec *end)
{
	long long ns = (end->tv_sec - start->tv_sec) * 1000000000LL +
		(end->tv_nsec - start->tv_nsec);

	return ns > 0 ? (unsigned long long)ns / 1000000 : 0;
}

static int wait_for_owner(int target_cpu, unsigned int timeout_ms)
{
	struct timespec start;
	struct timespec now;
	const struct timespec pause = { .tv_nsec = 1000000 };

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		int status;

		if (waitpid(cli_pid, &status, WNOHANG) == cli_pid) {
			cli_pid = -1;
			return 0;
		}
		if (cli_owner_active() &&
		    has_accel_worker_on_cpu(target_cpu))
			return 1;
		nanosleep(&pause, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
	} while (elapsed_ms(&start, &now) < timeout_ms);
	return 0;
}

static int wait_for_cli(void)
{
	int status;
	pid_t result;

	do {
		result = waitpid(cli_pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	cli_pid = -1;
	return result < 0 || !WIFEXITED(status) || WEXITSTATUS(status);
}

static void collect_cli_output(void)
{
	ssize_t count;

	if (cli_output_fd < 0)
		return;
	while (cli_output_size < sizeof(cli_output) - 1) {
		count = read(cli_output_fd, cli_output + cli_output_size,
			     sizeof(cli_output) - cli_output_size - 1);
		if (count > 0) {
			cli_output_size += count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		break;
	}
	cli_output[cli_output_size] = '\0';
	close(cli_output_fd);
	cli_output_fd = -1;
}

static bool cli_reclaim_owner_stop_completed(void)
{
	char expected[96];

	snprintf(expected, sizeof(expected), "state=%u mode=%u",
		 CPU_ACCEL_STATE_COMPLETE, CPU_ACCEL_MODE_LINUX);
	if (!strstr(cli_output, expected))
		return false;
	snprintf(expected, sizeof(expected), "backend=%u",
		 CPU_ACCEL_BACKEND_X86_RING3);
	if (!strstr(cli_output, expected))
		return false;
	snprintf(expected, sizeof(expected),
		 "recovery_state=%u recovery_error=%d",
		 CPU_ACCEL_RECOVERY_FAILED, -EALREADY);
	if (!strstr(cli_output, expected))
		return false;
	return strstr(cli_output, "user_escape_count=0") != NULL;
}

static unsigned long long cli_tlb_targets(void)
{
	const char *field = strstr(cli_output, "arch_tlb_shootdown_targets=");
	unsigned long long targets;

	if (!field || sscanf(field, "arch_tlb_shootdown_targets=%llu", &targets) != 1)
		return 0;
	return targets;
}

static unsigned long long cli_reschedule_requests(void)
{
	const char *field = strstr(cli_output, "arch_reschedule_deferred=");
	unsigned long long requests;

	if (!field || sscanf(field, "arch_reschedule_deferred=%llu", &requests) != 1)
		return 0;
	return requests;
}

static int test_reclaim_completion(int target_cpu, int control_cpu,
				   const char *cpu_accelctl,
				   unsigned long page_size,
				   int argc, char **argv,
				   bool mmu_gather_intermediate_only,
				   bool mmu_gather_final_only)
{
	char path[] = "/var/tmp/cpu-accel-reclaim-XXXXXX";
	char cpu_arg[16];
	char *cli_argv[20];
	unsigned char *contents = NULL;
	unsigned char residency;
	const struct timespec retry_pause = { .tv_nsec = 10000000 };
	const struct timespec completion_pause = { .tv_nsec = 1000000 };
	void *allocation;
	void *mapping = MAP_FAILED;
	int file_fd = -1;
	int pipe_fd[2] = { -1, -1 };
	int ret = -1;
	size_t written = 0;
	size_t file_bytes = mmu_gather_intermediate_only ? MMU_GATHER_PROBE_SIZE :
		mmu_gather_final_only ? MMU_GATHER_FINAL_PROBE_SIZE : page_size;
	size_t cli_argc = 0;
	const char *duration_ms = "5000";
	const char *escape_after_ms = (mmu_gather_intermediate_only ||
				       mmu_gather_final_only) ?
		MMU_GATHER_ESCAPE_AFTER_MS : ACCEL_ESCAPE_AFTER_MS;
	unsigned int owner_timeout_ms = (mmu_gather_intermediate_only ||
					 mmu_gather_final_only) ?
		MMU_GATHER_OWNER_TIMEOUT_MS : 3000;

	if (pin_to_cpu(control_cpu)) {
		perror("pin to controller CPU for reclaim probe");
		goto out;
	}
	file_fd = mkstemp(path);
	if (file_fd < 0) {
		perror("mkstemp(reclaim probe)");
		goto out;
	}
	if (mmu_gather_intermediate_only || mmu_gather_final_only) {
		close(file_fd);
		file_fd = open(path, O_RDWR | O_DIRECT | O_CLOEXEC);
		if (file_fd < 0) {
			perror("open direct-I/O gather probe");
			goto out;
		}
	}
	{
		int alloc_error = posix_memalign(&allocation, page_size, file_bytes);

		if (alloc_error) {
			errno = alloc_error;
			perror("allocate reclaim probe page");
			goto out;
		}
		contents = allocation;
	}
	memset(contents, RECLAIM_PAGE_MARKER, file_bytes);
	if (mmu_gather_intermediate_only || mmu_gather_final_only) {
		ssize_t count = write(file_fd, contents, file_bytes);

		if (count != (ssize_t)file_bytes) {
			if (count >= 0)
				errno = EIO;
			perror("direct write reclaim probe file");
			goto out;
		}
		written = file_bytes;
	}
	while (written < file_bytes) {
		ssize_t count = write(file_fd, contents + written,
					      file_bytes - written);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (!count)
				errno = EIO;
			perror("write reclaim probe page");
			goto out;
		}
		written += count;
	}
	if (fsync(file_fd)) {
		perror("fsync reclaim probe page");
		goto out;
	}
	mapping = mmap(NULL, file_bytes, PROT_READ, MAP_SHARED, file_fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap reclaim probe alias");
		goto out;
	}
	if (*(volatile unsigned char *)mapping != RECLAIM_PAGE_MARKER) {
		fprintf(stderr, "reclaim probe file has unexpected contents\n");
		goto out;
	}
	if (mincore(mapping, page_size, &residency)) {
		perror("mincore before reclaim probe");
		goto out;
	}
	if (!(residency & 1)) {
		fprintf(stderr, "reclaim probe page was not resident before pageout\n");
		goto out;
	}
	if (pipe2(pipe_fd, O_CLOEXEC)) {
		perror("pipe2(reclaim probe output)");
		goto out;
	}
	snprintf(cpu_arg, sizeof(cpu_arg), "%d", target_cpu);
	cli_argv[cli_argc++] = (char *)cpu_accelctl;
	cli_argv[cli_argc++] = "run";
	cli_argv[cli_argc++] = "--cpu";
	cli_argv[cli_argc++] = cpu_arg;
	cli_argv[cli_argc++] = "--duration-ms";
	cli_argv[cli_argc++] = (char *)duration_ms;
	cli_argv[cli_argc++] = "--period-us";
	cli_argv[cli_argc++] = "1000";
	if (escape_after_ms) {
		cli_argv[cli_argc++] = "--escape-after-ms";
		cli_argv[cli_argc++] = (char *)escape_after_ms;
		cli_argv[cli_argc++] = "--escape-retries";
		cli_argv[cli_argc++] = ACCEL_ESCAPE_RETRIES;
	}
	cli_argv[cli_argc++] = "--workload";
	cli_argv[cli_argc++] = "user-reclaim";
	cli_argv[cli_argc++] = "--test-reclaim-file";
	cli_argv[cli_argc++] = path;
	for (int index = 3; index < argc; index++) {
		if (!strcmp(argv[index], "--reclaim-only") ||
		    !strcmp(argv[index], "--mmu-gather-only") ||
		    !strcmp(argv[index], "--mmu-gather-final-only"))
			continue;
		cli_argv[cli_argc++] = argv[index];
	}
	cli_argv[cli_argc] = NULL;
	cli_shared_address = 0;
	cli_output_size = 0;
	memset(cli_output, 0, sizeof(cli_output));
	cli_pid = fork();
	if (cli_pid < 0) {
		perror("fork(reclaim probe)");
		goto out;
	}
	if (!cli_pid) {
		close(pipe_fd[0]);
		if (dup2(pipe_fd[1], STDOUT_FILENO) < 0 ||
		    dup2(pipe_fd[1], STDERR_FILENO) < 0)
			_exit(127);
		close(pipe_fd[1]);
		execvp(cli_argv[0], cli_argv);
		perror("exec cpu-accelctl reclaim probe");
		_exit(127);
	}
	close(pipe_fd[1]);
	pipe_fd[1] = -1;
	cli_output_fd = pipe_fd[0];
	pipe_fd[0] = -1;
	if (!wait_for_owner(target_cpu, owner_timeout_ms)) {
		fprintf(stderr, "did not observe a reclaim-test ring-3 owner\n");
		goto wait_cli;
	}
	if (mmu_gather_intermediate_only || mmu_gather_final_only) {
		/*
		 * Hole punching zaps this file mapping through unmap_mapping_range()
		 * and mmu_gather in the active owner's own mm. The owner keeps its
		 * mmap write lock, so this covers the lockless file-invalidation path.
		 * Its private COW fixture leaves one PTE in every PTE table. Punch the
		 * whole file in one operation so this gather removes more than 10,000
		 * populated pages without freeing any of those PTE tables.
		 */
		if (fallocate(file_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
			      0, file_bytes)) {
			perror("fallocate same-mm mmu_gather probe");
			goto wait_cli;
		}
		if (wait_for_cli()) {
			collect_cli_output();
			fprintf(stderr,
				"cpu-accelctl mmu_gather workload failed to exit\n");
			goto out;
		}
		collect_cli_output();
		if (!strstr(cli_output, "state=7 mode=0") ||
		    !strstr(cli_output, "recovery_state=2") ||
		    !strstr(cli_output, "user_escape_count=1") ||
		    !strstr(cli_output, "recovery_attempts=0")) {
			fprintf(stderr,
				"same-mm gather stop/ack failed or recovery was needed\n");
			goto out;
		}
		printf("PASS: %s same-mm gather release acked\n",
		       mmu_gather_intermediate_only ? "intermediate" : "final");
		ret = 0;
		goto out;
	}
	for (unsigned int attempt = 0; attempt < 8 && cli_owner_active(); attempt++) {
		if (madvise(mapping, page_size, MADV_PAGEOUT)) {
			perror("madvise(MADV_PAGEOUT reclaim probe)");
			goto wait_cli;
		}
		if (mincore(mapping, page_size, &residency)) {
			perror("mincore during reclaim probe");
			goto wait_cli;
		}
		if (!(residency & 1))
			break;
		nanosleep(&retry_pause, NULL);
	}
	if (wait_for_cli()) {
		collect_cli_output();
		fprintf(stderr,
			"cpu-accelctl reclaim workload did not report owner exit\n");
		goto out;
	}
	collect_cli_output();
	if (!strstr(cli_output, "user_escape_count=1") &&
	    !cli_reclaim_owner_stop_completed()) {
		fprintf(stderr,
			"pageout did not stop the reclaim-test ring-3 owner\n");
		goto out;
	}
	if (mincore(mapping, page_size, &residency)) {
		perror("mincore before post-exit reclaim probe");
		goto out;
	}
	if ((residency & 1) && madvise(mapping, page_size, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT after owner exit)");
		goto out;
	}
	for (unsigned int attempt = 0; attempt < 1000; attempt++) {
		if (mincore(mapping, page_size, &residency)) {
			perror("mincore after reclaim probe");
			goto out;
		}
		if (!(residency & 1))
			break;
		nanosleep(&completion_pause, NULL);
	}
	if (residency & 1) {
		fprintf(stderr,
			"reclaim-test pageout stopped the owner but left the page resident\n");
		goto out;
	}
	if (*(volatile unsigned char *)mapping != RECLAIM_PAGE_MARKER) {
		fprintf(stderr, "reclaim probe data changed after refault\n");
		goto out;
	}
	printf("PASS: MADV_PAGEOUT reclaimed the shared file page after the "
	       "ring-3 owner acknowledged exit\n");
	ret = 0;
	goto out;

wait_cli:
	if (cli_pid > 0) {
		int status;
		pid_t waited = waitpid(cli_pid, &status, WNOHANG);

		if (waited == cli_pid)
			cli_pid = -1;
		else if (waited == 0 && wait_for_cli())
			fprintf(stderr, "reclaim-test cpu-accelctl failed during cleanup\n");
		collect_cli_output();
	}
out:
	if (cli_pid > 0) {
		if (wait_for_cli())
			fprintf(stderr, "reclaim-test cpu-accelctl failed during cleanup\n");
	}
	collect_cli_output();
	if (ret && cli_output_size)
		fputs(cli_output, stderr);
	if (pipe_fd[0] >= 0)
		close(pipe_fd[0]);
	if (pipe_fd[1] >= 0)
		close(pipe_fd[1]);
	if (file_fd >= 0)
		close(file_fd);
	free(contents);
	if (mapping != MAP_FAILED)
		munmap(mapping, file_bytes);
	if (path[0] && unlink(path) && errno != ENOENT) {
		perror("unlink reclaim probe file");
		ret = -1;
	}
	return ret;
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = segv_handler };
	cpu_set_t allowed;
	struct timespec flush_start;
	struct timespec flush_end;
	struct timespec settle = { .tv_nsec = 5000000 };
	void *mapping;
	void * volatile cow_mapping = MAP_FAILED;
	void *volatile ptable_reuse_mapping = MAP_FAILED;
	void *volatile replacement_mapping = MAP_FAILED;
	uintptr_t mapping_start;
	uintptr_t region_start;
	uintptr_t mapping_end;
	uintptr_t probe;
	uintptr_t pageout_probe;
	uintptr_t reused_page_address = 0;
	uintptr_t cow_address;
	unsigned long page_size;
	bool pageout_present;
	size_t nr_physical_pages = 0;
	size_t ptable_candidate_count = 0;
	size_t released_ptable_count = 0;
	size_t replacement_ptable_count = 0;
	uint64_t ptable_candidates[MAX_PGTABLE_CANDIDATES];
	uint64_t released_ptables[MAX_PGTABLE_CANDIDATES];
	uint64_t new_ptables[MAX_PGTABLE_CANDIDATES];
	uint64_t adjacent_pfns[PTE_TABLE_SIZE / 4096];
	uint64_t original_probe_pfn;
	uint64_t replacement_pfns[REUSE_PROBE_SIZE / 4096];
	uint8_t *volatile ptable_bitmap = NULL;
	char cpu_arg[16];
	int output_pipe[2];
	int reschedule_pipe[2] = { -1, -1 };
	volatile int pagemap_fd = -1;
	volatile int kpageflags_fd = -1;
	int target_cpu;
	int control_cpu = -1;
	int cpu;
	char *cli_argv[17];
	size_t cli_argc = 0;
	int index;
	volatile int keeper_started = 0;
	volatile int reschedule_started = 0;
	volatile int result = EXIT_FAILURE;
	bool ptable_scan_available = false;
	bool ptable_reused_as_table = false;
	bool ptable_reused_as_data = false;
	bool reclaim_only = false;
	bool mmu_gather_intermediate_only = false;
	bool mmu_gather_final_only = false;
	pid_t pid;
	volatile pid_t cow_holder = -1;
	pthread_t keeper_thread;
	pthread_t reschedule_thread;
	struct reschedule_worker_arg reschedule_arg;

	if (argc < 3 || argc > 5) {
		fprintf(stderr, "usage: %s TARGET_CPU CPU_ACCELCTL [OPTIONS]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	for (index = 3; index < argc; index++) {
		if (!strcmp(argv[index], "--reclaim-only")) {
			reclaim_only = true;
			continue;
		}
		if (!strcmp(argv[index], "--mmu-gather-only")) {
			mmu_gather_intermediate_only = true;
			continue;
		}
		if (!strcmp(argv[index], "--mmu-gather-final-only")) {
			mmu_gather_final_only = true;
			continue;
		}
		if (strcmp(argv[index], "--quarantine-irqs")) {
			fprintf(stderr, "unsupported accelerator option: %s\n",
				argv[index]);
			return EXIT_FAILURE;
		}
	}
	sigemptyset(&action.sa_mask);
	target_cpu = atoi(argv[1]);
	if (target_cpu < 0 || target_cpu >= CPU_SETSIZE) {
		fprintf(stderr, "invalid target CPU: %s\n", argv[1]);
		return EXIT_FAILURE;
	}
	if (sched_getaffinity(0, sizeof(allowed), &allowed)) {
		perror("sched_getaffinity");
		return EXIT_FAILURE;
	}
	if (!CPU_ISSET(target_cpu, &allowed)) {
		fprintf(stderr, "target CPU %d is outside this process's CPU mask\n",
			target_cpu);
		return EXIT_FAILURE;
	}
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (cpu != target_cpu && CPU_ISSET(cpu, &allowed)) {
			control_cpu = cpu;
			break;
		}
	}
	if (control_cpu < 0) {
		fprintf(stderr, "need a controller CPU separate from target CPU %d\n",
			target_cpu);
		return EXIT_FAILURE;
	}

	page_size = (unsigned long)sysconf(_SC_PAGESIZE);
	if (page_size != 4096) {
		fprintf(stderr, "expected 4 KiB pages, got %lu\n", page_size);
		return EXIT_FAILURE;
	}
	if (reclaim_only || mmu_gather_intermediate_only ||
	    mmu_gather_final_only) {
		if ((reclaim_only && (mmu_gather_intermediate_only ||
				      mmu_gather_final_only)) ||
		    (mmu_gather_intermediate_only && mmu_gather_final_only)) {
			fprintf(stderr,
				"reclaim and mmu_gather probe options are exclusive\n");
			return EXIT_FAILURE;
		}
		return test_reclaim_completion(target_cpu, control_cpu, argv[2],
					       page_size, argc, argv,
					       mmu_gather_intermediate_only,
					       mmu_gather_final_only) ?
			EXIT_FAILURE : EXIT_SUCCESS;
	}
	mapping = mmap(NULL, PTE_TABLE_SIZE * 4, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return EXIT_FAILURE;
	}
	mapping_start = (uintptr_t)mapping;
	region_start = (mapping_start + PTE_TABLE_SIZE - 1) &
		~(PTE_TABLE_SIZE - 1);
	mapping_end = mapping_start + PTE_TABLE_SIZE * 4;
	if (region_start + PTE_TABLE_SIZE * 2 > mapping_end) {
		fprintf(stderr, "failed to reserve an aligned PTE-sized region\n");
		goto out_mapping;
	}
	if (madvise((void *)region_start, PTE_TABLE_SIZE, MADV_NOHUGEPAGE)) {
		perror("madvise(MADV_NOHUGEPAGE)");
		goto out_mapping;
	}
	probe = region_start + page_size;
	pageout_probe = probe + page_size;
	cow_mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (cow_mapping == MAP_FAILED) {
		perror("mmap(COW probe)");
		goto out_mapping;
	}
	cow_address = (uintptr_t)cow_mapping;
	/* Cache the COW address's writable translation on target_cpu. */
	if (pin_to_cpu(target_cpu)) {
		perror("pin to target CPU for COW probe");
		goto out_mapping;
	}
	write_page(cow_address, COW_SHARED_MARKER);
	if (pin_to_cpu(control_cpu)) {
		perror("pin to controller CPU for COW fork");
		goto out_mapping;
	}
	cow_holder = fork();
	if (cow_holder < 0) {
		perror("fork(COW holder)");
		goto out_mapping;
	}
	if (!cow_holder) {
		if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() == 1)
			_exit(EXIT_FAILURE);
		for (;;)
			pause();
	}
	/* Refresh the read-only PTE on target_cpu after fork write-protects it. */
	if (pin_to_cpu(target_cpu)) {
		perror("pin to target CPU after COW fork");
		goto out_mapping;
	}
	if (read_page(cow_address) != COW_SHARED_MARKER) {
		fprintf(stderr, "COW probe lost its shared-page marker after fork\n");
		goto out_mapping;
	}
	if (pin_to_cpu(control_cpu)) {
		perror("pin to controller CPU after COW fork");
		goto out_mapping;
	}

	/* Keep a real translation in this mm on the accelerator target CPU. */
	if (pin_to_cpu(target_cpu)) {
		perror("pin to target CPU");
		goto out_mapping;
	}
	kpageflags_fd = open("/proc/kpageflags", O_RDONLY | O_CLOEXEC);
	if (kpageflags_fd >= 0) {
		long physical_pages = sysconf(_SC_PHYS_PAGES);

		if (physical_pages > 0) {
			nr_physical_pages = physical_pages;
			ptable_bitmap = calloc((nr_physical_pages + 7) / 8, 1);
		}
		if (ptable_bitmap &&
		    !snapshot_pagetable_flags(kpageflags_fd, nr_physical_pages,
					     ptable_bitmap)) {
			ptable_scan_available = true;
		} else {
			fprintf(stderr,
				"SKIP: /proc/kpageflags cannot snapshot PTE pages\n");
			free(ptable_bitmap);
			ptable_bitmap = NULL;
			close(kpageflags_fd);
			kpageflags_fd = -1;
		}
	} else {
		fprintf(stderr,
			"SKIP: /proc/kpageflags is unavailable for PTE-page tracking\n");
	}
	for (unsigned long offset = 0; offset < PTE_TABLE_SIZE;
	     offset += page_size) {
		if (region_start + offset != pageout_probe)
			touch_page(region_start + offset);
	}
	if (ptable_scan_available &&
	    scan_pagetable_flags(kpageflags_fd, nr_physical_pages,
				 ptable_bitmap, ptable_candidates,
				 MAX_PGTABLE_CANDIDATES,
				 &ptable_candidate_count)) {
		perror("scan newly allocated PTE pages");
		ptable_scan_available = false;
	} else if (ptable_scan_available && !ptable_candidate_count) {
		fprintf(stderr,
			"SKIP: could not identify the probe mapping's PTE page\n");
		ptable_scan_available = false;
	}
	pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
	if (pagemap_fd < 0) {
		perror("open(/proc/self/pagemap)");
		goto out_mapping;
	}
	if (read_page_frame(pagemap_fd, probe, page_size,
			    &original_probe_pfn)) {
		perror("read probe PFN (requires CAP_SYS_ADMIN)");
		goto out_mapping;
	}
	if (pin_to_cpu(control_cpu)) {
		perror("pin to controller CPU");
		goto out_mapping;
	}
	/* MADV_PAGEOUT drains only this CPU's pending LRU additions. */
	touch_page(pageout_probe);
	if (pipe2(reschedule_pipe, O_CLOEXEC)) {
		perror("pipe2(reschedule worker)");
		goto out_mapping;
	}
	int thread_error = pthread_create(&keeper_thread, NULL,
					  keep_mm_on_target_cpu, &target_cpu);
	if (thread_error) {
		errno = thread_error;
		perror("pthread_create(mm keeper)");
		close(reschedule_pipe[0]);
		close(reschedule_pipe[1]);
		reschedule_pipe[0] = -1;
		reschedule_pipe[1] = -1;
		goto out_mapping;
	}
	keeper_started = 1;
	for (unsigned int attempt = 0;
	     attempt < 2000 && !atomic_load(&keeper_ready); attempt++) {
		const struct timespec pause = { .tv_nsec = 1000000 };

		nanosleep(&pause, NULL);
	}
	if (!atomic_load(&keeper_ready) || atomic_load(&keeper_error)) {
		thread_error = atomic_load(&keeper_error);
		errno = thread_error ? thread_error : ETIMEDOUT;
		perror("start target-mm keeper");
		goto out_mapping;
	}
	reschedule_arg.cpu = target_cpu;
	reschedule_arg.pipe_fd = reschedule_pipe[0];
	thread_error = pthread_create(&reschedule_thread, NULL,
				      wake_target_cpu_worker, &reschedule_arg);
	if (thread_error) {
		errno = thread_error;
		perror("pthread_create(reschedule worker)");
		goto out_mapping;
	}
	reschedule_started = 1;

	snprintf(cpu_arg, sizeof(cpu_arg), "%d", target_cpu);
	if (pipe2(output_pipe, O_CLOEXEC)) {
		perror("pipe2");
		goto out_mapping;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(output_pipe[0]);
		close(output_pipe[1]);
		goto out_mapping;
	}
	if (!pid) {
		cli_argv[cli_argc++] = argv[2];
		cli_argv[cli_argc++] = "run";
		cli_argv[cli_argc++] = "--cpu";
		cli_argv[cli_argc++] = cpu_arg;
		cli_argv[cli_argc++] = "--duration-ms";
		cli_argv[cli_argc++] = ACCEL_DURATION_MS;
		cli_argv[cli_argc++] = "--period-us";
		cli_argv[cli_argc++] = "1000";
		cli_argv[cli_argc++] = "--workload";
		cli_argv[cli_argc++] = "user-oslat";
		cli_argv[cli_argc++] = "--escape-after-ms";
		cli_argv[cli_argc++] = ACCEL_ESCAPE_AFTER_MS;
		cli_argv[cli_argc++] = "--escape-retries";
		cli_argv[cli_argc++] = ACCEL_ESCAPE_RETRIES;
		for (index = 3; index < argc; index++)
			cli_argv[cli_argc++] = argv[index];
		cli_argv[cli_argc] = NULL;
		close(output_pipe[0]);
		if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
		    dup2(output_pipe[1], STDERR_FILENO) < 0)
			_exit(127);
		close(output_pipe[1]);
		execvp(cli_argv[0], cli_argv);
		perror("exec cpu-accelctl");
		_exit(127);
	}
	cli_pid = pid;
	close(output_pipe[1]);
	cli_output_fd = output_pipe[0];
	if (!wait_for_owner(target_cpu, 2000)) {
		fprintf(stderr, "did not observe a ring-3 owner on CPU %d\n",
			target_cpu);
		goto out_cli;
	}
	nanosleep(&settle, NULL);
	/*
	 * A write in this mm must take wp_page_copy() while target_cpu runs the
	 * accelerator in another mm. The holder verifies that the old page stays
	 * intact until the COW mapping is switched to its new page.
	 */
	if (!cli_owner_active() || !has_accel_worker_on_cpu(target_cpu)) {
		fprintf(stderr, "COW probe started after the owner exited\n");
		goto out_cli;
	}
	write_page(cow_address, COW_PRIVATE_MARKER);
	unsigned char holder_value;
	if (read_page(cow_address) != COW_PRIVATE_MARKER) {
		fprintf(stderr, "parent did not retain its private COW value\n");
		goto out_cli;
	}
	if (read_process_byte(cow_holder, cow_address, &holder_value)) {
		perror("read COW probe pages");
		goto out_cli;
	}
	if (holder_value != COW_SHARED_MARKER) {
		fprintf(stderr,
			"COW holder saw %#x after parent wrote a private page\n",
			holder_value);
		goto out_cli;
	}
	if (waitpid(cli_pid, NULL, WNOHANG) == cli_pid ||
	    !cli_owner_active() || !has_accel_worker_on_cpu(target_cpu)) {
		fprintf(stderr,
			"COW page fault completed after the owner exited\n");
		goto out_cli;
	}
	printf("PASS: wp_page_copy COW completed while the other-mm ring-3 "
	       "owner remained active\n");
	/* Exercise rmap's task-local batched-unmap TLB flush while owner runs. */
	if (madvise((void *)pageout_probe, page_size, MADV_PAGEOUT)) {
		perror("madvise(MADV_PAGEOUT batch probe)");
		goto out_cli;
	}
	if (page_is_present(pagemap_fd, pageout_probe, page_size,
			    &pageout_present)) {
		perror("read pagemap(MADV_PAGEOUT batch probe)");
		goto out_cli;
	}
	if (!pageout_present) {
		if (waitpid(cli_pid, NULL, WNOHANG) == cli_pid ||
		    !cli_owner_active() || !has_accel_worker_on_cpu(target_cpu)) {
			fprintf(stderr,
				"MADV_PAGEOUT completed after the owner exited\n");
			goto out_cli;
		}
		printf("PASS: MADV_PAGEOUT completed while ring-3 owner remained active\n");
	} else {
		printf("SKIP: MADV_PAGEOUT left the batch probe PTE present\n");
	}

	/* Free a complete PTE table while this mm cannot run on target_cpu. */
	clock_gettime(CLOCK_MONOTONIC, &flush_start);
	if (munmap((void *)region_start, PTE_TABLE_SIZE)) {
		perror("munmap(TLB generation probe)");
		goto out_cli;
	}
	clock_gettime(CLOCK_MONOTONIC, &flush_end);
	flush_ms = elapsed_ms(&flush_start, &flush_end);
	if (waitpid(cli_pid, NULL, WNOHANG) == cli_pid ||
	    !has_accel_worker_on_cpu(target_cpu)) {
		fprintf(stderr,
			"accelerator owner exited before deferred mm flush returned\n");
		goto out_cli;
	}
	/* Keep the old VA reserved while its freed data page is recycled. */
	void *guard = mmap((void *)region_start, PTE_TABLE_SIZE, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
			   -1, 0);
	if (guard == MAP_FAILED) {
		perror("mmap(PROT_NONE reuse guard)");
		goto out_cli;
	}
	if ((uintptr_t)guard != region_start) {
		fprintf(stderr, "mmap reuse guard returned an unexpected address\n");
		munmap(guard, PTE_TABLE_SIZE);
		goto out_cli;
	}
	if (ptable_scan_available) {
		for (unsigned int attempt = 0; attempt < 100; attempt++) {
			if (released_pagetable_pfns(kpageflags_fd,
						    ptable_candidates,
						    ptable_candidate_count,
						    released_ptables,
						    &released_ptable_count)) {
				perror("check for released PTE page");
				ptable_scan_available = false;
				break;
			}
			if (released_ptable_count)
				break;
			const struct timespec delay = { .tv_nsec = 1000000 };

			nanosleep(&delay, NULL);
		}
		if (ptable_scan_available && !released_ptable_count) {
			fprintf(stderr,
				"SKIP: PTE page was not released during the bounded wait\n");
			ptable_scan_available = false;
		}
	}
	/* Reuse the next PMD-sized slot to encourage immediate PTE-page reuse. */
	uintptr_t adjacent_start = region_start + PTE_TABLE_SIZE;
	if (munmap((void *)adjacent_start, PTE_TABLE_SIZE)) {
		perror("munmap(adjacent PTE reuse slot)");
		goto out_cli;
	}
	ptable_reuse_mapping = mmap((void *)adjacent_start, PTE_TABLE_SIZE,
				    PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS |
				    MAP_FIXED_NOREPLACE, -1, 0);
	if (ptable_reuse_mapping == MAP_FAILED) {
		perror("mmap(adjacent PTE reuse probe)");
		goto out_cli;
	}
	if ((uintptr_t)ptable_reuse_mapping != adjacent_start) {
		fprintf(stderr,
			"adjacent PTE probe returned an unexpected address\n");
		goto out_cli;
	}
	if (madvise(ptable_reuse_mapping, PTE_TABLE_SIZE, MADV_NOHUGEPAGE)) {
		perror("madvise(adjacent PTE probe MADV_NOHUGEPAGE)");
		goto out_cli;
	}
	for (unsigned long offset = 0; offset < PTE_TABLE_SIZE;
	     offset += page_size)
		write_page((uintptr_t)ptable_reuse_mapping + offset,
			   REUSED_PAGE_MARKER);
	if (read_page_frames(pagemap_fd, (uintptr_t)ptable_reuse_mapping,
			     page_size, PTE_TABLE_SIZE / page_size,
			     adjacent_pfns)) {
		perror("read adjacent PTE probe PFNs");
		goto out_cli;
	}
	for (unsigned long offset = 0; offset < PTE_TABLE_SIZE;
	     offset += page_size) {
		uint64_t pfn = adjacent_pfns[offset / page_size];

		if (pfn == original_probe_pfn && !reused_page_address)
			reused_page_address =
				(uintptr_t)ptable_reuse_mapping + offset;
		for (size_t index = 0; index < released_ptable_count; index++) {
			if (pfn == released_ptables[index])
				ptable_reused_as_data = true;
		}
	}
	if (ptable_scan_available) {
		if (scan_pagetable_flags(kpageflags_fd, nr_physical_pages,
					 ptable_bitmap, new_ptables,
					 MAX_PGTABLE_CANDIDATES,
					 &replacement_ptable_count)) {
			perror("scan replacement PTE pages");
			ptable_scan_available = false;
		} else {
			for (size_t index = 0; index < released_ptable_count;
			     index++) {
			for (size_t replacement = 0;
			     replacement < replacement_ptable_count;
			     replacement++) {
				if (released_ptables[index] ==
				    new_ptables[replacement])
					ptable_reused_as_table = true;
			}
			}
		}
	}
	replacement_mapping = mmap(NULL, REUSE_PROBE_SIZE,
				   PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (replacement_mapping == MAP_FAILED) {
		perror("mmap(physical page reuse probe)");
		goto out_cli;
	}
	if (madvise(replacement_mapping, REUSE_PROBE_SIZE, MADV_NOHUGEPAGE)) {
		perror("madvise(reuse probe MADV_NOHUGEPAGE)");
		goto out_cli;
	}
	for (unsigned long offset = 0; offset < REUSE_PROBE_SIZE;
	     offset += page_size) {
		uintptr_t address = (uintptr_t)replacement_mapping + offset;

		write_page(address, REUSED_PAGE_MARKER);
	}
	if (ptable_scan_available) {
		if (scan_pagetable_flags(kpageflags_fd, nr_physical_pages,
					 ptable_bitmap, new_ptables,
					 MAX_PGTABLE_CANDIDATES,
					 &replacement_ptable_count)) {
			perror("scan data-reuse probe PTE pages");
			ptable_scan_available = false;
		} else {
			for (size_t index = 0; index < released_ptable_count;
			     index++) {
			for (size_t replacement = 0;
			     replacement < replacement_ptable_count;
			     replacement++) {
				if (released_ptables[index] ==
				    new_ptables[replacement])
					ptable_reused_as_table = true;
			}
			}
		}
	}
	if (read_page_frames(pagemap_fd, (uintptr_t)replacement_mapping,
			     page_size, REUSE_PROBE_SIZE / page_size,
			     replacement_pfns)) {
		perror("read replacement PFNs");
		goto out_cli;
	}
	for (unsigned long offset = 0; offset < REUSE_PROBE_SIZE;
	     offset += page_size) {
		uint64_t pfn = replacement_pfns[offset / page_size];

		if (pfn == original_probe_pfn && !reused_page_address) {
			reused_page_address =
				(uintptr_t)replacement_mapping + offset;
		}
		for (size_t index = 0; index < released_ptable_count; index++) {
			if (pfn == released_ptables[index])
				ptable_reused_as_data = true;
		}
	}
	if (!reused_page_address) {
		fprintf(stderr,
			"freed probe page was not reused within the 16 MiB allocation\n");
		goto out_cli;
	}
	if (read_page(reused_page_address) != REUSED_PAGE_MARKER) {
		fprintf(stderr, "reused physical page marker did not persist\n");
		goto out_cli;
	}
	if (ptable_scan_available && ptable_reused_as_data)
		printf("reused a freed PTE-table page as live data\n");
	else if (ptable_scan_available && ptable_reused_as_table)
		printf("reused a freed PTE-table PFN for a new PTE table\n");
	else if (ptable_scan_available)
		printf("SKIP: PTE-table PFN reuse not observed (%zu released, "
		       "%zu replacement PTE pages)\n",
		       released_ptable_count, replacement_ptable_count);
	if (waitpid(cli_pid, NULL, WNOHANG) == cli_pid ||
	    !cli_owner_active() || !has_accel_worker_on_cpu(target_cpu)) {
		fprintf(stderr,
			"accelerator owner exited before physical page reuse completed\n");
		goto out_cli;
	}
	if (test_table_batch_unmap()) {
		perror("munmap(512-table batch probe)");
		goto out_cli;
	}
	if (waitpid(cli_pid, NULL, WNOHANG) == cli_pid ||
	    !cli_owner_active() || !has_accel_worker_on_cpu(target_cpu)) {
		fprintf(stderr,
			"accelerator owner exited during the table-batch probe\n");
		goto out_cli;
	}
	printf("PASS: 512-table mmu_gather unmap completed while the other-mm "
	       "ring-3 owner remained active\n");
	if (write(reschedule_pipe[1], "r", 1) != 1) {
		perror("wake reschedule worker");
		goto out_cli;
	}
	close(reschedule_pipe[1]);
	reschedule_pipe[1] = -1;

	if (wait_for_cli()) {
		collect_cli_output();
		fputs(cli_output, stderr);
		fprintf(stderr, "cpu-accelctl did not complete successfully\n");
		goto out_mapping;
	}
	collect_cli_output();
	if (!cli_tlb_targets()) {
		fputs(cli_output, stderr);
		fprintf(stderr, "accelerator reported no TLB shootdown targets\n");
		goto out_mapping;
	}
	if (!cli_reschedule_requests()) {
		fputs(cli_output, stderr);
		fprintf(stderr, "reschedule request was not deferred by the owner\n");
		goto out_mapping;
	}
	void *reschedule_status;
	if (pthread_join(reschedule_thread, &reschedule_status) ||
	    reschedule_status) {
		fprintf(stderr, "reschedule worker did not exit successfully\n");
		reschedule_started = 0;
		goto out_mapping;
	}
	reschedule_started = 0;
	atomic_store(&keeper_stop, true);
	if (pthread_join(keeper_thread, NULL)) {
		fprintf(stderr, "target-mm keeper did not exit successfully\n");
		keeper_started = 0;
		goto out_mapping;
	}
	keeper_started = 0;
	if (pin_to_cpu(target_cpu)) {
		perror("return to target CPU");
		goto out_mapping;
	}
	if (sched_getcpu() != target_cpu) {
		fprintf(stderr, "task did not migrate back to CPU %d\n", target_cpu);
		goto out_mapping;
	}
	if (read_page(cow_address) != COW_PRIVATE_MARKER) {
		fprintf(stderr,
			"COW address-space re-entry observed a stale translation\n");
		goto out_mapping;
	}
	printf("PASS: parent mm re-entry observed the private COW page\n");
	if (sigaction(SIGSEGV, &action, NULL)) {
		perror("sigaction(SIGSEGV)");
		goto out_mapping;
	}
	if (!sigsetjmp(fault_env, 1)) {
		unsigned char value = read_page(probe);

		fprintf(stderr,
			"stale translation remained usable at %p (value %#x)\n",
			(void *)probe, value);
		goto out_mapping;
	}
	printf("reused the unmapped data page while the owner was active\n");
	printf("PASS: CPU %d reconciled the deferred TLB generation on re-entry\n",
	       target_cpu);
	printf("PTE teardown returned in %llums while the owner was active\n",
	       flush_ms);
	printf("accelerator reported %llu TLB shootdown target(s)\n",
	       cli_tlb_targets());
	printf("deferred %llu reschedule request(s) until owner exit\n",
	       cli_reschedule_requests());
	if (test_reclaim_completion(target_cpu, control_cpu, argv[2],
				    page_size, argc, argv, false, false))
		goto out_mapping;
	result = EXIT_SUCCESS;
	goto out_mapping;

out_cli:
	if (cli_pid > 0 && wait_for_cli())
		fprintf(stderr, "cpu-accelctl exited unsuccessfully during cleanup\n");
	collect_cli_output();
	if (cli_output_size)
		fputs(cli_output, stderr);
out_mapping:
	if (pagemap_fd >= 0)
		close(pagemap_fd);
	if (kpageflags_fd >= 0)
		close(kpageflags_fd);
	free(ptable_bitmap);
	if (cow_holder > 0) {
		pid_t waited;

		if (kill(cow_holder, SIGTERM) && errno != ESRCH)
			perror("kill(COW holder)");
		do {
			waited = waitpid(cow_holder, NULL, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited < 0 && errno != ECHILD)
			perror("waitpid(COW holder)");
	}
	if (cow_mapping != MAP_FAILED)
		munmap(cow_mapping, page_size);
	if (ptable_reuse_mapping != MAP_FAILED)
		munmap(ptable_reuse_mapping, PTE_TABLE_SIZE);
	if (replacement_mapping != MAP_FAILED)
		munmap(replacement_mapping, REUSE_PROBE_SIZE);
	if (reschedule_pipe[1] >= 0)
		close(reschedule_pipe[1]);
	if (reschedule_started)
		pthread_join(reschedule_thread, NULL);
	if (keeper_started) {
		atomic_store(&keeper_stop, true);
		pthread_join(keeper_thread, NULL);
	}
	if (reschedule_pipe[0] >= 0)
		close(reschedule_pipe[0]);
	if (region_start > mapping_start)
		munmap((void *)mapping_start, region_start - mapping_start);
	munmap((void *)region_start, PTE_TABLE_SIZE);
	if (region_start + PTE_TABLE_SIZE < mapping_end)
		munmap((void *)(region_start + PTE_TABLE_SIZE),
		       mapping_end - (region_start + PTE_TABLE_SIZE));
	return result;
}
