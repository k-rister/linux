// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <fcntl.h>
#include <linux/cpu_accel.h>
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
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PTE_TABLE_SIZE (512UL * 4096)
#define REUSE_PROBE_SIZE (16UL * 1024UL * 1024UL)
#define ACCEL_DURATION_MS "2000"
/* Escape a stuck owner so a broken synchronous flush cannot strand the VM. */
#define ACCEL_ESCAPE_AFTER_MS "1000"
#define ACCEL_ESCAPE_RETRIES "1"
#define CLI_OUTPUT_SIZE 4096
#define PAGEMAP_PRESENT (1ULL << 63)
#define PAGEMAP_PFN_MASK ((1ULL << 55) - 1)
#define REUSED_PAGE_MARKER 0xa5
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

		if (waitpid(cli_pid, &status, WNOHANG) == cli_pid)
			return 0;
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

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = segv_handler };
	cpu_set_t allowed;
	struct timespec flush_start;
	struct timespec flush_end;
	struct timespec settle = { .tv_nsec = 5000000 };
	void *mapping;
	void *volatile ptable_reuse_mapping = MAP_FAILED;
	void *volatile replacement_mapping = MAP_FAILED;
	uintptr_t mapping_start;
	uintptr_t region_start;
	uintptr_t mapping_end;
	uintptr_t probe;
	uintptr_t reused_page_address = 0;
	unsigned long page_size;
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
	pid_t pid;
	pthread_t keeper_thread;
	pthread_t reschedule_thread;
	struct reschedule_worker_arg reschedule_arg;

	if (argc < 3 || argc > 5) {
		fprintf(stderr,
			"usage: %s TARGET_CPU CPU_ACCELCTL [ACCELERATOR_OPTION ...]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	for (index = 3; index < argc; index++) {
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
	     offset += page_size)
		touch_page(region_start + offset);
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
