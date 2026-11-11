# CPU accelerator Phase 9 UAPI and SDK audit

Date: 2026-11-10
Branch: `codex/accelerator-oslat`
Audited ABI: 23

## Scope

This audit compares the current control UAPI and companion SDK with the Phase 9
requirement to stabilize the public interface after the Phase 2 through Phase 8
lifecycle and fault-model work. It is a readiness audit, not a declaration that
ABI 23 is frozen.

The authoritative interface is `include/uapi/linux/cpu_accel.h`. The SDK is
`tools/cpu_accel/cpu_accel.h` and `tools/cpu_accel/libcpu_accel.c`. The driver
lifecycle and ioctl implementation are in `drivers/cpu_accel/cpu_accel.c`.

## Result

**ABI 23 is not ready to freeze.**

The current design has valuable prototype properties: it is file-descriptor
based, uses fixed-width UAPI types, validates explicit flags and reserved
fields, exposes runtime capabilities, and has deliberate process teardown
behavior. The control mapping also carries an ABI version, structure size, and
layout fingerprint.

The blocking gaps are equally clear:

* The ioctl argument structures are fixed-size and have no per-object version
  or length field, so the reserved fields are not a real extension mechanism.
* SDK attach uses strict ABI equality and returns `EPROTO`; it does not
  negotiate with older userspace or newer kernels.
* User-image admission exposes numeric worker virtual addresses and x86-specific
  backend and telemetry values in the shared control ABI.
* The device is a single global domain rather than a scalable domain object.
* There is no `poll()` file operation; the SDK waits by sleeping and re-reading
  shared state.
* Permission policy is currently implicit in the `0600` misc-device mode rather
  than an explicit capability policy.
* Process-exit cleanup is implemented, but control-process crash, descriptor
  passing, and accidental `execve()` leakage are not covered by focused tests.
* Compatibility testing consists of one synthetic old-version rejection case,
  not old userspace against a newer kernel or the reverse.

## Requirement matrix

| Phase 9 requirement | Current status | Authoritative evidence | Gap | Recommended Phase 9 action |
|---|---|---|---|---|
| File-descriptor-based domain object | Implemented as a single global device. `open()` stores per-file private data, but every descriptor refers to the one `cpu_accel` singleton and only one descriptor may be open. | `cpu_accel_open()` and the global `opened` gate in `drivers/cpu_accel/cpu_accel.c`; SDK `cpu_accel_open()` and `cpu_accel_attach_fd()` in `tools/cpu_accel/libcpu_accel.c`. | The object is an administrative singleton, not a reusable multi-domain model. | Preserve the descriptor model, but decide before freeze whether the public object is one singleton administrative domain or multiple domains created through a device or subsystem file. |
| Sized, extensible structures | Partially implemented. `cpu_accel_shared` carries `abi_version`, `struct_size`, and a layout fingerprint. Ioctl arguments are sized by `_IOR`/`_IOWR` and contain reserved fields. | `struct cpu_accel_shared` and `CPU_ACCEL_LAYOUT_FINGERPRINT()` in `include/uapi/linux/cpu_accel.h`; ioctl definitions at the end of that header. | The `_IOC` size is fixed per command. Reserved words do not define tail growth, ignored bits, or a way for an old kernel and new userspace to agree on an extended object. | Add an explicit extension policy: either versioned commands with a user-supplied object length, or immutable command layouts plus separate extension objects. Test every extension rule in both directions. |
| Explicit flags and unknown-flag rejection | Implemented for the main control inputs. Configuration rejects unknown flags and a nonzero reserved word. Region create and reference ioctls reject nonzero reserved words; region permissions are validated against supported masks. Unknown ioctl numbers return `ENOTTY`. | `CPU_ACCEL_IOC_CONFIG` validation in `cpu_accel_ioctl()`; `cpu_accel_region_create_locked()`, `cpu_accel_region_attach_locked()`, and related functions in `drivers/cpu_accel/cpu_accel.c`. | Existing tests cover invalid region permissions, but not unknown configuration flags, the configuration reserved word, or stale region-reference reserved words. | Add focused negative tests for every flags and reserved field before changing the validation logic. Keep zero reserved words mandatory for stable commands. |
| Fixed-width types and no persistent userspace pointers | Partially implemented. All UAPI fields use `__u32`, `__s32`, or `__u64`; there are no C pointer members. `mmap_token` is opaque. | UAPI structure definitions in `include/uapi/linux/cpu_accel.h`. | `cpu_accel_config` carries numeric worker virtual addresses such as entry, stack, image, argument, escape, and reclaim-page addresses. They are address-space-specific values, not portable handles. | Replace the image admission fields with opaque registered objects, or isolate them in an explicitly architecture-specific extension that is not part of the frozen control ABI. |
| Capability and permission checks | Partially implemented. The device is registered with mode `0600`. Region creation accounts locked memory against the companion `mm`. Runtime region limits and permission masks are queryable. | `misc.mode = 0600` in `cpu_accel_init()`; `account_locked_vm()` in region creation; `CPU_ACCEL_IOC_GET_CAPABILITIES` in `drivers/cpu_accel/cpu_accel.c`. | There is no explicit `capable()` policy. Security depends on device-node ownership and mode, which can differ by distribution or administrative setup. | Define the required credential and LSM policy. If `CAP_SYS_ADMIN` is required, enforce it explicitly and document why. Separately define whether a delegated, non-root control process is supported. |
| Pollable Linux-side state changes | Not implemented. The SDK polls the shared state every millisecond and sleeps with `nanosleep()`. The driver has no `.poll` file operation. | `cpu_accel_wait()` in `tools/cpu_accel/libcpu_accel.c`; `cpu_accel_fops` in `drivers/cpu_accel/cpu_accel.c`. | It is not documented whether Linux-side wakeup is intentionally excluded or merely missing. The hot path correctly does not depend on a wakeup. | Make the contract explicit. Either add `.poll` with wakeups on terminal state transitions and document it as a control-plane optimization, or document shared-memory polling as the only supported wait model and keep the SDK polling helper. |
| Shared-memory ABI version negotiation | Not implemented. SDK attach requires exact equality of ABI version, structure size, and layout fingerprint. Workload-region headers likewise use an exact version check. | `cpu_accel_attach_fd()` in `tools/cpu_accel/libcpu_accel.c`; `cpu_accel_reset_shared()` in `drivers/cpu_accel/cpu_accel.c`; `cpu_accel_workload_header_valid()` in `tools/cpu_accel/cpu_accel_workload.h`. | Strict rejection is safe for an experimental prototype but cannot satisfy the compatibility requirement. It provides no path for old userspace on a newer kernel or new userspace on an older kernel. | Define a minimum-supported ABI range and negotiation result. Add a capability or attach response that reports supported and selected versions, and define behavior when the shared object is larger or smaller than the caller's compile-time layout. |
| Defined process exit, exec, fork, descriptor passing, and control crash behavior | Partially implemented and partially tested. Device release requests stop, exits the lifecycle thread when needed, releases resources, drops the companion `mm` reference, and clears the singleton-open gate. SDK opens use `O_CLOEXEC`; the worker deliberately clears `FD_CLOEXEC`, closes unrelated descriptors, forks, and execs. Region tests fork an owner and verify role-specific mappings. A user-worker death test reconciles to `ERROR`/Linux. | `cpu_accel_release()` in `drivers/cpu_accel/cpu_accel.c`; worker descriptor handling in `tools/cpu_accel/cpu-accel-testctl.c`; process-mm handoff in `Documentation/accel/cpu-accel-memory.rst`; fork tests in `tools/cpu_accel/test-region-lifecycle.c`; `tools/cpu_accel/test-user-death-owner.sh`. | There is no focused test for killing the control process while the domain is active, passing the descriptor through a Unix socket, or accidentally retaining the descriptor across `execve()`. Fork without exec is exercised only for region ownership, not as a complete control-plane lifecycle case. | Add lifecycle selftests for manager SIGKILL during `READY`/`RUNNING`, deliberate descriptor passing, accidental exec with `FD_CLOEXEC`, and fork without exec. Assert final CPU mode, region state, resource accounting, and singleton recovery. |
| Compatibility expectations and bidirectional selftests | Not implemented. `test_abi_mismatch()` constructs an old-version mapping on a `memfd` and expects `EPROTO`; it does not run an old SDK against a new kernel or a new SDK against an old kernel. | `test_abi_mismatch()` in `tools/cpu_accel/test-region-lifecycle.c`. | There are no preserved compatibility binaries, old UAPI snapshots, compatibility ioctls, or a matrix of accepted version/layout combinations. | Build compatibility fixtures before freezing. Preserve at least one pre-freeze userspace binary and UAPI snapshot, then test old userspace/new kernel and new userspace/old kernel. Include the shared mapping, workload-region header, and every stable ioctl. |
| Final chosen public interface | Not selected. The current header is `include/uapi/linux/cpu_accel.h`, named for the prototype. | UAPI header and `Documentation/accel/cpu-accel.rst`. | The plan permits `include/uapi/linux/accelerator.h` or another final name, but the current name and architecture-specific contents are prototype artifacts. | Choose the final name and object model only after the extension, permission, and compatibility policies are defined. Do not rename as a substitute for designing the ABI. |
| Stable control library | Experimental implementation exists. It wraps open, attach, configure, start, stop, exit, reset, capabilities, regions, mapping, and wait. | `tools/cpu_accel/cpu_accel.h` and `tools/cpu_accel/libcpu_accel.c`. | There is no library version, symbol-compatibility policy, error-code contract, or compatibility test suite. | Stabilize only the operations proven by Phase 8. Publish a symbol/version policy and error contract before calling the library stable. |
| ABI documentation and administrative tooling | Prototype documentation and test tooling exist. `cpu-accel.rst` describes operations and commands; `cpu-accel-testctl` exercises the interface. | `Documentation/accel/cpu-accel.rst`; `tools/cpu_accel/cpu-accel-testctl.c`. | The docs do not define stable ABI rules, extension policy, compatibility matrix, permissions, or descriptor lifecycle. The CLI is a test controller, not a stable administrative interface. | Convert the operational semantics into ABI documentation with normative state and error tables. Keep `cpu-accel-testctl` as a test tool unless a separate stable administrative command is intentionally published. |

## Detailed findings

### Extension policy is the first freeze blocker

`struct cpu_accel_shared` is guarded well for the current prototype: its ABI
version, exact structure size, and sentinel layout fingerprint all reject an
incompatible mapping. That is the right fail-stop behavior while the layout is
changing.

The other ioctl structures do not have an equivalent contract. Their sizes are
encoded in `_IOC` command numbers, and the `reserved` words are only zero checks.
They do not tell the kernel how many bytes the caller understands or give a new
caller a way to discover which optional fields an older kernel supports. Using
those words as future flags would also be ambiguous because the current contract
requires every reserved word to be zero.

A frozen ABI needs one of two explicit designs:

1. Immutable command layouts, with all growth represented by separate
   capability and extension objects selected through new commands.
2. Explicitly versioned or length-prefixed objects, where userspace supplies the
   understood length and the kernel returns the selected version and length.

The first option is less invasive and is a better fit for the existing explicit
ioctl commands. Either way, the policy must be tested in both compatibility
directions before freeze.

### Raw worker addresses and architecture fields must not leak into the freeze

The UAPI has no C pointer members, which satisfies the basic fixed-width rule.
It does not yet satisfy the stronger portability goal. The user workload
configuration includes numeric worker virtual addresses for entry, stack, image,
argument, escape, and the reclaim test page. Those values are meaningful only
inside the admitted worker address space and are coupled to the current
process-`mm` prototype.

The shared structure also publishes x86 backends and architecture counters.
Those are useful telemetry, but they are not neutral control-plane semantics.
The final ABI should separate architecture-neutral lifecycle and capability
objects from architecture-specific admission and telemetry extensions.

### Permission policy needs to be explicit

Mode `0600` makes the current prototype root-only on a normal installation and
is appropriate for a device that can remove CPUs from normal Linux scheduling.
It is not itself an ABI guarantee: device-node ownership and mode can be changed
without changing the kernel, and the driver performs no explicit capability
check.

The stable interface should state the required credential, likely
`CAP_SYS_ADMIN` unless a narrower, well-audited capability is chosen. If
delegated operation is intended, the permission model must also cover CPU
selection, locked-memory accounting, cgroup placement, and access to worker
images and regions.

### Process lifecycle is stronger than its test coverage

The driver's release path already contains the important fail-safe: if a
descriptor closes while the domain is `READY` or `RUNNING`, it requests stop;
if a lifecycle thread remains, it exits the accelerator CPU; it then restores
quarantine state, releases regions where possible, drops the companion `mm`
reference, and allows a subsequent open. Worker death has a focused runtime
test.

The missing evidence is control-process failure and descriptor movement. The
SDK's `O_CLOEXEC` default prevents accidental descriptor inheritance, and the
worker deliberately clears that flag for its own exec. This is good design, but
the stable contract needs direct tests for:

* manager SIGKILL during `READY` and `RUNNING`;
* descriptor passing through a Unix socket;
* `execve()` retaining and not retaining the descriptor;
* fork without exec and the resulting manager/owner mapping roles;
* a new open after forced release, including region and locked-memory cleanup.

### Polling must be a documented decision, not an accident

The accelerator hot path must not depend on Linux wakeups. That requirement is
not a reason to leave the Linux-side wait model unspecified. The SDK currently
sleeps for one millisecond and reads the shared state. That is simple and
adequate for the prototype, but a stable control library should state whether
`poll()` is supported.

If `poll()` is added, it should be a control-plane optimization only: report
terminal state transitions and descriptor hangup, never a prerequisite for
accelerator execution. If it is not added, the shared-memory polling helper and
its timeout semantics should be documented as the supported SDK behavior.

## Recommended order

1. Add negative compatibility tests for unknown config flags, config reserved
   words, region reserved words, unknown ioctl commands, and old/new control
   mapping layouts. These tests should run against the current ABI before any
   extension mechanism is added.
2. Define the extension policy. Prefer immutable command layouts plus separate
   capability and extension objects unless a length-prefixed design is chosen
   deliberately.
3. Define a minimum-supported control ABI and shared-workload ABI range,
   including exact behavior for smaller and larger objects. Implement attach
   negotiation only after that contract is written.
4. Add process-lifecycle tests for control crash, descriptor passing, exec
   behavior, and fork without exec. Require CPU recovery, region state, and
   resource accounting to be verified after each case.
5. Decide the poll contract: implement terminal-state `poll()` or explicitly
   document shared-memory polling as the control-plane wait model.
6. Define the privilege model and enforce it in the driver rather than relying
   solely on misc-device mode.
7. Move worker image addresses and architecture-specific telemetry behind
   opaque or explicitly extension-scoped objects.
8. Choose the final UAPI name and library symbol policy, write normative ABI
   documentation, and only then run the bidirectional compatibility matrix and
   declare a freeze candidate.

## Freeze verdict

The Phase 9 gate is not met. The current UAPI can express the proven one-CPU
lifecycle and recovery operations, but it also exposes prototype details that
would become permanent ABI accidents if frozen now. The most important next
work is extension policy, version negotiation, explicit permissions, and the
missing process-lifecycle compatibility tests. ABI 23 should remain explicitly
experimental until those items are complete.
