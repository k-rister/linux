# Phase 8 IRQ-work ordering probe test plan

Date: 2026-10-28
Branch: `codex/accelerator-oslat`
Target host: `accelerator-dev` (dedicated, recovery-capable accelerator VM)

## Purpose

Run the focused local-IRQ-work ordering probe
(`tools/cpu_accel/test-call-function-single-owner-stop.sh irq-work`) and
record whether a deferred IRQ-work callback observed an active accelerator
owner after the synchronous `smp_call_function_single()` wait returned.

A nonzero `irq_work_callback_owner_active_count` in the `post_sync` dmesg
line confirms the documented sync-callback ordering gap (see the "Probe
review" section of `phase8-handoff-2026-10-28.md` and the ordering notes in
`cpu-accel.rst`); by the happens-before chain in that review, it also
implies the waited call-function CSD callback ran before the owner's exit
completed. A zero count means the gap was not observable in that run and the
probe may be repeated within the bounded retry limit.

## Scope and non-disruption assessment

- One bounded probe run per iteration: a single ring-3 `user-hang` owner on
  CPU 2 for at most 5 s, one 100 ms PMU NMI window on the owned CPU, and one
  synchronous call-function request from CPU 3.
- No stress loops, no network traffic, no CPU hotplug, no sysctl changes,
  no reboot or kexec, no other test harness running concurrently.
- Worst case is the already-exercised NMI escape and recovery path
  (`state=7`, `recovery_state=2`) on a recovery-capable VM; the harness
  enforces an 8 s timeout with kill on the probe trigger and a 2 s owner
  wait bound, and cleans up the trace instance, module, and owner run.
- The host OS continues to run normally on the other CPUs; no persistent
  state is created beyond the captured logs.

## Preconditions

1. `accelerator-dev` is reachable and a human has confirmed no other
   workload or test is using it.
2. `uname -r` on the VM is recorded before the run. The probe module's
   vermagic must match; the harness fails cleanly on mismatch. If the
   previously copied probe was built against a different build (the session
   reports both `...-02141-g1c9eb2e109a0` and `...-02104-ge406bf98cb01`),
   rebuild the probe against the booted build's source tree before running.
3. No other accelerator owner is active anywhere on the VM (single-owner
   attribution precondition for the global `x86_cpu_accel_any_active()`
   predicate): no running `cpu-accelctl` process, no leftover probe module.
4. CPUs 2 and 3 are online; CPU 2 is RCU callback-offloaded (as used by the
   recorded RCU admission runs); `/dev/cpu_accel` exists; the `cpu_accel`
   tracepoints and `function_graph` are available (harness preflight).
5. `dmesg` baseline is captured and contains no soft lockup, hung task,
   RCU stall, BUG, Oops, or panic records.

## Procedure

1. Record `uname -r`, `date`, and the dmesg baseline on the VM.
2. Stage the reviewed probe module and
   `test-call-function-single-owner-stop.sh` (and a matching
   `cpu-accelctl` if the VM copy is stale) on the VM.
3. Run, as root on the VM:

   ```sh
   CPU_ACCELCTL=/path/to/cpu-accelctl \
   CPU_ACCEL_SINGLE_PROBE=/path/to/cpu_accel_call_function_single_probe.ko \
   CPU_ACCEL_CPU=2 CPU_ACCEL_SINGLE_WORK_CPU=3 \
   sh /path/to/test-call-function-single-owner-stop.sh irq-work
   ```

4. Capture from the harness output directory: `dmesg-new.log` (probe
   `pre_exit` and `post_sync` or final lines), `trace.log` (owner stop /
   exit pairing), `ctl.log` (final state, recovery, escape count), and
   `probe_elapsed_ms`.
5. Interpret:
   - `irq_work_callback_owner_active_count=0` and PASS: the gap was not
     observable in this run. Repeat up to three additional runs; if all are
     zero, record "not observed" with all four runs' evidence and keep the
     gap open as unconfirmed for this build.
   - Nonzero `irq_work_callback_owner_active_count`: stop iterating; record
     the run as gap confirmation with the full log set.
   - Any other failure (precondition, timeout, kernel error, lost trace
     events): diagnose as a test failure; do not record it as gap evidence.
6. Verify cleanup: module removed, no active owner, CPU 2 back to normal
   housekeeping, no new kernel-error records in dmesg.
7. Record the result (kernel build, date, logs, interpretation) in the
   frequency/idle/thermal/power row of the Phase 8 matrix in
   `Documentation/accel/cpu-accel.rst`.

## Exit criteria for this plan

- At least one completed, interpreted probe run with the booted kernel
  build recorded, or a documented blocker.
- If the gap is confirmed, the next work item is the
  owner-exit-before-callback handshake design (separate plan).

## Out of scope (requires a separate reviewed plan)

- Stress loops or repeated-run endurance beyond the bounded retries above.
- Other Phase 8 matrix rows: IRQ storms, hotplug races, RCU storms,
  frequency/cooling runtime tests, fault-injection suites, and the
  run-specific worst-case latency report.
