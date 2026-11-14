# Phase 10 blast-radius call-site audit

## Scope

This note records the review of all 218 retained files that call the three
owner-aware workqueue wrappers:

- `cpu_accel_flush_workqueue()`
- `cpu_accel_drain_workqueue()`
- `cpu_accel_destroy_workqueue()`

The review baseline is commit `40288c9206c1`. The live population contains
218 files and 526 wrapper calls: 112 flush, 28 drain, and 386 destroy. The
population matches `Documentation/accel/phase10-blast-radius-call-site-inventory.tsv`.

## Method

The audit used three independent checks:

1. **Population verification:** the live call-site set was regenerated from the
   current tree and compared with the committed inventory.
2. **Normalization comparison:** each current file was compared with its
   baseline after replacing the three wrapper names with their ordinary
   workqueue counterparts. This isolated files whose only change was the
   wrapper rename from files with additional semantic edits.
3. **Line-level review:** the 34 files that remained different after
   normalization were reviewed individually against the baseline.

The normalization comparison proved that 184 files contain only the mechanical
wrapper-name substitution. Their call context, control flow, error handling,
and ordering are unchanged from baseline. The wrapper API audit in
`Documentation/accel/phase10-blast-radius-wrapper-api-audit.md` therefore
covers their only semantic difference.

## Substantive review

The remaining 34 files contain additional teardown, race, or ordering fixes.
Each was reviewed for the call-site requirements in
`Documentation/accel/phase10-blast-radius-review-close-plan.md`:

| File | Verdict | Review result |
|---|---|---|
| `drivers/bus/mhi/ep/main.c` | pass | Adds `unregistering` and `powering_down` state, synchronizes the IRQ, and cancels reset work before workqueue teardown. |
| `drivers/dma/idxd/cdev.c` | pass | Generalizes EVL draining and drains final WQ entries before destroying the per-WQ workqueue. |
| `drivers/gpu/drm/bridge/analogix/anx7625.c` | pass | Disables the interrupt and cancels work before bridge teardown. |
| `drivers/hv/mshv_eventfd.c` | pass | Flushes the irqfd cleanup workqueue before destroying it. |
| `drivers/hv/vmbus_drv.c` | pass | Wraps per-CPU schedule and flush with the architecture sync-wait begin/stop/end sequence. |
| `drivers/infiniband/hw/mlx4/cm.c` | pass | Comment and brace formatting only; no semantic change. |
| `drivers/md/dm-crypt.c` | pass | Indentation only; no semantic change. |
| `drivers/media/test-drivers/vivid/vivid-core.c` | pass | Creates global workqueues before exposing devices and drains them before device release. |
| `drivers/mmc/host/omap.c` | pass | Quiesces IRQ, timer, and work before freeing resources. |
| `drivers/net/can/spi/hi311x.c` | pass | Flushes the workqueue before destroy. |
| `drivers/net/can/spi/mcp251x.c` | pass | Flushes the workqueue before destroy. |
| `drivers/net/ethernet/cavium/liquidio/lio_main.c` | pass | Protects link-status publication and teardown with `link_status_lock`. |
| `drivers/net/ethernet/cavium/liquidio/lio_vf_main.c` | pass | Protects VF link-status publication and teardown with `link_status_lock`. |
| `drivers/net/ethernet/freescale/dpaa2/dpaa2-eth.c` | pass | Flushes the PTP workqueue after netdev unregister. |
| `drivers/net/ethernet/marvell/octeontx2/af/cgx.c` | pass | Drains the command workqueue before destroy. |
| `drivers/net/ethernet/marvell/octeontx2/af/mcs_rvu_if.c` | pass | Uses sched-RCU for the RVU pointer and publishes it only after the interrupt workqueue is ready. |
| `drivers/net/ethernet/marvell/octeontx2/af/rvu.c` | pass | Drains mailbox workqueues and adds representor exit before final teardown. |
| `drivers/net/ethernet/marvell/octeontx2/af/rvu_cgx.c` | pass | Drains the CGX event workqueue before destroy. |
| `drivers/net/ethernet/marvell/octeontx2/af/rvu_rep.c` | pass | Adds an exit helper that drains and destroys the representor event workqueue. |
| `drivers/net/ethernet/marvell/octeontx2/nic/cn10k_ipsec.c` | pass | Drains the SA workqueue before destroy. |
| `drivers/net/ethernet/marvell/prestera/prestera_pci.c` | pass | Drains the firmware workqueue before destroy. |
| `drivers/net/ethernet/qlogic/qed/qed_main.c` | pass | Uses synchronous delayed-work cancellation and restructures teardown safely. |
| `drivers/net/ethernet/wiznet/w5100.c` | pass | Flushes work and reorders unregister/reset/destroy safely. |
| `drivers/net/wireless/realtek/rtw88/usb.c` | pass | Drains the RX workqueue before purging and destroy. |
| `drivers/net/wireless/realtek/rtw89/usb.c` | pass | Drains the RX workqueue before purging and destroy. |
| `drivers/net/wireless/st/cw1200/bh.c` | pass | Flushes the BH work before workqueue destroy. |
| `drivers/pci/endpoint/functions/pci-epf-test.c` | pass | Unregisters the endpoint driver before destroying the workqueue. |
| `drivers/power/supply/ab8500_btemp.c` | pass | Stops periodic work before workqueue destroy. |
| `drivers/scsi/ibmvscsi_tgt/ibmvscsi_tgt.c` | pass | Kills the tasklet before workqueue destroy. |
| `drivers/scsi/qedf/qedf_main.c` | pass | Stops LL2 before destroying its receive workqueue. |
| `drivers/xen/privcmd.c` | pass | Flushes the irqfd cleanup workqueue before destroy. |
| `fs/gfs2/ops_fstype.c` | pass | Comment wrapping only; no semantic change. |
| `kernel/cgroup/cgroup-v1.c` | pass | Comment only; no semantic change. |
| `net/vmw_vsock/vsock_loopback.c` | pass | Adds the workqueue include required by the wrapper call. |

## Findings

No unresolved call-site defect remains:

- The 184 mechanical substitutions preserve the original call context and
  control flow exactly.
- The 34 substantive files fix producer/consumer, teardown, or ordering races
  without narrowing completion guarantees or changing error semantics.
- No file adds a new sleepable operation in an atomic context.
- No file can execute on an accelerator-owned CPU as a result of these changes.
- No cancellation, teardown, or resource-release ordering change introduces a
  new deadlock or leak.

## Evidence

- The complete 218-file inventory is recorded in
  `Documentation/accel/phase10-blast-radius-call-site-inventory.tsv`.
- The live population check found 218 files and 526 wrapper calls.
- The normalization comparison found 184 exact mechanical substitutions and
  34 files requiring individual review.
- The substantive diffs were reviewed with three lines of context and indexed
  as ephemeral capture `cap_778`.

## Status

All 218 call-site files have `pass` verdicts. The Phase 10 workqueue call-site
audit is complete.
