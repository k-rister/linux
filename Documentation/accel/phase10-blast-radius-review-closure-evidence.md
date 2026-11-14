# Phase 10 blast-radius review-closure evidence

## Scope

This record summarizes the authoritative build, runtime, documentation, and
review-readiness evidence for the Phase 10 blast-radius review-closure branch.
The review baseline is `40288c9206c17eb66a603262e06a58d300d0f279`; the
runtime candidate is commit `6f200ac0bcad9b46fbe106641f0f2c0cdb9d57f5`.

## Build and configuration audit

| Item | Result | Evidence |
|---|---|---|
| Production-like x86-64 build, `CONFIG_CPU_ACCEL_CORE=y` | Pass | `/tmp/build-simon-prod/build-verification.log` |
| Accelerator-disabled full build, `CONFIG_CPU_ACCEL` unset | Pass | `/tmp/build-simon-prod-core-n/build-verification.log` |
| Focused accelerator tooling build | Pass | `/tmp/cpu-accel-tool-build-verification.log` |
| Accelerator Sphinx build | Pass | `/tmp/accel-sphinx-build.log` |
| `git diff --check` | Pass | Final worktree check, exit `0` |

### Production-like build

- Command: `make -C /tmp/build-simon-prod -s`
- Source commit: `6f200ac0bcad9b46fbe106641f0f2c0cdb9d57f5`
- Configuration: `CONFIG_CPU_ACCEL_CORE=y`, `CONFIG_CPU_ACCEL=m`
- Exit code: `0`
- `bzImage` SHA-256: `604912dc5957b19b1085336a7cfa6a7f3d62476e44eacef392178882ad108054`

### Accelerator-disabled build

- Command: `make -C /tmp/build-simon-prod-core-n -s`
- Source commit: `6f200ac0bcad9b46fbe106641f0f2c0cdb9d57f5`
- Configuration: `# CONFIG_CPU_ACCEL is not set`
- Exit code: `0`
- `bzImage` SHA-256: `ec88451cd547d8a8eae9bbfb74278c7ea1c48ad7118b3c687dc8d66ec3212bad`

### Focused tooling

- Command: `make -C tools/cpu_accel cpu-accel-testctl test-uapi-contract test-uapi-lifecycle test-uapi-abi23-compat test-uapi-sdk-fallback libcpu-accel-sdk-fallback.so test-sync-wait-prime`
- Exit code: `0`
- Current control-tool SHA-256: `6fbd77a46d7a45d43dfa7bad620bac624480739bc0618df3fb5fc38dc3a66690`

No compiler warning was emitted for the changed files in either full build or
the focused tool build. The disabled configuration compiles the fallback paths;
the dedicated x86 vector and IDT entry are config-gated.

## Runtime gates

All runtime gates ran on `simon.lab.gtristers.consone.net` with kernel
`7.3.0-rc3-accel-tlbfix-00522-gda8c5e8e7e62` and the exact current control
tool SHA-256
`6fbd77a46d7a45d43dfa7bad620bac624480739bc0618df3fb5fc38dc3a66690`.

| Gate | Command | Result | Evidence |
|---|---|---|---|
| Phase 10 SMP gate | `CPU_ACCEL_PHASE10_CPUS=5,7 CPU_ACCEL_TESTCTL=... test-phase10-smp-gate.sh` | Pass, exit `0` | `/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/phase10-smp-gate.log` |
| Phase 9 freeze gate | `CPU_ACCEL_FREEZE_GATE_CPU=5 CPU_ACCEL_TESTCTL=... test-phase9-freeze-gate.sh` | Pass, exit `0` | `/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/phase9-freeze-gate.log` |
| `sync_workqueue_flush` stress | `CPU_ACCEL_TEST_CASE=sync_workqueue_flush ... test-sync-work-owner-stop.sh` | Pass; owner stopped in `3 ms` | `/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/wrapper-sync-workqueue-flush-abi24-current/run.log` |
| `sync_workqueue_drain` stress | `CPU_ACCEL_TEST_CASE=sync_workqueue_drain ... test-sync-work-owner-stop.sh` | Pass; owner stopped in `3 ms` | `/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/wrapper-sync-workqueue-drain-abi24-current/run.log` |
| `sync_workqueue_destroy` stress | `CPU_ACCEL_TEST_CASE=sync_workqueue_destroy ... test-sync-work-owner-stop.sh` | Pass; owner stopped in `3 ms` | `/root/accel-phase10/evidence/phase10-6f200ac0bcad-20261114T2205Z-current-gates/wrapper-sync-workqueue-destroy-abi24-current/run.log` |

The Phase 9 freeze gate includes the UAPI contract, lifecycle, ABI 23
compatibility, and SDK fallback tests.

## Four-CPU latency comparison

The exact-current-binary 10-minute four-CPU comparison passed from
`2026-11-14T22:15:15Z` to `2026-11-14T22:25:16Z`.

| Tool | Samples | Weighted mean (ns) | Max (ns) | p50/p90/p99/p999 (ns) | Samples >1 µs |
|---|---:|---:|---:|---|---:|
| `user-oslat` | 118,940,989,088 | 20.178044682 | 250 | 20/20/30/30 | 0 |
| rt-tests `oslat` | 105,715,488,032 | 22.702276206 | 8,773 | 20/30/30/30 | 2,381,879 |

Both tools exited `0`; the module refcount returned to `0`, and no new NVMe
QID 2 timeout appeared. The full agreed-format report is
`phase10-blast-radius-reduction-2026-11-14.md`.

Canonical raw artifacts:

- `/root/accel-measure/phase10-blast-v3-4cpu-user-oslat-and-rt-tests-oslat-10m-20261114T2215Z-current`
- `/root/accel-measure/phase10-blast-v3-scheduler-observation-20261114T2235Z-current`

## Architectural-class validation

| Class | Validation evidence | Result | Environment limitation |
|---|---|---|---|
| x86 architecture | Production and accelerator-disabled full builds; Phase 10 SMP gate; Phase 9 freeze gate; `phase10-blast-radius-x86-audit.md` | Pass | None |
| Workqueue and scheduling | Wrapper stress matrix; 218-file call-site audit; `phase10-blast-radius-wrapper-api-audit.md`; `phase10-blast-radius-wrapper-concurrency-audit.md` | Pass | None |
| Memory management | Accelerator-disabled build; internal MMU/reclaim audit; config-gated fallback review | Pass | None |
| IRQ and CPU hotplug | Phase 9 freeze gate; internal IRQ-affinity and CPU-hotplug audits | Pass | None |
| SMP and stop-machine | Phase 10 SMP gate; internal SMP/stop-machine audit | Pass | None |
| Tracing and executable memory | Accelerator-disabled build; internal debug/tracing audit; config-gated fallback review | Pass | None |
| Driver-specific changes | Internal driver-specific audit; targeted source and invariant review | Pass | Target hardware runtime validation is environment-gated |
| Include paths and small call sites | Internal include-path/small-call-site audit | Pass | None |

The driver-specific limitation is accepted because the touched target TMR,
Greybus, and BCM PHY PTP paths are not exercisable on the available Simon
host. The internal audits and config/build gates provide the replacement
invariant coverage for those paths.

## Documentation and evidence audit

- `cpu-accel-blast-radius-report.html` was reconciled against the current
  tracked diff; the full tracked diff is 436 files, 40,799 insertions, and 876
  deletions.
- `phase10-smp-design.md` cross-links
  `phase10-blast-radius-reduction-2026-11-14.md`.
- Wrapper API and concurrency documentation match the retained implementation
  and current ABI 24 stress evidence.
- No stale `_with_sync_wait()` implementation reference remains.
- The session transcript remains intentionally untracked and excluded from the
  review diff.

## Subsystem review readiness

Internal review readiness is complete and recorded in
`phase10-blast-radius-subsystem-review-matrix.md`. External maintainer reviews
remain documented follow-ups.
