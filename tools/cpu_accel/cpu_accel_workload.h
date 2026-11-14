/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPU_ACCEL_WORKLOAD_H
#define CPU_ACCEL_WORKLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_ACCEL_WORKLOAD_MAGIC	0x43505541UL
#define CPU_ACCEL_WORKLOAD_ABI_VERSION	2U
#define CPU_ACCEL_WORKLOAD_ROLE_TELEMETRY	1U
#define CPU_ACCEL_WORKLOAD_ROLE_BUFFER	2U
#define CPU_ACCEL_WORKLOAD_MAX_SAMPLES	2048U
#define CPU_ACCEL_WORKLOAD_LATENESS_BINS	64U
#define CPU_ACCEL_WORKLOAD_LINEAR_BINS	2048U
#define CPU_ACCEL_WORKLOAD_RING_MAX_ENTRIES	4096U

struct cpu_accel_workload_sample {
	uint64_t timestamp_ns;
	int64_t lateness_ns;
};

struct cpu_accel_workload_region {
	uint32_t magic;
	uint32_t abi_version;
	uint32_t header_size;
	uint32_t region_role;
	uint64_t region_size;
	uint64_t region_epoch;
	uint64_t mmap_token;
	uint64_t completion_sequence;
	uint64_t samples_offset;
	uint64_t lateness_hist_offset;
	uint64_t linear_hist_offset;
	uint64_t payload_offset;
	uint64_t samples_produced;
	uint64_t samples_valid;
	uint64_t max_lateness_ns;
	uint64_t min_lateness_ns;
	uint64_t last_lateness_ns;
	uint64_t sum_lateness_ns;
	uint64_t work_iterations;
	uint64_t work_bytes;
	uint64_t command_sequence;
	uint64_t result_sequence;
	uint64_t result_consumed;
	uint64_t command_overflow;
	uint64_t result_overflow;
	uint64_t manager_ready;
	uint64_t ring_entries;
	uint64_t ring_block_bytes;
	uint64_t queue_enqueued;
	uint64_t queue_dequeued;
	uint64_t queue_full_events;
	uint64_t queue_empty_events;
	uint64_t queue_max_backlog;
	uint64_t queue_checksum;
	uint8_t payload[];
};

#define CPU_ACCEL_WORKLOAD_HEADER_BYTES	offsetof( \
	struct cpu_accel_workload_region, payload)

static inline bool cpu_accel_workload_header_valid(
	const struct cpu_accel_workload_region *region,
	uint64_t region_size, uint64_t region_epoch, uint64_t mmap_token)
{
	return region && region->magic == CPU_ACCEL_WORKLOAD_MAGIC &&
		region->abi_version == CPU_ACCEL_WORKLOAD_ABI_VERSION &&
		region->header_size == CPU_ACCEL_WORKLOAD_HEADER_BYTES &&
		region->region_size == region_size &&
		region->region_epoch == region_epoch &&
		region->mmap_token == mmap_token;
}

#endif
