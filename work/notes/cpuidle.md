# CPU idle: cluster power-down (CPD)

Status 2026-11-13: working on cluster 1 (sections 5-6). Tracker group: cpuidle-psci RFC + exynos-pmu + DT.
Plan: cluster 1 only, as the vendor does. Cluster 0 holds the CPUCL0 watchdog (suspend.md
item 2), and the vendor never powers it down ("no benefit in boot cluster").

## 1. Registers (PMU 0x10480000)

| reg | CL0 | CL1 | boot value (CL0 / CL1) |
|---|---|---|---|
| NONCPU_CONFIGURATION | 0x2400 | 0x2420 | 0x0f0f / 0x0f0f0f |
| NONCPU_STATUS | 0x2404 | 0x2424 | 0x0008000f (on) |
| NONCPU_OPTION | 0x2408 | 0x2428 | 0x161 |
| CPUSEQUENCER_CONFIGURATION | 0x2480 | 0x24a0 | 0 |
| CPUSEQUENCER_STATUS | 0x2484 | 0x24a4 | 1 |
| CPUSEQUENCER_OPTION | 0x2488 | 0x24a8 | 0: **bit 0 = power the cluster down after its last core** |
| L2_CONFIGURATION | 0x2600 | 0x2620 | 0x707 |
| L2_STATUS | 0x2604 | 0x2624 | 7 (on) |
| L2_OPTION | 0x2608 | 0x2628 | 0 |
| ACK_LAST_CPU | 0x0940 | | |
| CPUn_OPTION (n = cluster*4+cpu) | 0x2008 + n*0x80 | | firmware sets bit 17 around a cluster/system entry |

Runtime survey on our kernel (core power-down only): 300/300 samples of CL1 NONCPU/L2/CPUSEQ
status show "on". Cluster 1 never powers down today.

## 2. Vendor kernel (exynos-powermode.c, exynos-pmu.c, exynos-mp-cpufreq.c)

- enter_c2(): after the core power-down setup, it checks whether this CPU is the last one in
  cluster 1 (all others in c2_mask) and whether every cluster-1 CPU's next event is at least
  cpd_residency (3000 us) away. If so: CPUSEQUENCER_OPTION |= 1 and PSCI power_state
  0x01000000 (afflvl 1, type 0, id 0).
- wakeup_from_c2(): the first CPU to wake clears the bit again.
- The boot cluster is never powered down. SICD (power_state id 1) is a separate feature.
- cpufreq blocks CPD around every cluster-1 frequency change and sends an IPI to wake a
  sleeping cluster first (exynos_disable_cpd()).
- The CPU_STARTING hotplug notifier clears the bit for non-boot clusters.

## 3. Firmware (BL31, "Cubic-OPM" ATF fork, May 2018)

Read-only, local only: mmcblk0boot0 backup offset 0x2000..0x10300 (0x10-byte header), load base
0x02024000. Ghidra via REA. Offsets below are relative to the blob.

- SMC dispatch 0x7014: CPU_SUSPEND 0x84000001/0xc4000001 -> 0xc080 -> 0x6f7c. That passes
  afflvl = power_state[25:24] and the whole power_state to plat affinst_suspend (0x50b4, ops
  table at 0xc2f0). **The type bit (16) is never read**, so mainline's 0x01010000 is
  equivalent to the vendor's 0x01000000. afflvl 3 = system (SMC 0x82000003).
- 0x50b4 affinst_suspend: records the per-CPU state (8 = core, 0x10 = cluster), then
  0xadec(afflvl, id = power_state[15:0]).
- 0xadb8 mode: id != 0 -> 2 (system idle/SICD); afflvl != 0 -> 1 on cluster 1, **2 on
  cluster 0**; else 0 (core). Modes 1/2 go through 0xad54/0xac70: set CPUn_OPTION bit 17,
  require exactly one ACK_LAST_CPU bit in the cluster and exactly one cluster CPU whose
  STATUS[23:16] != 0x4b. Otherwise they abort, and CPU_SUSPEND returns without sleeping.
- 0xadec: afflvl 0 -> clean+inv to LoUIS (L1). afflvl != 0 -> clean+inv to LoC (**L2
  flushed**). Then it clears CPUECTLR.SMPEN. For a cluster request, 0xabd4 runs under a
  firmware lock: if this is the only powered-on CPU of the cluster (CPUn_STATUS[3:0]), it
  flushes again, turns off CCI-400 snoops/DVM for the cluster (0x10700000 + 0x4000/0x5000) and
  goes to WFI. Otherwise it returns -1, and the CPU returns to Linux without sleeping (cpuidle
  counts that as a failed entry).
- **CPU_SUSPEND never writes CPUSEQUENCER_OPTION.** Without the kernel setting it, the
  cluster stays powered even after a cluster request.
- CPU_OFF (0x51e8, hotplug) *does* set CPUSEQUENCER_OPTION bit 0 itself, when the CPU is on
  cluster 1 (MPIDR bit 8) and it is the last one, then requests a cluster power-down.
- The cluster power-up that clears the bit (0xb98c, also 0xb970) runs only on the BL31
  cold-boot path, not on a warm boot. **After a cluster suspend the kernel must clear the bit
  itself**, as the vendor does. If the bit stayed set and the last core later entered a
  core-only state (L1 flush only), the cluster could power down with dirty L2 lines.

## 4. Consequences for mainline

- The DT cluster state: arm,psci-suspend-param = <0x1010000> (id 0!), on cpu4-7 only.
- The kernel must set CPUSEQUENCER_OPTION(CL1) bit 0 **only** when the last cluster-1 CPU
  enters the cluster state, and clear it when any cluster-1 CPU comes back (exit or failed
  entry). The firmware double-checks last man under its own lock and aborts if wrong, so a
  kernel race costs a failed entry, not data. The real hazard is a stale bit together with a
  core-only entry.
- PSCI 0.1 has no OSI, so cpuidle-psci-domain runs in PC mode (genpd always on). The cluster
  state has to be a flattened second state. The CPU PM notifier does not know which state was
  chosen, so the kernel needs a way to learn it (open design point).
- cpufreq on policy4 while cluster 1 is down: still untested whether CMU_CPUCL1 is reachable or
  keeps its state. The vendor blocks CPD around it.

## 5. Implementation (2026-11-13, kernel commits e5d7bdb98b1b, 43474abbcafb, cafe936e74df)

- cpuidle-psci: `psci_cpuidle_set_state_fixup()`, an optional platform callback that runs
  before a non-WFI state is entered and returns the power state to request.
- exynos-pmu (`pmu_cluster_seq`): the fixup grants afflvl 1 on cluster 1 only if all other
  online cluster-1 CPUs are in power-down (CPU_PM_ENTER seen) and none of them has
  `cpuidle_device.next_hrtimer` within 3 ms. Otherwise it masks out afflvl, which gives the
  core state. The CPU PM notifier writes CPUSEQUENCER_OPTION bit 0 = (granted request) on
  every cluster-1 ENTER, and clears it on every EXIT/ENTER_FAILED, under a raw spinlock.
- DT: `cluster_sleep` (0x1010000, entry 150 / exit 250 / residency 3000 us; the latencies are
  estimates), on cpu4-7 only.

## 6. Results (Alpine, a3-install-kernel)

- `tools/probes/cpd-state.sh`. For a realistic picture, sample pinned to CPU0 (`taskset -c 0`,
  util-linux-misc installed on the Alpine rootfs): idle, 0.1 s interval, CL1 NONCPU_STATUS
  0x005e0000 (off) in 288/300 samples. L2_STATUS off = 0x10080000.
- About 7% of cluster-state entries are rejected (cpuidle stateN/rejected): the firmware's
  last-man check (STATUS[23:16] == 0x4b) races cores that have passed CPU_PM_ENTER but are
  not down yet. Harmless: the CPU returns and idles again.
- Dirty-L2 test: `cp` 256 KiB on cpu4-7, sleep 0.5 s (cluster off), sha256 on cpu0: 100/100
  ok. Read test with an 8 MiB file, 150/150 ok.
- cpufreq: CMU_CPUCL1 can be written while cluster 1 is off, and the rate survives the
  power-down (sha256 of 48 MiB: cpu4 1.50 s = cpu0 1.50 s at 1248 MHz). Unlike the vendor,
  no blocking around frequency changes is needed.
- Hotplug cpu4-7 off: the firmware sets the sequencer bit itself (0x1); after cpu5 is back
  online it reads 0. state2 keeps counting.
- s2idle with RTC wake: ok.
- Not done yet: battery current comparison (needs the cable out); cluster 0 stays core-only.
