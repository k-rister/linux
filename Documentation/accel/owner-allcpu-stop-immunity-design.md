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

- An active bounded owner is not terminated by an all-CPU stop op (text_poke, hotplug, MTRR,
  microcode, ...); the run survives it and reaches its full duration.
- Correctness preserved: the owned CPU still stops long enough for the op to land (no
  half-patched instruction executes; MTRR / microcode / hotplug complete).
- No unbounded starvation of the all-CPU op: a deferred op must be allowed to proceed within
  a bounded time even if the owner keeps running.
- Retain and generalise the existing DRAIN (hotplug-maintenance) semantics.
- No added latency on the non-stopped path.

## Candidate designs

### Option 1: admission-gate / defer the op (recommended direction)

While an owner is active, a new `STOP_TERMINATE` maintenance op is *deferred* (queued) until
the owner exits, with a bounded wait; the op's text_poke then lands. This matches the
"admission closes, the set cannot grow" model already in
`x86_cpu_accel_maintenance_begin()`.

- Pros: the owner window is never broken.
- Cons: the op is delayed by the owner's remaining time. On x86, text_poke uses a full
  all-CPU stop, so the whole op (not just the owned CPU's commit) waits. A bound is required
  so a long or misbehaving owner cannot starve it; the bounded fallback is the current
  terminal escape.

### Option 2: non-terminal stop for bounded owners (yield the op, keep the run)

Make the driver stop callback, for a *bounded* user owner, take a yield-not-terminate path:
the owner finishes its in-flight deadline, the stop/escape context lets the op land, and the
run resumes for the remainder of its duration. This is distinct from the per-cpu-work
yield-and-resume that was ruled out: here the op is a global all-CPU stop that *must*
involve the CPU, so the run must survive it rather than the per-cpu work being avoided.

- Pros: the op is never deferred (no starvation); the run reaches its full duration.
- Cons: re-entering a user image after an NMI escape is a large state-machine change; the
  current escape is terminal by construction (redirects to `escape_ip`, image released).
  Re-entry needs a save/restore or re-admission (context, TLB, `mmu_owner` re-registration,
  watchdog re-arm).

### Option 3: keep it terminal; require quiesced sources (status quo + policy)

Keep the terminal stop and define a "clean host" policy: disable BPF JIT, avoid
ftrace/kprobes/dynamic-debug, avoid set_memory/MTRR churn and microcode updates during a
run. This is the policy that produced the clean 49-min run.

- Pros: no code change.
- Cons: not robust (any unanticipated text_poke consumer ends the run); a host policy is
  not a design guarantee. Rejected as the solution; retained only as a test-harness
  constraint.

## Open questions

- Can text_poke be split so the owned CPU's commit is independent of the other CPUs
  (enabling deferral without a full all-CPU stall)? Current x86 `text_poke` is a
  stop_machine-style full stop; confirm whether a per-CPU commit-barrier variant exists or
  is feasible.
- What starvation bound fits the latency contract for Option 1, and does the bounded terminal
  fallback stay within it?
- For Option 2, is re-admission of the user image after an NMI escape feasible and safe
  (context save/restore, TLB, `mmu_owner` re-registration, watchdog re-arm)?

## Out of scope

- The per-cpu kernel-work (RCU / workqueue / reschedule) yield-and-resume, explicitly ruled
  out as a goal (that work should not be processed on the owned CPU at all).
- The `isolcpus` / `irqaffinity` boot-config gap on the current box (separate host-isolation
  item, not the immunity design).
- Unbounded / duration-less owners (the escalation grace window already exists).
