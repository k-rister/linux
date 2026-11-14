# Phase 10 blast-radius reduction review-closure plan

## Purpose

This checklist defines what review closure means for commit
`bc4b96e4d9ec` on branch `codex/accelerator-blast-radius-v2`. The
implementation milestone is complete, but review closure requires explicit
evidence that the reduced workqueue API and the remaining semantic changes are
correct under ownership, error, teardown, and configuration boundaries.

The review population is:

- Three retained workqueue APIs: `cpu_accel_flush_workqueue()`,
  `cpu_accel_drain_workqueue()`, and `cpu_accel_destroy_workqueue()`.
- 218 call-site files that use those APIs.
- 89 semantic-remainder files outside the module and direct support tree.
- The x86, kernel, memory-management, IRQ, tracing, and driver-specific
  architectural changes represented by that remainder.

Review closure is achieved only when every item below has authoritative
evidence, no unchecked box remains, and the branch is ready to submit for
subsystem review without an open implementation or review caveat.

## 1. Wrapper API contract

### Requirements

- [ ] Audit `cpu_accel_flush_workqueue()` against original
  `flush_workqueue()` semantics.
- [ ] Audit `cpu_accel_drain_workqueue()` against original
  `drain_workqueue()` semantics.
- [ ] Audit `cpu_accel_destroy_workqueue()` against original
  `destroy_workqueue()` semantics.
- [ ] Confirm each wrapper preserves the original completion boundary and does
  not silently narrow or broaden the wait scope.
- [ ] Confirm each wrapper is safe when an accelerator owner is active on any
  CPU in the workqueue's allowed mask.
- [ ] Confirm each wrapper cannot sleep in an atomic context or deadlock with
  an owner exit handshake.
- [ ] Confirm failure and rollback paths cannot strand a CPU in owner mode or
  return with required work still pending.

### Required evidence

- A line-by-line review note for each wrapper implementation.
- A control-flow and lock-order analysis covering owner entry, owner exit,
  cancellation, teardown, and hotplug interactions.
- Targeted stress evidence for flush, drain, and destroy with active and
  exiting owners.

### Current state

- The three wrappers are implemented and the broad `*_with_sync_wait()` family
  is removed.
- A 10-minute four-CPU runtime comparison passed.
- The wrapper-level semantic audit is not yet complete.

## 2. Workqueue call-site audit

### Requirements

- [ ] Review all 218 retained call-site files.
- [ ] For each site, confirm the original work completion guarantee is
  preserved.
- [ ] For each site, confirm the original error-return semantics are
  preserved.
- [ ] For each site, confirm no early return leaves required work unfinished.
- [ ] For each site, confirm no path can execute on an owned CPU.
- [ ] For each site, confirm no path introduces a new sleepable operation in
  an atomic context.
- [ ] For each site, confirm no path changes cancellation, teardown, or
  resource-release ordering in a way that can deadlock or leak.
- [ ] Record the review verdict and any follow-up defect for each file.

### Required evidence

- A call-site review inventory with one row per file.
- A review verdict per file: `pass`, `defect`, `follow-up`, or
  `needs-subsystem-review`.
- A defect list with remediation status.
- A final machine-verifiable count proving all 218 files have a verdict.

### Current state

- The call-site population is identified.
- No file-by-file review verdict table exists yet.
- The 10-minute smoke test exercised the overall implementation, not each
  individual call site.

## 3. Semantic remainder audit

### Requirements

Review the 89 semantic-remainder files by architectural class:

- [ ] x86 ownership, entry, vector, IDT, IPI, TLB, and text-patching paths.
- [ ] Workqueue core and scheduling boundaries.
- [ ] SMP, stop-machine, CPU online/offline, and freeze/thaw paths.
- [ ] IRQ affinity reservation, migration, effective-route validation, and
  rollback.
- [ ] MMU gather, rmap, reclaim, vmalloc, SLUB, and TLB shootdown paths.
- [ ] Debug, tracing, kprobe, ftrace, jump-label, alternative, MTRR,
  cache-info, and ring-buffer paths.
- [ ] Required driver-specific changes, especially target TMR, Greybus
  release, and BCM PHY PTP ordering.
- [ ] Include-path and small required call-site adjustments.

For each class:

- [ ] Verify state transitions and lifetime rules.
- [ ] Verify lock ordering and deadlock avoidance.
- [ ] Verify error and rollback paths.
- [ ] Verify hotplug and owner-exit interactions.
- [ ] Verify no unsafe execution on an owned CPU.
- [ ] Verify no silent semantic change outside the accelerator contract.

### Required evidence

- A per-class review note.
- A per-file review verdict for all 89 semantic files.
- Targeted tests or reasoned invariants for paths that cannot be exercised on
  Simon.
- A final machine-verifiable count proving all 89 files have a verdict.

### Current state

- The semantic population and class breakdown are documented in
  `Documentation/accel/cpu-accel-blast-radius-report.html`.
- The per-class and per-file reviews are not yet complete.

## 4. Build and configuration audit

### Requirements

- [ ] Build with `CONFIG_CPU_ACCEL_CORE=y`.
- [ ] Build with `CONFIG_CPU_ACCEL_CORE=n`.
- [ ] Build an x86-64 production-like configuration.
- [ ] Build the focused accelerator tooling.
- [ ] Confirm no new compiler warnings in the changed files.
- [ ] Confirm disabled-config fallbacks compile and are no-ops.
- [ ] Confirm the dedicated vector and IDT entry are config-gated.
- [ ] Confirm `make SPHINXDIRS=accel htmldocs` passes.
- [ ] Confirm `git diff --check` passes.

### Required evidence

- Build logs for both config states.
- Compiler-warning inventory for changed files.
- Sphinx build log.
- Whitespace-check result.

### Current state

- The focused userspace build and accelerator Sphinx build pass.
- `git diff --check` passes on the committed tree.
- Full both-config build coverage is not yet recorded.

## 5. Regression and stress validation

### Requirements

- [ ] Run `tools/cpu_accel/test-phase10-smp-gate.sh`.
- [ ] Run `tools/cpu_accel/test-phase9-freeze-gate.sh`.
- [ ] Run the UAPI contract, lifecycle, ABI 23 compatibility, and SDK
  fallback tests.
- [ ] Run the four-CPU `user-oslat` versus rt-tests `oslat` comparison.
- [ ] Run a targeted workqueue stress test for each retained wrapper.
- [ ] Run targeted tests for each architectural class in section 3.
- [ ] Run subsystem-specific tests for target TMR, Greybus, and BCM PHY PTP
  changes.
- [ ] Record pass/fail evidence and any environment limitations.

### Required evidence

- One artifact per test with command, environment, result, and timestamp.
- A final summary that distinguishes passed tests from deferred or
  environment-gated tests.
- A stress matrix showing owner-active and owner-exit coverage.

### Current state

- The 10-minute four-CPU comparison passed.
- The focused tool build and accelerator documentation build passed.
- The Phase 10 and Phase 9 gates have prior passing evidence, but the current
  committed tree has not yet rerun the complete gate set.

## 6. Documentation and evidence audit

### Requirements

- [ ] Verify `Documentation/accel/cpu-accel-blast-radius-report.html`
  accurately describes the committed tree.
- [ ] Verify
  `Documentation/accel/phase10-blast-radius-reduction-2026-11-14.md`
  matches the retained evidence.
- [ ] Verify `Documentation/accel/phase10-smp-design.md` cross-links the
  reduction record.
- [ ] Verify wrapper documentation matches implementation.
- [ ] Verify no stale `_with_sync_wait()` references remain.
- [ ] Verify all numbers in the reports match the current commit.
- [ ] Verify the session transcript remains untracked and excluded.

### Required evidence

- A documentation review note.
- A search result proving no stale broad-wrapper references.
- A cross-link and consistency check.
- A report-versus-commit metric comparison.

### Current state

- The current reports exist and the broad-wrapper reference search is clean.
- Report-to-commit metric reconciliation and full documentation review are not
  yet complete.

## 7. Subsystem review readiness

### Requirements

- [ ] Obtain or prepare an x86 architecture review.
- [ ] Obtain or prepare a workqueue review.
- [ ] Obtain or prepare a memory-management review.
- [ ] Obtain or prepare an IRQ and CPU-hotplug review.
- [ ] Obtain or prepare an SMP and stop-machine review.
- [ ] Obtain or prepare a tracing and executable-memory review.
- [ ] Obtain or prepare a driver-specific review for target TMR, Greybus, and
  BCM PHY PTP.
- [ ] Confirm each review has either a sign-off or a documented follow-up.

### Required evidence

- Review notes or maintainer feedback.
- A final review matrix mapping each subsystem to its verdict.
- A list of any unresolved review findings.

### Current state

- No external subsystem review has been requested yet.
- The current branch is pushed and ready for review.

## 8. Final repository state

### Requirements

- [ ] Rebase or otherwise integrate the latest intended base.
- [ ] Preserve a clean commit history with clear review boundaries.
- [ ] Keep AI attribution trailers on AI-authored commits.
- [ ] Remove temporary or generated files from the review diff.
- [ ] Confirm the branch is pushed.
- [ ] Confirm no unintended untracked files remain.
- [ ] Confirm the final commit contains the review-closure evidence.

### Required evidence

- `git status --short --branch` output.
- `git log` output for the final review sequence.
- Remote branch tracking status.
- A final review-closure summary in the repository.

### Current state

- Branch `codex/accelerator-blast-radius-v2` is pushed and tracks
  `origin/codex/accelerator-blast-radius-v2`.
- Commit `bc4b96e4d9ec` is the implementation inflection point.
- The session transcript is intentionally untracked and is not part of the
  review diff.
- Review-closure commits are not yet present.

## Completion definition

The blast-radius reduction plan is review-closed when:

1. Every requirement in sections 1 through 8 has a checked box.
2. Every required artifact exists in the repository or in a referenced
   canonical evidence location.
3. Every review finding is either fixed or explicitly accepted with a reason.
4. Every automated and stress gate that is not environment-limited passes.
5. Every environment-limited gate has a recorded limitation and a targeted
   replacement invariant.
6. The final commit contains the closure evidence and is pushed.
