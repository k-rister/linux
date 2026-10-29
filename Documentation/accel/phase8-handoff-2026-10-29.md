# CPU accelerator Phase 8 handoff

Date: 2026-10-29
Repository: `/home/krister/github/linux`
Branch: `codex/accelerator-oslat`
Supersedes: `phase8-handoff-2026-10-28.md` (kept for history)

## Status

Phase 8, adversarial isolation validation, remains open. Current state:

- The sync-callback ordering gap is **confirmed** on
  `7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0` by one bounded irq-work probe
  run (`post_sync irq_work_queued=1 irq_work_callback_count_after_sync=1
  irq_work_callback_owner_active_count=1 callback_count=1`; evidence in
  `phase8-handoff-2026-10-28.md` and the frequency/idle/thermal/power row of
  the Phase 8 matrix in `cpu-accel.rst`).
- The owner-exit-before-callback handshake is **designed and verified,
  pending implementation**: `phase8-owner-exit-handshake-design.md`
  (commit `b59e8b1a47c0`) records the design and Option C as the
  recommendation; commit `a9be1dccfe8a` records the implementation
  verification findings and the three-step implementation plan.
- Remaining Phase 8 work after the handshake lands: the remaining
  unclassified matrix rows (device-management beyond the tested network
  backlog, microcode other update paths, MCE general events),
  fault-injection coverage, and the run-specific worst-case latency report.
  MCE hardware injection stays deferred (needs a disposable system).

## Repository state

- Worktree clean. Branch is ahead 2 of origin:
  - `a9be1dccfe8a` Documentation: accel: record owner-exit handshake verification
  - `b59e8b1a47c0` Documentation: accel: design owner-exit-before-callback handshake
  - origin head: `f078a5208193` (pushed)
- **Push pending**: github.com:22 intermittently blackholes long-lived SSH
  sessions (push stalls with "Timeout, server github.com not responding"
  while `git ls-remote` succeeds in under a second). A push can land
  server-side even when the client reports failure — always verify with
  `git ls-remote origin codex/accelerator-oslat` before retrying.
- Working push command when the network is up:
  `timeout 90 git -c core.sshCommand="ssh -F /dev/null -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=3" push origin codex/accelerator-oslat`
- The VM's booted kernel `7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`
  corresponds to commit `1c9eb2e109a0`; every commit after it is
  docs/tools-only, so the booted-kernel code equals the current tree code.

## The gap and the chosen fix

Root cause (settled): for a waited `smp_call_function_single()` to an owned
CPU, `__smp_call_function_single()` in `kernel/smp.c` queues the CSD and only
then requests owner stop via `arch_smp_sync_wait_stop()`. The stop NMI escape
returns the CPU to Linux mode and interrupts are re-enabled during the
escape flow, while the full exit bookkeeping — `x86_cpu_accel_user_exit()` =
owner exit + `x86_cpu_accel_tlb_reconcile()` + `x86_cpu_accel_owner_exit_finish()`
— only runs from the char-device release path
(`cpu_accel_user_image_release()` in `drivers/cpu_accel/cpu_accel.c`). The
CSD (and any queued IRQ work, such as the probe's perf-NMI IRQ work)
therefore observes the owner still marked active and runs before TLB
reconciliation completes.

**Chosen fix — Option C**: complete the remaining exit steps in the stop NMI
handler itself (NMI context on the owned CPU), before the escape tail runs.
Verified properties (details in the design doc):

- No vector can be serviced during NMI context or while the escape tail runs
  with the image's forced `IF=0`, so the first post-escape interrupt service
  is ordered after `x86_cpu_accel_owner_exit_finish()` by construction. The
  exact interrupt re-enable point (FRED syscall entry vs. the USER_EXIT
  handler's `local_irq_enable()`) is therefore irrelevant — do not
  re-litigate it during implementation.
- NMI-safety of every call in the path is verified: raw spinlocks only;
  `mmu_owner_unregister()` uses `mmput_async()` (explicitly atomic-context
  safe); the local reconcile branch is `__flush_tlb_all()` (lock-free) plus
  atomic-only subscription completions (the production subscriber
  `mmu_gather_completion_owner_ops` is atomic-safe); finish is a raw
  spinlock, one `atomic_dec()`, and `wake_up_all()`. No blocking lock, no
  sleeping.
- The stop target is always the calling CPU (the NMI handler checks
  `raw_smp_processor_id() == dev->config.cpu`), so
  `x86_cpu_accel_tlb_reconcile()` always takes the local branch; the
  remote-ack nested call-function is unreachable from stop context. Assert
  the invariant with `WARN_ON_ONCE`.
- The `stop_inflight` spin at the end of `x86_cpu_accel_owner_exit()` is
  bounded: the requester CPU (spinning in the CSD wait loop) decrements
  `stop_inflight` immediately after the asynchronous NMI send.
- The release path is already idempotent (`atomic_xchg` on `active`, finish
  no-ops when the exit did not happen, no double `atomic_dec` of the global
  active count). One telemetry correction is required:
  `cpu_accel_user_image_release()` overwrites
  `shared->arch_tlb_shootdown_targets` from `x86_cpu_accel_user_exit()`'s
  return value, which reads 0 when the stop-context exit already ran; the NMI
  handler must capture the `tlb_shootdown_targets` snapshot and the release
  path must prefer it.

## Implementation plan (from commit a9be1dccfe8a)

1. `arch/x86/kernel/accel.c`: add exported
   `x86_cpu_accel_user_stop_exit(unsigned int cpu)` running
   `x86_cpu_accel_owner_exit()`, `x86_cpu_accel_tlb_reconcile()` (local
   branch only), and `x86_cpu_accel_owner_exit_finish()`, returning the
   `tlb_shootdown_targets` snapshot like `x86_cpu_accel_user_exit()`. Add
   the non-LAPIC stub in `arch/x86/include/asm/cpu_accel.h` next to the
   `user_enter`/`user_exit` stubs.
2. `drivers/cpu_accel/cpu_accel.c`: `cpu_accel_user_nmi()` calls
   `x86_cpu_accel_user_stop_exit()` after the escape-drop check and before
   redirecting `regs->ip` to the escape tail; store the snapshot plus a done
   flag in `struct cpu_accel_device`; the release path uses the captured
   snapshot for `shared->arch_tlb_shootdown_targets` when the stop-context
   exit ran.
3. Build the candidate kernel, boot it on `accelerator-dev`, and run the
   design-doc test plan; the irq-work probe must show
   `irq_work_callback_owner_active_count=0`.

## Environment

- **Reviewer model works**: `~/.codex/dodo.config.toml` has
  `model_provider = "dodo"`, `model = "qwen3.8-27b"`, and
  `review_model = "qwen3.8-27b"` (the reviewer runs the same self-hosted dodo
  vLLM model as the session). The base config's dodo provider points
  directly at `http://dodo.lab.gtristers.consone.net:8000/v1` (the
  `127.0.0.1:8001` trace-proxy line is commented out). Verified end-to-end on
  2026-10-29: an escalated command was approved by the auto-review reviewer.
- VM `accelerator-dev` = `accelerator-dev.lab.gtristers.consone.net`, root
  ssh; all ssh needs `-F /dev/null` (the system ssh config is broken).
  Booted `7.3.0-rc3-accel-tlbfix-02141-g1c9eb2e109a0`; cmdline
  `isolcpus=managed_irq,2 irqaffinity=0-1,3-7 nohz_full=2 rcu_nocbs=2`.
- VM staging `/tmp/cpu-accel-irq-work-test-20261028/`: probe `.ko` (vermagic
  matches the booted kernel), `test-call-function-single-owner-stop.sh`, and
  the ABI-17 `cpu-accelctl`. The old `/usr/local/bin/cpu-accelctl` is
  ABI-stale (fails open with EPROTO/"Protocol error") — do not use it.
- Working probe invocation (root on the VM):
  `cd /tmp/cpu-accel-irq-work-test-20261028 && mkdir -p /tmp/cpu-accel-irq-work-runN && CPU_ACCELCTL=$PWD/cpu-accelctl CPU_ACCEL_SINGLE_PROBE=$PWD/cpu_accel_call_function_single_probe.ko CPU_ACCEL_CPU=2 CPU_ACCEL_SINGLE_WORK_CPU=3 CPU_ACCEL_TRACE_DIR=/tmp/cpu-accel-irq-work-runN sh test-call-function-single-owner-stop.sh irq-work`
  — `CPU_ACCEL_TRACE_DIR` must pre-exist; the harness does not create it and
  a missing dir silently kills the trace capture.
- Build-host logs from the confirming run: `/tmp/accel-phase8-logs/`
  (ctl.log, dmesg-new.log, probe.log, trace.log).
- Sandbox: network disabled, `.git` read-only (commits need escalation),
  writable = repo + /tmp.

## Next steps

1. Implement Option C per the plan above (driver-sized change).
2. Commit (AI-signed trailers: `AI-Tool: Codex`, `AI-Model: qwen3.8-27b`,
   `AI-Effort: medium`) and retry the push; verify with `ls-remote`.
3. Build the kernel in-tree (incremental; the tree has build artifacts).
4. Boot the candidate on `accelerator-dev` (one-time GRUB entry pattern used
   before) and run the design-doc test plan: the bounded irq-work probe must
   PASS with `irq_work_callback_owner_active_count=0` across the bounded
   retry set; re-run the `sync`, `smp-call-on-cpu`, and `work-on-cpu` modes,
   the watchdog admission run, and the perf owner-stop run. Record all
   results with the kernel build in the Phase 8 matrix; close the row only
   when the handshake run evidence lands.
5. Then the remaining Phase 8 rows: device-management classification audit,
   microcode other update paths, fault-injection coverage, worst-case
   latency report.

## Constraints

- Never commit unreviewed work (commits go through the auto-reviewer via
  escalation); no unreviewed stress on the VM — the bounded test plan in
  `phase8-irq-work-probe-test-plan.md` is the approved probe artifact.
- Keep changes driver-sized; update the `cpu-accel.rst` matrix as
  dispositions land.
- MemPalace memory is current as of this handoff (2026-10-29 checkpoint:
  project drawers in `linux-dataplane-accelerator` decisions/architecture/
  testing/status, knowledge-graph facts superseded and added, agent diary
  entry filed).
