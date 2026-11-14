// SPDX-License-Identifier: GPL-2.0-only

#include "cpu_accel.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void fail(const char *message)
{
	perror(message);
	exit(1);
}

int main(void)
{
	struct cpu_accel_handle handle;

	if (cpu_accel_open(&handle) < 0)
		fail("open with ABI 23 fallback");
	if (handle.shared->abi_version != CPU_ACCEL_ABI_MIN_VERSION) {
		fprintf(stderr, "SDK fallback selected ABI %u, expected %u\n",
			handle.shared->abi_version, CPU_ACCEL_ABI_MIN_VERSION);
		cpu_accel_close(&handle);
		return 1;
	}
	cpu_accel_close(&handle);
	return 0;
}
