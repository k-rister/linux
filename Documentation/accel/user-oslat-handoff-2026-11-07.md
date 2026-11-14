# CPU accelerator user-oslat validation handoff

Date: 2026-11-07
Repository: `/home/krister/github/linux`
Branch: `codex/accelerator-oslat`
Follows: `phase8-handoff-2026-10-30.md` (Phase 8, adversarial isolation validation —
substantially complete). This handoff covers the user-oslat data-accuracy fix and the
rt-tests `oslat` side-by-side validation work stream.

## Status

The `cpu-accel user-oslat` data-accuracy anomaly is resolved and validated. The
`avg_lateness_ns` artifact (read ~99–102 ns on 600 s runs) was diagnosed as a
**32-bit histogram-bin wrap**, not the earlier "ABI layout mismatch" theory (that
diagnosis was wrong and is corrected). The fix — ABI 21, `lateness_hist`/`linear_ns_hist`
promoted `__u32` → `__u64` — is committed, deployed, and **verified at both 600 s and
24 h scale**: the module `avg_lateness_ns` now equals the 1-ns histogram mean at up to
4.55×10¹² samples, with no counter wrap. A concurrent side-by-side comparison
(`user-oslat` on an owned CPU vs rt-tests `oslat` on a non-owned FIFO CPU) confirms the
owned CPU is both cleaner and leaner.

The work moved from the Phase 8 VM (`accelerator-dev`) to the **simon.lab box**
(`192.168.12.219`), so the Phase 8 VM-specific next steps need re-mapping.

### The data-accuracy fix (ABI 21)

- **Root cause:** `lateness_hist` and `linear_ns_hist` were `__u32`. A long run puts
  >2³² samples in the dominant 10/20-ns bin, so the bin wraps and understates
  `hist_total` (the divisor). `sum_lateness_ns` is `uint64` (≈ total loop time ≈
  duration, never wraps), so `avg = sum_lateness / wrapped_hist_total` came out ~7× too
  high. The 1-ns histogram *mean* (sum/count, both wrapped ~proportionally) read ~18 ns
  and looked correct, which masked the bug. 60 s runs (<2³² samples) were consistent.
- **Fix (commit `39743a3`):** both histograms `__u32` → `__u64` (struct 41 656 → 50 104 B,
  still within the 64 KB map); tool prints → `PRIu64`; retains the ABI-20 `offsetof`
  layout-fingerprint + exact `struct_size ==` gate. ABI version 20 → 21.
- **Deployed:** `cpu_accel_abi21.ko` (srcversion `54621BD9`) + `cpu-accelctl-abi21` on the
  box, sha256-verified.

### Verification (concurrent A/B, owned CPU 3 vs non-owned FIFO CPU 2)

| run | samples | avg (module) | 1-ns mean | 20-ns bin | iters/s |
|-----|---------|--------------|-----------|-----------|---------|
| 600 s | 3.17×10¹⁰ | 18 ns | 18.93 ns | 2.83×10¹⁰ | 52.8 M |
| 24 h | 4.551×10¹² | 18 ns | 18.99 ns | 4.09×10¹² | 52.67 M |

- `avg_lateness_ns` == 1-ns histogram mean in both runs (the fix holds at scale).
- The 24 h 20-ns bin (4.09×10¹², ~1000× 2³²) is held exactly as `__u64` — no wrap.
- Weighted 1-ns sum ≈ duration in both runs (loop ran the full time).
- Owned CPU stayed clean all day at 24 h: 0 context switches, 0 arch IPIs, 0 escapes,
  worst sample ≤140 ns.

### Findings (cpu-accel `user-oslat` vs rt-tests `oslat`)

- Both average ~18–22 ns. The **owned CPU** (cpu-accel, CFS, isolated) is tighter (all
  percentiles ~20 ns, no >1 µs events, 0 IPIs/ctx switches) and runs a leaner loop
  (~52.7 vs ~45.8 M iters/s). The **non-owned FIFO CPU** (oslat) carries a `~1.45 µs`
  housekeeping-IPI cluster that scales ~linearly with run duration (~590 K over 600 s vs
  ~86 M over 24 h) and, over 24 h, a single ~23.18 µs outlier.
- 64-bit counters (ABI 21) give ~10⁴ years of headroom at observed rates — safe for any
  realistic run length up to the 24 h max.

## Repository state

- Worktree clean; branch in sync with `origin` (all pushed). HEAD `d16a5ac600cc`.
- Commits in this work stream (newest first):
  - `d16a5ac` Documentation: accel: add full /proc/cmdline + kernel note to side-by-side reports
  - `368e11a` Documentation: accel: add ABI-21 user-oslat vs oslat 24h comparison
  - `e6d3b89` Documentation: accel: add ABI-21 user-oslat vs oslat 600s comparison
  - `80d269e` Documentation: accel: add corrected user-oslat side-by-side report
  - `39743a3` cpu_accel: fix user-oslat avg_lateness with 64-bit histograms (ABI 21)
  - `da6138e` cpu_accel: merge cpu-accel-cyclictest into cpu-accelctl
  - `349e5f0` cpu_accel: add user-oslat tight-loop latency probe
- `origin` head: `d16a5ac600cc` (pushed).

## Environment

- **Box:** `192.168.12.219` (simon.lab), root ssh. Use `-o BatchMode=yes -F /dev/null`
  and detach stdin (`< /dev/null`) for any backgrounded run so it survives the SSH
  session.
- **Running kernel (authoritative):** `7.3.0-rc3-accel-tlbfix-g635d5b061dd8`.
  **Note:** `/proc/cmdline` `BOOT_IMAGE` names `…accel-phase8-84`, but `uname -r` and the
  ABI-21 module's vermagic both confirm the running kernel is
  `…accel-tlbfix-g635d5b061dd8` — the `BOOT_IMAGE` filename is a stale/mismatched label.
- **cmdline (full):**
  `BOOT_IMAGE=(hd0,gpt2)/vmlinuz-7.3.0-rc3-accel-phase8-84 earlycon=ttyS0,115200 console=ttyS0,115200 root=/dev/mapper/almalinux-root ro crashkernel=2G-64G:256M,64G-:512M resume=UUID=1a4b3557-9483-455f-be24-1ad6a1fb957c rd.lvm.lv=almalinux/root rd.lvm.lv=almalinux/swap nosmt iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=16 amd_iommu=on skew_tick=1 tsc=reliable rcupdate.rcu_normal_after_boot=1 rcutree.nohz_full_patience_delay=1000 nohz=on nohz_full=1-11 rcu_nocbs=1-11 tuned.non_isolcpus=00000001 systemd.cpu_affinity=0 intel_pstate=disable nosoftlockup`
- **Isolation:** `nohz_full=1-11 rcu_nocbs=1-11 skew_tick=1 nosmt`; CPU 0 housekeeping;
  SMT off. Latency-relevant tuning: `intel_pstate=disable`, `tsc=reliable`, `iommu=pt`,
  `hugepages=16` (1G), `nosoftlockup`, `amd_iommu=on`.
- **CPU assignment:** cpu-accel owns CPU 3 (user-oslat); oslat runs CPU 2 (FIFO 99); the
  cpu-accel manager self-pins to CPU 0.
- **Deployed artifacts** (`/root/accel-measure/`): `cpu_accel_abi21.ko` (srcversion
  `54621BD9`), `cpu-accelctl-abi21`, `oslat` (rt-tests 2.11). Run outputs in
  `/root/accel-measure/compare/` (`accel_cpu3_{600s,24h}.txt`, `oslat_cpu2_{600s,24h}.txt`).
- **kbuild caveat:** the incremental build did NOT detect the `cpu_accel.h` change (same
  `.ko` size + symbol-based srcversion); a forced clean rebuild (`rm <obj> .o .ko && make`)
  was required. `BUILD_BUG_ON(sizeof(struct shared) <= MAP_SIZE)` passed at 50 104 B.

## Next steps

1. **(comparison thread)** Enable ftrace/tracing on the box (the `tracing_enabled` write
   was permission-denied) to pinpoint the *source* of the `~1.45 µs` housekeeping-IPI
   cluster on the non-owned CPU — currently characterized, not identified.
2. **(Phase 8 remainder)** Re-map and resume fault-injection coverage on the simon.lab
   box: device-management matrix, resctrl, memory-pressure envelope; source-audit rows
   (microcode / suspend / kexec / reboot-panic); hardware rows (MCE / INVLPGB /
   irq_storm).
3. **(optional)** If runs longer than 24 h are wanted, raise
   `CPU_ACCEL_MAX_DURATION_NS` (currently exactly 24 h; the check is
   `duration_ns > MAX`, so `== MAX` is accepted).
4. **(optional)** Upstream the ABI-21 data-accuracy fix + the user-oslat workload.

## Constraints

- Never commit unreviewed work; no unreviewed stress on the box.
- Keep changes driver-sized.
- 24 h is the current max user run duration; the owned CPU spins 100 % for the whole run.
  For long unattended runs use `--persistent` (kernel watchdog = clean stop at the
  deadline) + `nohup`/detached stdin, and keep the box up and CPUs 2/3 untouched.
- Both latency histograms are 64-bit (ABI 21): no overflow up to ~10⁴ years at observed
  rates.
- MemPalace memory is current as of this handoff: KG facts for `cpu-accel user-oslat
  avg_lateness` (root_cause, fix, verified, 24h_scaling, concurrent_ab_verified,
  24h_run_verified), `simon.lab box 192.168.12.219` (kernel_bootimage_mismatch,
  cmdline_latency_tuning), and `cpu-accel user-oslat vs rt-tests oslat`
  (comparison_600s, ipi_cluster_scales_with_duration); drawers in
  `linux-dataplane-accelerator`; agent diary in `wing_opencode`.

---

— AI-signed: opencode | model: radiance/qwen3.8-27b | effort: not specified |
contribution: authored this handoff from the session's committed work and verification data.
