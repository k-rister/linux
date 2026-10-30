# CPU accelerator Phase 8 handoff

Date: 2026-10-30
Repository: `/home/krister/github/linux`
Branch: `codex/accelerator-oslat`
Supersedes: `phase8-handoff-2026-10-29.md` (kept for history)

## Status

Phase 8, adversarial isolation validation, is substantially complete. The
Option C owner-exit-before-callback handshake is implemented, committed,
and re-validated on the current VM kernel. The death-safe worker kill
path is verified end-to-end. The user-oslat workload is verified on the
RCU-offloaded CPU. Remaining items are source-audit-only rows (microcode,
suspend, kexec, reboot/panic) and fault-injection coverage that require
either hardware or a disposable test system.

### Completed this session (2026-10-30)

- **Death test verified end-to-end**: SIGKILL mid-run on `user-death`
  workload (CPU 4): `state=5`, `recovery_state=5`, `recovery_error=-3`
  (ESRCH), 0 "scheduling while atomic". Worker active ~428 ms before
  kill. Kill timing: single-SSH-session tight poll loop scanning `/proc`
  for `comm=exe` on the target CPU.
- **user-oslat verified on CPU 2**: 2048 samples, p50=8 ns, p90=16 ns,
  p99=4096 ns, p99.9=16384 ns, clean exit. The earlier EOPNOTSUPP on
  CPU 4 was simply using the wrong CPU; the VM boot cmdline has
  `rcu_nocbs=2`.
- **Option C re-validated on build #80**: all four probe modes passed
  (`irq-work`: `irq_work_callback_owner_active_count=0`; `sync`,
  `smp-call-on-cpu`, `work-on-cpu`: `callback_count=1`), zero kernel
  errors. Recorded in `cpu-accel.rst` matrix.
- **WCL report updated**: baseline (p50=16 ns, max=247595 ns) and load
  (p50=8 ns, max=81825 ns) lateness distributions recorded for build
  #80 in `phase8-wcl-report.md`.
- **Source tree clean**: all diagnostics removed, module rebuilt and
  deployed.

### Remaining Phase 8 items

Source-audit-only rows (no runtime run recorded; classification is in
the matrix):

- Microcode updates: no runtime update run; failure injection, event
  counters, and latency evidence remain open.
- Suspend and hibernate: no lifecycle run; failure injection and
  external continuity evidence remain open.
- Kexec and kexec-jump: no lifecycle run; failure injection and
  external continuity evidence remain open.
- Reboot and panic: no lifecycle run; deterministic owner termination
  and external continuity evidence remain open.
- Resctrl: no fault-injection while owner active; no resctrl-specific
  counter for the remaining task-group and monitoring paths.

Requires hardware or disposable system:

- MCE hardware injection (deferred; in-tree injector's software mode
  only decodes a synthetic record, hardware mode may panic).
- Runtime `INVLPGB` validation (requires AMD EPYC 7003+).
- `irq_storm` WCL class (requires a test NIC).

Fault-injection coverage (open):

- Device-management: no device reset/teardown/management-operation fault
  matrix. RFS-to-owned-CPU case is source-classified only.
- Frequency/idle/thermal: runtime frequency, idle, thermal, and
  power-policy tests, event counters, and observed latency remain open.
- Memory pressure: no sustained pressure envelope or dirty/writeback/
  migration stress report.

## Repository state

- Worktree clean. Branch is ahead 1 of origin (pushed):
  - `4b5836e7fae5` Documentation: accel: record Phase 8 Option C
    re-validation on build #80
  - `e9767ed80559` cpu_accel: fix death-safe preempt_count underflow in
    USER_EXIT
  - origin head: `4b5836e7fae5` (pushed)

## Environment

- VM `accelerator-dev` = `accelerator-dev.lab.gtristers.consone.net`,
  root ssh; all ssh needs `-F /dev/null`.
- Booted `7.3.0-rc3-accel-tlbfix-gb381f8577d1a-dirty` (build #80);
  cmdline `isolcpus=managed_irq,2 irqaffinity=0-1,3-7 nohz_full=2
  rcu_nocbs=2`.
- A newer kernel (build #81, commit `e9767ed80559`) is on the VM at
  `/boot/vmlinuz-7.3.0-rc3-accel-phase8-20261030` but needs a matching
  initramfs before it can boot. The current kernel (build #80) includes
  all Option C code and works for testing.
- VM staging `/tmp/cpu-accel-irq-work-test-20261028/`: `cpu-accelctl`,
  `cpu_accel.ko`, probe modules (all rebuilt with matching vermagic),
  test scripts.
- Module vermagic workaround: `include/generated/utsrelease.h` was
  temporarily patched to match the running kernel's UTS_RELEASE, modules
  rebuilt, file restored. This avoids a full kernel rebuild + initramfs
  + reboot cycle.

## Next steps

1. (Optional) Build a matching initramfs for build #81 and boot it on
   the VM to eliminate the vermagic workaround.
2. (Optional) Rebuild a clean bzImage without the `dbg_syscall` /
   `dbg_doexit` kernel-core diagnostics from build #80.
3. Device-management fault matrix: design and implement bounded
   device-reset/teardown fault injection while an owner is active.
4. Resctrl fault injection: run a resctrl operation while an owner is
   active and record the owner-stop/exit trace pair.
5. Memory pressure sustained envelope: run a sustained reclaim/migration
   stress with the owner active and record the lateness distribution.
6. MCE injection: requires a disposable recovery-capable test system.
   Deferred.

## Constraints

- Never commit unreviewed work; no unreviewed stress on the VM.
- Keep changes driver-sized; update the `cpu-accel.rst` matrix as
  dispositions land.
- MemPalace memory is current as of this handoff (2026-10-30
  checkpoint: project drawers in `linux-dataplane-accelerator`
  decisions/architecture/testing/status/workflow, knowledge-graph facts
  superseded and added, agent diary entries filed).
