# Durable owner run: wall-clock duration + must-stop preempt/resume -- design

Date: 2026-11-05
Branch: `codex/accelerator-oslat`
Status: open design (not implemented). Continues `owner-allcpu-stop-immunity-design.md`.

## Where this picks up

The safe-to-skip immunity (exclude the ring-3 owned CPU from `text_poke`-class all-CPU stops)
is **implemented and validated**: commit `635d5b0`, and a 12h soak that survived ~140K
safe-to-skip maintenance windows with `user_escape_count=0`, `max_lateness_ns=24`, never
leaving CPU 6. That work closed the safe-to-skip half of the earlier doc.

Two things from `owner-allcpu-stop-immunity-design.md` stay open and are the subject of this
doc:

1. **The run-duration knob must be wall-clock stable.** The earlier doc side-observed a ~22%
   TSC/fast-clock skew ("a durable run-duration knob should be defined against a stable
   timebase"). We now have a concrete repro: a `--duration-ms 43200000` (12h) run self-
   completed at **9.73h wall-clock**.
2. **The must-stop class** (MTRR / microcode / hotplug / reboot / TDX). The earlier doc asked
   whether these should be "refused/failed while a ring-3 owner is active, or does the run
   end?" This doc proposes a **third option: preempt / resume** -- the op runs, and the owner
   comes back.

## Requirement (shared by both points)

The owner run lasts **exactly** as long as the user requested, in **wall-clock** time, and
ends **only** on:

- **(a) duration expired** -- the control program's clean exit, or
- **(b) a fatal error.**

A must-stop op is *neither*: it **preempts** the owner, resolves, **resumes** it, and the gap
is recorded as a **latency spike** so the op's real cost is visible in the latency histogram.

## Point 1 -- wall-clock run duration

### Symptom

12h soak, build84, target CPU 6 (nohz_full 1-11): the run reported `state=COMPLETE` at
`lifecycle_exit` ~05:26:57 for a start of 19:43:10 -- **9.73h wall-clock** -- for a 12h
nominal duration. It did complete the full intended *work* (`hist_total=43200000` = 12h of
samples), but in 9.73h of real time. Global timekeeping stayed accurate (`uptime` matched the
RTC); only the run loop's clock ran ~24% fast.

### Root cause (corrected)

For `user-oslat` the run does **not** use the kernel `cpu_accel_run()` loop. The workload runs
in ring-3 as the user image `cpu_accel_user_oslat()` (tools/cpu_accel/cpu-accelctl.c:220), and
both the per-sample deadline and the total-duration gate are driven by the **raw TSC** plus a
one-time `cycles_per_ns` calibration:

- `period_cycles = period_ns * cycles_per_ns` sets the sample cadence (cpu-accelctl.c:230, 281).
- `duration_cycles = duration_ns * cycles_per_ns` sets the end condition
  `now - start_tsc >= duration_cycles` (cpu-accelctl.c:231, 276-277).
- `cycles_per_ns` comes from `cpu_accel_calibrate_cycles_per_ns()` (cpu-accelctl.c:360), which
  sleeps 10 ms against `CLOCK_MONOTONIC` and returns `cycles / elapsed_ns`.

The bug: that return is **integer division** (`cycles / elapsed_ns`, cpu-accelctl.c:386), so
`cycles_per_ns` is truncated to a whole integer. On a non-integer-GHz TSC (this box ~3.7 GHz)
the true value is ~3.7 but the code stores 3 -- ~19% low. Every downstream quantity built from
it is ~19% small: the sample cadence runs ~19% fast (12h of samples in ~9.7h wall) and the
duration gate fires ~19% early (the observed 9.73h wall for a 12h run). The steady-state
*jitter* stays self-consistent (measured against the same TSC deadline), but the absolute
cadence, the absolute lateness scale, and the run length are all off by the same factor.

The kernel `cpu_accel_run()` + `ktime_get_mono_fast_ns` gate (cpu_accel.c:1825) is a separate
path that serves the *kernel* workloads (timestamp/memmove); it does not drive user-oslat. The
earlier doc's "fast-mono stale on the tickless CPU" theory was a misattribution for this
workload -- the TSC itself is stable; the calibration integer-truncation is the defect.

### Design: a dedicated cyclictest probe owns the run (A1 + A2)

The generic control plane is the **driver** (`/dev/cpu_accel` ioctls + the shared mmap).
`cpu-accelctl` is a workload-agnostic CLI wrapper over it. The cyclictest-style latency probe -- the ring-3
user image plus its timing/control -- does **not** belong in that generic tool, so it moves out
into a dedicated program:

- **`cpu-accel-cyclictest`** (new, tools/cpu_accel/) is a cyclictest-style periodic
  deadline-latency probe: fixed period, `lateness = now - deadline`, grid-anchored deadline. It
  drives the driver via the ioctl interface, hosts its **own** ring-3 periodic user image, and owns
  the probe-specific control. (This is the cyclictest-model variant, NOT the oslat
  workload-duration model; the oslat-equivalent is a separate tool still to be built.) Two pieces
  of work:
  - **A1 -- correct the clock.** Compute `cycles_per_ns` at sub-integer (fixed-point) precision
    so the ring-3 sample cadence, the absolute lateness scale, and the TSC-based durations are
    all accurate (the integer-truncation bug, Root cause above).
  - **A2 -- own the wall-clock duration.** `cpu-accelctl` **already** does the manager/owner
    split for user workloads: the manager (`run_user_workload`) pins itself to CPU 0 (ticked),
    opens the device, `fork()`s + re-execs the owner (`--cpu-accel-user-worker`) pinned to the
    target CPU (cpu-accelctl.c:1111, 1140-1166); the manager keeps its own handle on the shared
    fd. `cpu-accel-cyclictest` follows that same pattern, specialized to the probe. The change: today the
    manager passes `duration_ns` to the owner and the owner's image self-terminates on its TSC
    (the bug); instead the **manager owns the wall-clock duration** -- the owner's probe image
    runs **open-ended** (until stopped, the run-forever model), and the manager (on CPU 0) waits
    for `RUNNING`, `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t0 + duration)`, then sends
    a clean **duration-quit** `STOP` via its own handle (same fd -- no new IPC). The driver
    records `state=COMPLETE` for a duration-quit (the intended end), not `STOPPED`.
- **`libcpu_accel.c`** (already exists, tools/cpu_accel/) already provides the generic control
  API -- `open`/`attach_fd`/`configure`/`start`/`stop`/`exit`/`reset`/`wait` + the
  `cpu_accel_handle`. No extraction is needed; `cpu-accel-cyclictest` links it directly. The
  `attach_fd` entry is what lets the manager hold its own handle on the shared fd to send the
  STOP.
- **`cpu-accelctl`** keeps the generic CLI; its built-in `user-oslat` (user image + timing) is
  **removed** once `cpu-accel-cyclictest` works, to avoid two drifting copies.

Why the manager owns duration (rather than the worker's TSC clock):

- It moves the run-length time read **off** the tickless owner CPU entirely.
- It separates concerns: owner = workload + sampling; manager = duration + control. This is the
  "the control program tells it to exit" intent, and what makes the run a durable run-forever
  platform feature rather than a worker-internal timer.
- It reuses the existing stop seam (`stop_requested` + a new duration-quit reason) instead of
  adding a second completion mechanism.

Caveats / enforcement:

- The manager **must run on a ticked CPU** (pin the parent to CPU 0/12+, outside `nohz_full`);
  else it inherits a wall-clock problem of its own. In the soak the trigger/poller already ran
  on CPU 0 and stayed consistent.
- No self-terminating safety cap in the owner. Run-forever is the platform model: the duration
  is a *test* artifact. If the manager is lost, the user stops the owner through the control
  interface (or a terminal op / reboot); we do not add overhead to the owner to police that.

### Sampling data model (histogram vs detail buffer)

The latency **histogram** (`lateness_hist[CPU_ACCEL_LATENESS_HIST_BINS]`) is a fixed set of
bins with a running count; `hist_total` is the *result* (approx duration / period), not a set
size. It scales to a forever run (just keep incrementing bins). The `shared->samples
[CPU_ACCEL_MAX_SAMPLES = 2048]` buffer is a *separate* bounded per-sample detail record (the
first 2048 `(timestamp, lateness)` pairs, for inspection); it does **not** limit the run. For
a forever platform: keep the histogram free-growing; keep `samples[]` as a small bounded
recent-window (or drop it) -- it is a debug convenience, not a budget.

### Sampling-clock accuracy

Making the per-sample **latency** wall-clock-true collides with the probe tight spin, which
relies on the fast clock for speed. Decision in "Sampling clock (decision)" below: keep the
fast clock for the per-sample steady-state (preserve resolution), take the run length
manager-side (wall-clock), and measure the must-stop spike with a dedicated wall-clock
timestamp pair. This does not block Point 1's run-length fix.

## Point 2 -- must-stop ops: preempt / resume (not terminate)

### Current behaviour

A must-stop op calls `x86_cpu_accel_maintenance_begin(TERMINATE)` (arch/x86/kernel/accel.c:
563). The drain loop does **not** skip the owned CPU (only `SKIP_OK` skips), so
`x86_cpu_accel_request_stop_owner()` fires the owner's callback
`cpu_accel_arch_stop_user()` (drivers/cpu_accel/cpu_accel.c:447), which escalates to the
owner-terminating NMI. `cpu_accel_user_nmi()` (cpu_accel.c:473) -- while the owner is in user
mode -- completes the owner exit (`x86_cpu_accel_user_stop_exit`) and redirects the owner to
`image->escape_ip`. The run ends (`ESCAPED`/`STOPPED`). **There is no save/restore: the owner
is not resumable today.**

### Design: make the owner resumable across a must-stop

Instead of redirecting to a terminal escape, the stop **preempts** the owner (save state), the
op runs on all CPUs, then the owner is **resumed** (restore state). The run stays `RUNNING`;
the gap becomes a latency spike.

**Preempt** (in the stop NMI, `cpu_accel_user_nmi`, cpu_accel.c:473):

- Capture the full user `pt_regs` (rip, rsp, rbp, callee- + arg GPRs, rflags) -- the workload's
  execution point.
- Keep `image->return_regs` (the kernel context to return to) -- already saved at
  `cpu_accel_arch_user_enter` (cpu_accel.c:1484).
- Ensure the **XSAVE/FPU/SIMD/MC** context is captured (not lost to lazy FPU handling).
- Keep the **user image mm + pinned stack** intact (the image is pinned before the NMI; do not
  unmap/tear it down on a resumable stop).
- Record a **wall-clock** preempt timestamp for the spike.
- Release mm/TLB ownership for the op via the existing `x86_cpu_accel_user_stop_exit` (so the
  op can use the CPU), remembering the returned TLB target set for the resume reconcile.

**Resolve** (the op runs): the must-stop op proceeds on all CPUs, including the quiesced owned
CPU, exactly as today.

**Resume** (after the op, at `x86_cpu_accel_maintenance_end`):

- Re-acquire mm/TLB ownership and reconcile TLBs (flush/rebuild) using the remembered targets.
- Restore XSAVE/FPU.
- Re-enter user mode at the saved rip/rsp -- the workload continues where it left off.
- The workload's next sample computes `lateness = now - (pre-preempt deadline)` = the whole
  gap -> the **latency spike**, landing in the histogram and bumping `max_lateness_ns` to the
  op's real (wall-clock) cost. This is the measurement the requirement asks for.

### State to preserve (checklist)

- [user regs]  `pt_regs` at the NMI: rip, rsp, rbp, rbx/r12-r15, rdi/rsi, rflags
- [user state] XSAVE/FPU/SIMD/MC (the workload's vector state)
- [mm]         `image->mm` ownership + the pinned image/stack pages (kept, not unmapped)
- [tlb]        the TLB target set from `x86_cpu_accel_user_stop_exit` (reconcile on resume)
- [kernel]     `image->return_regs` (the kernel context)
- [sampling]   **confirmed** -- for `user-oslat` the sampling runs in ring-3 (the user image,
               in the calling process's mm) and the counters live in the pinned mmap'd shared
               struct, so they survive the stop; no kernel sampling-counter save/restore is
               needed. (Kernel workloads, e.g. timestamp/memmove, run in `cpu_accel_run` with
               counters in kernel locals on the preserved kernel stack.)

### Op classification (resumable vs terminal)

**Resumable** (the op is transient; the CPU is usable again after it) -> preempt/resume:

- MTRR / CKSC update (arch/x86/kernel/cpu/mtrr/mtrr.c:259/412) -- memory types change + flush,
  then the CPU is usable.
- microcode load (arch/x86/kernel/cpu/microcode/core.c:775) -- arch-defined registers are not
  changed by a microcode load.
- `stop_machine`-style all-CPU stop (livepatch/kpatch, some memory ops) -- transient; CPUs
  resume. *(confirm the mechanism + whether it routes through the accel stop today)*

**Terminal** (cannot resume; the run ends in a **distinct** state, not a generic stop):

- reboot (arch/x86/kernel/reboot.c:679) -- the machine is going down.
- panic -- fatal.
- `cpu_down` / hotplug removing the target CPU -- that CPU is gone (resuming on another CPU is
  a separate, out-of-scope design).
- TDX / SEAMLDR (arch/x86/virt/vmx/tdx/seamldr.c:343) -- a SEAM/VMX state change; resuming
  ring-3 in the new SEAM state is not safe/possible.

A terminal op is one of the "fatal error" ends (e.g. `CPU_ACCEL_STATE_TERMINATED_BY_OP`),
preserving the invariant "the run ends only on duration or a fatal error."

### Correctness invariants

- The owned CPU is **fully quiesced** for the op (it physically stops) -- we are *not* skipping
  it (contrast with safe-to-skip). The op sees every CPU it needs stopped, including the owned
  one.
- Resume must not reintroduce stale TLB/mm that the op invalidated.
- **Re-entrancy:** a second must-stop arriving during the preempt/resume window must serialize
  with the existing maintenance-window join path (arch/x86/kernel/accel.c:585).

## Sampling clock (decision)

Constraint (user): the per-sample measurement must not lose resolution, so the hot sampling
path stays on the fast clock. Given that:

- **Per-sample steady-state lateness: keep `ktime_get_mono_fast_ns`.** It is the high-
  resolution clock and `lateness = now - deadline` is self-consistent on it. On this box the
  fast clock runs ~24% fast on the tickless CPU, so the *absolute* scale of the steady-state
  numbers (and the effective period, ~0.81 ms instead of 1 ms) carries a known offset -- but
  the *resolution* is preserved. Making the steady-state wall-clock-true would require the
  accurate clock in the spin (resolution loss) or an hrtimer schedule (changes what the probe
  measures); that is a separate, deferred fix (the fast-mono tickless refresh), out of scope now.
- **Run length: manager-owned** (Point 1) -- wall-clock accurate, off the tickless CPU.
- **The spike (Point 2): a dedicated wall-clock timestamp pair.** At the preempt boundary
  record `preempt_ts = ktime_get_ns()` (accurate); at resume record `resume_ts =
  ktime_get_ns()`; `spike_ns = resume_ts - preempt_ts` is the op's **true wall-clock cost**.
  This is a rare event (only on a must-stop op), so the accurate read adds no per-sample cost.
  Report it as its own metric (`preempt_count` / `preempt_max_ns` / `preempt_total_ns`) and
  also surface it in the histogram as a wall-clock lateness so it is visible beside the steady-
  state tail.

Net: the hot path never uses the slower clock (no resolution loss); the run length is wall-
clock (manager); the one measurement that *must* be wall-clock-true -- the op spike -- is taken
with the accurate clock, only when it happens.

## Implementation plan

**Phase A -- Point 1 (the durable cyclictest run; A1 + A2 together):**

1. **`libcpu_accel.c`** already provides the generic control API (incl. `attach_fd`) -- no
   extraction needed. `cpu-accel-cyclictest` links it directly.
2. **Driver / uapi: no change needed.** An orderly `STOP` (`cpu_accel_stop`) only sets
   `stop_requested` (cpu_accel.c:2380-2389); the probe image breaks on it and exits via
   `USER_EXIT` -> `cpu_accel_user_exit_locked`, which records `state=COMPLETE` for a non-escaped
   exit (cpu_accel.c:1594). So the manager's `STOP` already yields the intended `COMPLETE` end.
   (The `PERSISTENT`/watchdog interaction is handled in the probe program so the manager -- not a
   kernel timer -- is the duration authority.)
3. **`cpu-accel-cyclictest`**: new standalone program. Hosts its own periodic user image. **A1**:
   sub-integer (fixed-point) `cycles_per_ns` + the `* cpn` / `/ cpn` sites in the image.
   **A2**: `fork()` parent=manager (ticked CPU) / child=owner (open-ended image); the manager
   owns the wall-clock duration + sends the duration-quit `STOP`.
4. **Remove** `cpu-accelctl`'s built-in `user-oslat` (user image + timing) once (3) works.
5. Validate: a 12h run on CPU 6 completes at ~12h **wall** (RTC-confirmed), ~43.2M samples at a
   true ~1 ms cadence, correct absolute lateness scale, zero escapes.

**Phase B -- Point 2 (larger):**

4. Add the resumable-owner path: capture/restore in `cpu_accel_user_nmi` + a resume hook at
   `x86_cpu_accel_maintenance_end`.
5. Op classification table + the terminal-op distinct state.
6. Verify the spike measurement (the first post-resume sample records the gap).
7. Validation across the **full resumable set** (MTRR + microcode + `stop_machine`),
   incrementally -- confirm the owner survives each **and** the spike is measured. (Do the full
   set now, not a subset, so we do not come back to it.)

## Open questions

- (Resolved) `user-oslat` sampling runs in ring-3 and its counters are in the pinned mmap'd
  shared struct (see the state checklist); the resume path needs no kernel sampling-counter
  save/restore.
- `stop_machine`'s exact interaction with the accel stop today (does it route through the owner
  stop callback, or bypass it and take the owned CPU by another means?).
- Whether MTRR/microcode resume needs an explicit TLB flush of the owner's address space on
  re-entry, and how the remembered targets reconcile.
- Manager pinning policy: hard-require a ticked CPU, or detect + refuse if `cpu-accelctl` lands
  on the isolated CPU.

## Out of scope

- Migrating the owner to a different CPU after a `cpu_down` (separate design).
- The per-cpu kernel-work yield-and-resume (ruled out in the earlier doc).
- The global (kernel) TSC calibration. The observed drift was the tool's *local* integer-
  truncated calibration, fixed in A1; the kernel TSC rate is not the issue.
