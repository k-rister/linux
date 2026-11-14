# Phase 10 blast-radius driver-specific audit

## Scope

This note records the semantic review of all 27 driver-class rows in
`Documentation/accel/phase10-blast-radius-semantic-inventory.tsv`:

- `drivers/Kconfig`
- `drivers/Makefile`
- `drivers/auxdisplay/ht16k33.c`
- `drivers/auxdisplay/max6959.c`
- `drivers/auxdisplay/seg-led-gpio.c`
- `drivers/char/hw_random/n2-drv.c`
- `drivers/extcon/extcon-ptn5150.c`
- `drivers/firmware/efi/efi.c`
- `drivers/greybus/interface.c`
- `drivers/hsi/clients/ssi_protocol.c`
- `drivers/infiniband/hw/ionic/ionic_admin.c`
- `drivers/input/misc/palmas-pwrbutton.c`
- `drivers/input/misc/wm831x-on.c`
- `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-test.c`
- `drivers/leds/leds-cr0014114.c`
- `drivers/net/dsa/mv88e6xxx/ptp.c`
- `drivers/net/ethernet/cavium/liquidio/octeon_main.h`
- `drivers/net/ethernet/marvell/octeontx2/af/mcs.h`
- `drivers/net/ethernet/marvell/octeontx2/af/rvu.h`
- `drivers/net/ethernet/ti/icssg/icss_iep.c`
- `drivers/net/phy/bcm-phy-ptp.c`
- `drivers/platform/x86/intel/ifs/load.c`
- `drivers/platform/x86/intel/ifs/runtest.c`
- `drivers/scsi/qla2xxx/qla_gbl.h`
- `drivers/scsi/qla2xxx/qla_init.c`
- `drivers/scsi/qla2xxx/qla_mid.c`
- `drivers/target/target_core_tmr.c`

The review baseline is `40288c9206c1` on
`codex/accelerator-blast-radius-v2`. None of these rows is a mechanical
wrapper-call-site conversion; each required source review against its
producer, teardown, lock, error, or accelerator-ownership boundary.

## Method

The review compared the complete baseline-to-current diff for each file with:

- The accelerator admission, maintenance-window, and per-CPU sync-wait
  contracts in `arch/x86/kernel/accel.c`.
- The retained workqueue wrapper contracts in `kernel/workqueue.c`.
- Each driver's producer lifecycle, IRQ release order, device reference
  rules, self-requeue behavior, and lock-taking work callbacks.
- The supporting call-site files that implement the semantics implied by
  driver headers, especially LiquidIO PF/VF teardown and Marvell MCS
  interrupt publication.

The review classified the changes as build plumbing, producer teardown,
lock-order repair, accelerator rendezvous coordination, RCU lifetime repair,
or additive structure initialization.

## Per-file verdicts

| File | Verdict | Review result |
|---|---|---|
| `drivers/Kconfig` | pass | The added `source` entry exposes the accelerator Kconfig only; it changes no existing driver configuration. |
| `drivers/Makefile` | pass | The accelerator directory is built only under `CONFIG_CPU_ACCEL`, which selects `CONFIG_CPU_ACCEL_CORE`. |
| `drivers/auxdisplay/ht16k33.c` | pass | Matrix and segment producers are unregistered before `disable_delayed_work_sync()`. The self-requeuing matrix refresh cannot restart, and framebuffer resources are freed only after the work is disabled. |
| `drivers/auxdisplay/max6959.c` | pass | `linedisp_unregister()` stops timer and sysfs producers before delayed-work cancellation, preventing a post-cancel requeue. |
| `drivers/auxdisplay/seg-led-gpio.c` | pass | As with MAX6959, linedisp producers are stopped before the delayed work is canceled. |
| `drivers/char/hw_random/n2-drv.c` | pass | Removal sets the shutdown flag, unregisters the hwrng producer, and then synchronously cancels delayed work. The shutdown flag prevents an error-path self-requeue. |
| `drivers/extcon/extcon-ptn5150.c` | pass | The devm action that cancels IRQ work is registered before the IRQ. Normal devm release therefore frees the IRQ before the action waits for work, and no handler can requeue during cancellation. |
| `drivers/firmware/efi/efi.c` | pass | The static `efi_mm` initializer supplies the new execution-owner lock and list. The change is additive and does not alter EFI runtime-services behavior. |
| `drivers/greybus/interface.c` | pass | Removing cancellation from release is required for reference correctness: mode-switch work holds a device reference and may perform the final `put_device()`. Cancellation from release would dereference freed storage. |
| `drivers/hsi/clients/ssi_protocol.c` | pass | Reset disables the netdev TX path, drops `ssi->lock` before `cancel_work_sync()`, and reacquires the lock before clearing queues. The xmit producer can no longer queue while cancellation is in progress. |
| `drivers/infiniband/hw/ionic/ionic_admin.c` | pass | Both destroy paths disable the event queue and free its IRQ before `cancel_work_sync()`. The work callback checks `eq->enable` before requeueing, so cancellation cannot race a new producer. |
| `drivers/input/misc/palmas-pwrbutton.c` | pass | Error and remove paths release the IRQ before canceling the delayed work it queues. |
| `drivers/input/misc/wm831x-on.c` | pass | Probe validates the raw platform IRQ, maps it through `wm831x_irq()`, and stores the mapped IRQ for removal. Removal frees that IRQ before canceling its delayed work. |
| `drivers/iommu/arm/arm-smmu-v3/arm-smmu-v3-test.c` | pass | The test MM initializer supplies the new execution-owner fields so the fixture remains layout-complete. |
| `drivers/leds/leds-cr0014114.c` | pass | `disable_delayed_work_sync()` is required because recount work requeues itself after each firmware workaround cycle. |
| `drivers/net/dsa/mv88e6xxx/ptp.c` | pass | The TAI work takes the switch register mutex, so cancellation is moved outside that mutex after register state is updated. |
| `drivers/net/ethernet/cavium/liquidio/octeon_main.h` | pass | The declared NIC_INFO lock is now initialized and used by both PF and VF paths. Producers hold it while resolving the interface and queueing link work; teardown clears interface identity under it before draining work and freeing the netdev. |
| `drivers/net/ethernet/marvell/octeontx2/af/mcs.h` | pass | The RVU back-pointer is typed `__rcu`, and the interrupt entry path now uses an explicit RCU-sched critical section. Publication is delayed until the interrupt workqueue exists. |
| `drivers/net/ethernet/marvell/octeontx2/af/rvu.h` | pass | The added `rvu_rep_exit()` declaration is an additive prototype for the representor teardown implementation; it introduces no independent runtime semantics. |
| `drivers/net/ethernet/ti/icssg/icss_iep.c` | pass | PPS/PEROUT disable updates hardware under the PTP mutex, releases the mutex, and then synchronously cancels work that also takes that mutex. |
| `drivers/net/phy/bcm-phy-ptp.c` | pass | `pin_mutex` serializes enable and stop transitions. Pin state and hardware bits change under the inner mutex; cancellation happens after marking the pin inactive and dropping that mutex. A new work item is initialized only after the old one is drained. |
| `drivers/platform/x86/intel/ifs/load.c` | pass | Stack authentication work is queued and flushed inside a per-CPU accelerator sync-wait bracket, so an active owner on the selected package CPU is asked to exit rather than leaving the loader blocked. `destroy_work_on_stack()` runs after the flush. |
| `drivers/platform/x86/intel/ifs/runtest.c` | pass | Core-stop testing refuses to start while an accelerator owner is active by taking the nonblocking maintenance window and returning `-EBUSY` on failure. Every exit path releases the window. |
| `drivers/scsi/qla2xxx/qla_gbl.h` | pass | The added declaration exposes the queue-work cancellation helper used by queue-pair teardown. |
| `drivers/scsi/qla2xxx/qla_init.c` | pass | Queue-pair deletion releases the response IRQ before calling the owner-aware work cancellation helper and freeing the request/response queues. |
| `drivers/scsi/qla2xxx/qla_mid.c` | pass | The cancellation helper brackets `cancel_work_sync()` with begin/stop/end over every possible CPU, covering a per-CPU workqueue rescuer that may execute away from its pool CPU. |
| `drivers/target/target_core_tmr.c` | pass | TMR abort and state-list drain flush every per-queue submission work item inside an all-possible-CPU sync-wait bracket. The original flush-before-lock ordering is preserved. |

## Invariants verified

- **State transitions and lifetime:** every synchronous work wait occurs after
  its producer is disabled or after an owner-exit request is registered.
  Self-requeuing work is disabled rather than merely canceled. Stack work is
  destroyed after completion, and device storage is freed only after the final
  producer reference is gone.
- **Lock ordering and deadlock avoidance:** work callbacks that take driver
  locks are canceled only after those locks are released. The LiquidIO
  NIC_INFO lock serializes producer lookup against teardown identity
  publication. The BCM PHY outer pin lock serializes stop/restart transitions
  while cancellation remains outside the inner hardware lock.
- **Error and rollback:** probe error paths release IRQs before waiting for
  their work. IFS returns `-EBUSY` rather than partially entering a core-stop
  maintenance window. Workqueue and RCU publication failures leave the RVU
  pointer unpublished.
- **Accelerator ownership:** per-CPU stack, QLA2xxx, and target work waits use
  the architecture sync-wait bracket. IFS core-stop rendezvous uses the
  nonblocking maintenance window. No audited path assumes that per-CPU work
  can run on an accelerator-owned CPU.
- **RCU lifetime:** the MCS RVU pointer is assigned with `rcu_assign_pointer()`,
  read with `rcu_dereference_sched()` inside an explicit RCU-sched section,
  unpublished before `synchronize_rcu()`, and drained only after that grace
  period.
- **No silent semantic change:** config-disabled accelerator builds leave the
  added Kconfig/Makefile entries inert, and architecture sync-wait helpers are
  no-ops when the accelerator core is disabled.

## Findings

Three defects were found during this audit and fixed in the same change:

1. **LiquidIO producer/teardown mismatch:** `octeon_main.h` declared
   `link_status_lock`, but neither PF nor VF code used it. PF/VF cleanup could
   free a netdev before the shared NIC_INFO callback stopped dereferencing it.
   Both paths now initialize and use the lock; teardown clears the interface
   pointer and GMX port under the lock before draining link work.
2. **Premature MCS RVU publication:** the RVU pointer was published before
   `rvu->mcs_intr_wq` was allocated. Interrupt entry now holds an explicit
   RCU-sched critical section, tolerates an unpublished pointer, and RVU is
   published only after the workqueue is ready.
3. **Unbracketed IFS stack-work wait:** generation-zero image authentication
   scheduled stack work on a per-CPU workqueue and used plain `flush_work()`.
   An accelerator owner on the selected CPU could defer that work indefinitely.
   The queue-and-flush sequence is now bracketed by the architecture sync-wait
   API.

No unresolved driver-specific semantic defect remains in the reviewed
population.

## Evidence

- The complete 27-file baseline-to-current driver diff was reviewed with
  30 lines of context and indexed as ephemeral capture `cap_746`.
- `scripts/checkpatch.pl --strict --no-signoff` reports no errors, warnings,
  or checks for the focused driver-fix diff.
- An x86-64 focused build compiles the LiquidIO PF and VF objects, the MCS
  object, and both Intel IFS objects.
- `git diff --check` passes.
- Sparse is not installed on this host, so sparse validation is
  environment-gated and is not claimed as passing.
- No LiquidIO, Marvell MCS, QLA2xxx, BCM PHY, ICSS IEP, or target hardware
  runtime test was available in this environment. This note therefore claims
  source and compile correctness, not subsystem hardware validation.

## Status

The driver-specific semantic class is **pass** for source review and focused
compilation. Hardware-specific runtime validation remains a separate item in
the Phase 10 review-closure plan.
