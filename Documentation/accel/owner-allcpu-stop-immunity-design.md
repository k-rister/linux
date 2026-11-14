# Owner immunity to all-CPU stop operations -- design

Date: 2026-11-04
Branch: `codex/accelerator-oslat`
Status: open design problem (not implemented)

## Problem

An active owner is terminated by the first all-CPU stop operation. During the oslat
soak (bounded `user-oslat`, 1 ms period, isolated CPU) a run was repeatedly cut at a
non-deterministic ~17-144 s by an owner-terminating maintenance window. The full chain,
captured with the `owner_stop_request` tracepoint plus function/stack ftrace:

```
prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER)        (sshd-session, one per SSH connection)
  -> bpf_prog_create_from_user -> bpf_int_jit_compile
  -> bpf_jit_binary_pack_finalize -> bpf_arch_text_copy
  -> text_poke_copy -> x86_cpu_accel_maintenance_begin(STOP_TERMINATE)
  -> x86_cpu_accel_request_stop_owner_gen -> owner_stop_request (stop_cb=1, sent=1)
  -> owner-terminating NMI -> user_escape_count=1, "did not complete its ring-3 contract"
```

`text_poke` (live kernel-text patching) must stop every CPU, and it reaches the accelerator
through `x86_cpu_accel_maintenance_begin()`. On the accelerator path the stop is
`STOP_TERMINATE`, so the first such op ends the run.

The captured trigger was BPF JIT of a seccomp filter (each SSH session installs one), but the
same class of op applies to every `text_poke` consumer -- jump-labels / static keys,
ftrace / kprobes, `set_memory_*` / CKSC MTRR updates, microcode load -- plus the hotplug,
`cpu_up`, reboot, and TDX maintenance paths (all `STOP_TERMINATE`). A dedicated accelerator
host must not have an active owner window be a reason to tear the owner down; that is the
design goal this doc tracks. "Avoid triggering such ops during the window" (e.g. no SSH) is
a workaround, not the solution.

## Current behavior

- `x86_cpu_accel_maintenance_begin(stop_flags)` (arch/x86/kernel/accel.c:558) closes
  admission, then for every possible CPU calls `x86_cpu_accel_request_stop_owner(cpu,
  stop_flags)` and waits for `x86_cpu_accel_active_count == 0`.
- The driver stop callback `cpu_accel_arch_stop_user()` (drivers/cpu_accel/cpu_accel.c:447)
  has two paths:
  - `X86_CPU_ACCEL_STOP_DRAIN` (the one hotplug-maintenance caller, kernel/cpu.c:1426):
    lets a bounded owner finish and only escalates past its own deadline
    (`cpu_accel_stop_escalation_timer`). Non-terminal.
  - no DRAIN (the `STOP_TERMINATE` callers: arch/x86/kernel/cpu/mtrr/mtrr.c:259/412,
    arch/x86/kernel/cpu/microcode/core.c:775, `x86_cpu_accel_cpu_up_maintenance_begin`,
    arch/x86/kernel/reboot.c:679, arch/x86/virt/vmx/tdx/seamldr.c:343): sets
    `user_escape_requested` and immediately sends the owner-terminating NMI; the run ends.
- `text_poke_copy` / `text_poke_set` arrive as `STOP_TERMINATE`, so BPF JIT and the other
  text_poke consumers take the terminal path.

The owned CPU is *correct* to stop during `text_poke` (it must not execute a half-patched
instruction), but the *run* is ended instead of being allowed to survive the stop.

## Evidence (the workaround holds; the design gap remains)

On the current box (build83, 1-11 isolated via nohz_full + rcu_nocbs, target CPU 6), keeping
all-CPU stop ops out of the window (no new SSH sessions; a local on-box 30 s poller) gives a
clean continuous run:

| window (wall) | samples   | max lateness | p99.9 | escapes | stop requests |
|---------------|-----------|--------------|-------|---------|---------------|
| ~8 min        | 600000    | 24 ns        | 32 ns | 0       | 0             |
| ~49 min       | 3600000   | 24 ns        | 32 ns | 0       | 0             |

Every sample is < 32 ns, ~400x under the 10 us max-latency gate, flat tail. So the
requirement is met; the open problem is making the owner survive an all-CPU stop op rather
than requiring such ops to be absent.

Side observation: the "1 ms" period ran ~811 us wall-clock (3600 s TSC => ~2919 s real), a
~22% TSC-frequency skew under `tsc=reliable`. It does not affect the lateness numbers, but a
durable run-duration knob should be defined against a stable timebase.

## Design goals

- The looping user workload runs -- potentially unbounded ("forever") -- without unintended
  interruption: a spurious all-CPU stop op must not tear the run down.
- The only intended stop is the control program's explicit exit, and the run must honor it
  in a timely fashion (within seconds).
- Correctness preserved: for an op that does run, every CPU it actually needs is quiesced;
  a CPU the op does not need (the ring-3 owned CPU, which is not executing the patched
  target) is left running and safe.
- No added latency on the non-stopped path.

## Why "defer the op" does not work for a forever-run

Deferring (the op waits for the owner to exit) is not viable when the run is unbounded:

- The op's caller blocks in `x86_cpu_accel_maintenance_begin()` until
  `x86_cpu_accel_active_count == 0` (accel.c:616). A forever-owner never exits, so the
  caller (e.g. the BPF-JIT / SSH thread) wedges indefinitely.
- The physical all-CPU stop is per-CPU: `arch_smp_sync_wait_stop()` (accel.c:364) runs on
  each quiesced CPU, and the stop waits for every target CPU to reach it. A ring-3 owned
  CPU is masked and does not quiesce, so the stop would hang on it. This is exactly why
  `maintenance_begin` forces the owner out *before* the stop today.

So the op cannot wait its turn while the run is unbounded; the run and the op must not be
serialised on the owned CPU.

## Design: exclude the owned (ring-3) CPU from the op (safe-to-skip)

The owned CPU, while active, runs ring-3 and is not executing the kernel text a
`text_poke`-class op patches. It is safe to leave it *out* of the all-CPU stop: the op
proceeds on every other CPU and the owned CPU keeps running. The owned CPU's own
(lazy/queued) commit of the patched site is deferred until it next re-enters the kernel;
on x86 the instruction cache is coherent, so it observes the patched instruction then.

This is the direction this doc now tracks. It is not "defer the op" (the op is not delayed)
and not "interrupt and re-admit the owner" (the owner is never stopped). It works for an
unbounded run because the owned CPU never has to stop.

### Op classification

- **Safe-to-skip (immune for an unbounded run):** `text_poke` consumers -- BPF JIT (the
  SSH-seccomp case), jump-labels / static keys, ftrace / kprobes, `set_memory_*` (non-MTRR).
  The owned CPU is not executing the patched target, so excluding it is safe.
- **Must-stop (cannot be skipped or deferred; ends the run):** MTRR / CKSC (changes the
  owned CPU's own memory-type state), microcode load, hotplug / `cpu_down`, reboot, TDX.
  These require the owned CPU to physically stop or the CPU to be removed. On a dedicated
  accelerator host this class must be absent (no MTRR churn, no hotplug / reboot /
  microcode on the owned CPU); otherwise a running owner legitimately ends.

### Concrete change (outline)

- Add a stop flag, e.g. `X86_CPU_ACCEL_STOP_SKIP_OK`, meaning "the op may proceed without
  the owned CPU because the owned CPU is not executing the op's target." `text_poke` callers
  pass it; must-stop callers do not.
- `x86_cpu_accel_maintenance_begin(stop_flags)`: when `SKIP_OK` and a CPU is owned and in
  ring-3, do not force the owner out; instead mark that CPU skip-able for the in-flight
  stop. Without `SKIP_OK`, keep today's force-exit (terminal / drain).
- The physical stop (`arch_smp_sync_wait_*` / the op's stop path) omits skip-able owned CPUs
  from its target set and does not wait for them to quiesce.
- The owned CPU's queued commit of a patched site lands on re-entry (control-program exit or
  natural completion); no change beyond the existing re-entry path.
- The control-program exit is unchanged: the owner polls `stop_requested` and exits within
  ~one period (well under seconds).

### Ruled-out alternatives (for the record)

- *Defer the op* (former Option 1): not viable for an unbounded run (see above).
- *Interrupt and re-admit the owner* (former Option 2): unnecessary if the owned CPU is
  excluded from the op; it would only be needed for the must-stop class, where the run ends
  anyway.
- *Quiesce the sources* (former Option 3): a host policy, not a design guarantee; retained
  only as a test-harness constraint.

## Open questions

- Which all-CPU stop mechanism each safe-to-skip caller actually uses (`stop_machine` vs
  `smp_call_function`-style sync) and whether it can omit a single CPU from its wait set.
  `stop_machine` is all-or-nothing; a per-CPU `smp`-sync stop can exclude CPUs. This decides
  whether each caller is SKIP_OK-able as-is or needs its stop reworked.
- Should the must-stop class be refused / failed while a ring-3 owner is active (so a
  misconfigured host cannot silently end a run), or is "absent on a dedicated host" sufficient?
- Confirm the safe-to-skip set is closed: enumerate every caller of
  `x86_cpu_accel_maintenance_begin()` / the physical stop and tag it safe-to-skip or
  must-stop.

## Out of scope

- The per-cpu kernel-work (RCU / workqueue / reschedule) yield-and-resume, explicitly ruled
  out as a goal (that work should not be processed on the owned CPU at all).
- The `isolcpus` / `irqaffinity` boot-config gap on the current box (separate host-isolation
  item, not the immunity design).
