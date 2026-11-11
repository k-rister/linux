# CPU accelerator Phase 9 UAPI/SDK implementation

Date: 2026-11-11
Branch: `codex/accelerator-oslat`
ABI: 24 (minimum supported ABI: 23)

## Summary

ABI 24 adds explicit version negotiation, separates architecture telemetry and
x86 image admission into extension ioctls, enforces `CAP_SYS_ADMIN`, and adds
terminal-state `poll()` support. ABI 23 remains the compatibility floor.

The control mapping layout is unchanged between ABI 23 and ABI 24. ABI 24 is
therefore a control-plane extension, not a shared-memory layout replacement.

## ABI negotiation

`CPU_ACCEL_IOC_GET_ABI_INFO` returns:

* `current_abi`
* `min_abi`
* `max_abi`
* `selected_abi`

`CPU_ACCEL_IOC_SET_ABI_VERSION` accepts a requested ABI in the supported range
and returns the selected ABI. The default selected ABI is 23 so legacy userspace
keeps working before negotiation.

The SDK requests ABI 24 by default. Set `CPU_ACCEL_ABI_VERSION=23` or
`CPU_ACCEL_ABI_VERSION=24` to force a specific version for testing.

## Architecture extensions

`CPU_ACCEL_IOC_GET_ARCH_TELEMETRY` returns the architecture counters without
requiring new userspace to interpret the legacy shared-mapping fields.

`CPU_ACCEL_IOC_X86_IMAGE_ADMISSION` accepts x86 image admission metadata and
returns an opaque image handle. ABI 23’s legacy image fields remain available
for compatibility.

## Security and polling

`cpu_accel_open()` now requires `CAP_SYS_ADMIN`.

`cpu_accel_fops.poll()` reports `EPOLLIN | EPOLLRDNORM` when the shared state
is terminal. `cpu_accel_wait()` uses `poll()` while preserving the shared-memory
state as the source of truth.

## Freeze gate

Run:

```sh
make -C tools/cpu_accel test-phase9-freeze-gate
```

The gate:

1. Runs the ABI contract test.
2. Runs the UAPI lifecycle test.
3. Runs `user-oslat` on ABI 23.
4. Runs `user-oslat` on ABI 24.
5. Compares p99, p999, and maximum latency against the ABI 23 baseline.

The default target is CPU 5 for the current Simon tuned `cpu-partitioning`
layout, which reserves CPUs 0-3 for housekeeping. Set
`CPU_ACCEL_FREEZE_GATE_CPU` to override the target on a different host.

A run fails if ABI 24 exceeds twice the ABI 23 baseline for any of those three
metrics, or if either run reports:

* a non-terminal state,
* non-Linux final mode,
* wrong backend or workload,
* missing samples or histogram data,
* context switches,
* migration,
* user escapes.

## Test coverage

`tools/cpu_accel/test-uapi-contract.c` covers:

* ABI 23 and ABI 24 negotiation,
* invalid ABI requests and ABI reserved words,
* unknown configuration flags,
* configuration reserved words,
* unknown region permissions,
* region reserved words,
* unknown ioctl commands,
* architecture telemetry,
* x86 image admission and rejection of invalid image metadata.

`tools/cpu_accel/test-uapi-lifecycle.c` covers:

* manager crash during an active run,
* descriptor passing over a Unix socket,
* accidental `execve()` descriptor closure,
* deliberate `execve()` descriptor retention,
* fork without exec.
