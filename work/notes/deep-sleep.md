# Deep sleep (suspend-to-RAM, `mem`): how it works on the 7870

Status 2026-11-14 (night): **deep sleep works on the clean series**: entry, RTC wake, display
resume (blanked or on, fbcon bound), touch keys without read errors. 11 of 12 real cycles on the
series without debug code passed (8 in a row with tools/probes/mem-cycles); one cycle did not
come back and left no log (section 8.4, treated as a fluke until it repeats). Root causes and
takeaways: section 9. Still to do: the user's look at the screen and a touch key press after a
wake, the other wake sources, S2MPU05 sleep modes (needs the user's yes), drain against s2idle.

Kernel (a3y17lte, not pushed; SHAs change with every rebase, so by subject; tags
a3y17lte-pre-sleep-fold and a3y17lte-pre-sleep-reorder keep the earlier tips):
- pinctrl: samsung: Add Exynos7870 pad retention control
- arm64: dts: exynos7870-a3y17lte: Add power-down pin states (CON_PDN/PUD_PDN = stock)
- clk: samsung: exynos7870: Switch the PHY clock muxes to OSCCLK for suspend (the DISPAUD part is
  required for the entry; the USB and CSI parts follow the vendor, not tested alone)
- clk: samsung: exynos7870: Enable the CMU_MIF top clocks for suspend (the entry fix, section 7)
- soc: samsung: exynos-pmu: Add Exynos7870 system sleep (CPU0 local power configuration kept on
  for the PSCI call, as the vendor; that part was never tested alone)
- drm/exynos: exynos7_drm_decon: set up the Exynos7870 content firewall (section 8.1)
- drm/bridge: samsung-dsim: Defer the HS clock request on Exynos7870 (section 8.2)
- Input: tm2-touchkey - enable the interrupt after powering up on resume (section 10)
- Input: imagis - power-cycle the chip if it does not answer at probe
- Dropped: the DSIM M_PLL_CTRL1 write (0x98 = 0x40000000, vendor and S-Boot value). The display
  works with the register at 0 (#153, twice). The commit is still in a3y17lte-pre-sleep-reorder.
With the suspend ops in place `mem_sleep` defaults to `deep`.

Deep sleep is a PMU-sequenced system power-down. The kernel programs the PMU (per-block SYS_PWR
table, central sequencers, wake masks), GPIO power-down states and pad retention, then calls PSCI
CPU_SUSPEND at affinity level 3. Wake is a warm reset: iROM -> BL1 -> BL31 -> the kernel's resume
entry. S-Boot and our U-Boot are not on the wake path.

## 1. Vendor kernel (src/vendor-kernel)

- drivers/soc/samsung/exynos-pm.c: suspend_ops `.enter` = `cpu_suspend(PSCI_SYSTEM_SLEEP)` on CPU0
  (nonboot CPUs already offline). syscore suspend -> `exynos_prepare_sys_powerdown(SYS_SLEEP)`;
  syscore resume -> `exynos_wakeup_sys_powerdown(SYS_SLEEP, early_wakeup)`, then prints PMU
  WAKEUP_STAT (0x600) and the pending alive EINTs.
- arch/arm64/kernel/psci.c `psci_suspend_customized_finisher`: PSCI_SYSTEM_SLEEP = CPU_SUSPEND
  with afflvl 3, id 0, type 0 (power_state 0x03000000).
- drivers/soc/samsung/exynos-powermode.c, prepare: EINT_WAKEUP_MASK 0x60C (pinctrl wake mask),
  WAKEUP_MASK/2/3 0x610/614/618 from DT; stock SLEEP values 0x50006e1e / 0xffff00ff / 0xffff00ff
  (set bit = masked; WAKEUP_MASK bit 0 = EINT stays unmasked). Then `cal_pm_enter(SYS_SLEEP)`.
- drivers/soc/samsung/pwrcal/S5E7870/S5E7870-syspwr.c (offsets: S5E7870-pmusfr.h):
  - Init once (`syspwr_init`): RESET_LPI_TIMEOUT bit 0; OPTION feedback for TOP_PWR, TOP_PWR_MIF,
    DISPAUD, MFCMSCL, ISP, G3D, MEMORY_G3D; L2_OPTION both clusters: clear AUTOMATIC_L2FLUSHREQ (17)
    and RETENTION (4), set STANDBYWFIL2 (16); CPUn_OPTION: SC_COUNTER, SMPEN (28), STANDBYWFI (16),
    MEMPWRDOWN_FEEDBACK (3), clear STANDBYWFE and SC_FEEDBACK, DURATION0 fields [11:8] and [7:4] = 3;
    NONCPU_OPTION: SC_COUNTER, clear SC_FEEDBACK, DURATION0 likewise; UP_SCHEDULER bits 0-1;
    XXTI/TCXO_DURATION3 0x658, EXT_REGULATOR_DURATION3 0xFDE, EXT_REGULATOR_MIF_DURATION3 0xCB1,
    G3D_DURATION0[11:4] = 0; PS_HOLD_CONTROL |= bit 31 | 0x300; CPUCLx_PWR_CTRL3 bit 0 (L2QACTIVE).
  - Prepare: save CMU registers (PLL lock/con, muxes, divs, gates of TOP/MIF/PERI/FSYS; the domain
    CMUs G3D/DISPAUD/ISP/MFCMSCL only if the domain is on), force the top-mux gates on, USB and
    MIPI PHY user muxes to OSC, armidleclockdown off, write the SYS_PWR table (~130 registers,
    SLEEP column) and CENTRAL_SEQ_OPTION 0xFF0000, OPTION1 0x10000000, CENTRAL_SEQ_MIF_OPTION
    0x10, TOP_BUS_MIF_OPTION 0; WAKEUP_MASK and WAKEUP_MASK_MIF (0x628) bit 30; enable the central
    sequencer (CENTRAL_SEQ_CONFIGURATION 0x200 bit 16 = 0) and the MIF one (0x240 bit 16 = 0).
    SLEEP only: MIF BUS_PLL mux -> OSC, BUS_PLL off, FSYS_BUS divider 1, DECON INT VCLK mux 0.
  - Exit: PAD_RETENTION_{BOOTLDO 0x3248, AUD 0x3028, TOP 0x3108, UART 0x3128, MMC0 0x3148,
    MMC1 0x3168, MMC2 0x30c8, SPI 0x31c8}_OPTION = bit 28 (release); both central sequencers off
    (bit 16 = 1); TOP_BUS_MIF_OPTION 0; WAKEUP_MASK bit 30 cleared; armidleclockdown on; restore
    the CMU registers.
  - Early wakeup (firmware aborted the entry): BUS_PLL path back first, then as exit, without the
    pad release.
- Stock DT (work/stock-dt/stock-02.dts): ~100 `samsung,pin-con-pdn` / `pin-pud-pdn` settings
  (CON_PDN 0 = out 0, 1 = out 1, 2 = input, 3 = keep; e.g. gpf3-0 Wi-Fi power keep, gpd1-6 touch
  supply out 1, gpd3-6 WLAN_EN and gpd4-0 BT_EN keep). Our A3 DTS has 2. In SLEEP,
  GPIO_MODE_SYS_PWR_REG = 0 puts every non-alive pin into its power-down setting.
- S2MPU05 initial modes (stock DT): 0x3 = always on, 0x1 = off in sleep (PWREN), 0x2 = low power in
  sleep. BUCK1 0x1; BUCK2-5 0x2; LDO1/3/5 0x3; LDO4/6-10/25/31 0x1; eMMC LDO26/27 and LCD LDO29/30 0x3.

## 2. BL31 ("Cubic-OPM" ATF fork, May 2018), RE

Extract: mmcblk0boot0 backup, file offset 0x2010..0x10300 (BL31 code; 0x2000 is a 0x10-byte
header), mapped at 0x02024000 in Ghidra. The firmware actually runs it 0x10 higher (BL1 jumps to
0x02024010), so: address here = cpuidle.md blob offset - 0x10 + 0x02024000. Read only, local only.

- `affinst_suspend` 0x020290a4: afflvl == 3 -> iRAM 0x0206b008 = entry, 0x0206b00c = 0xbad,
  call system sleep 0x0202e8f0, return -3 if that returns. Other levels: cpuidle.md section 3.
  As for idle, the power_state type bit is never read: Linux may pass 0x03010000.
- System sleep 0x0202e8f0:
  1. secure OS (tbase) suspend hook 0x02028e70 and runtime-service PM hooks 0x02029964: nonzero
     aborts.
  2. PMU WAKEUP_MASK 0x610 |= bit 31.
  3. INFORM0 0x10480800 = entry (from 0x0206b008), INFORM1 0x10480804 = 0xbad (sleep marker),
     0x1373080c |= 3.
  4. SCR_EL3/SCTLR_EL3 changes, then copies secure iRAM 0x02045000 (0xa004 bytes) to secure DRAM
     0x7ffc0000. That is in the 0x7e400000-0x80000000 hole of our memory node: no conflict.
  5. If ISR_EL1 shows no pending IRQ/FIQ: checksum of iRAM 0x02021810..0x0203f480 through the iROM
     services at 0x02020110/0x02020114 (stored in INFORM5/6 0x814/0x818), SCR_EL3.IRQ, WFI forever.
     The PMU central sequencer then takes the system down.
  6. Abort path: restores WAKEUP_MASK bit 31, INFORM0/1 = 0, returns -> CPU_SUSPEND returns -3.
- BL31 writes no SYS_PWR, central sequencer, clock or pad retention register. All of that is
  the kernel's job, as in the vendor kernel.
- BL31 entry 0x02024000: RST_STAT & 0x10800000 -> reset bookkeeping 0x0202f790; warm boot with
  INFORM1 == 0xbad clears iRAM 0x0206b028; 0x0202fa64 (= cpuidle.md 0xba74) re-arms the CPUCL0
  watchdog if AUTOMATIC_DISABLE_WDT (0x408) bit 23 is set and INFORM1 == 0xbad.

## 3. BL1 wake path (boot0 offset 0, runs from 0x02021000)

- WAKEUP_STAT 0x10480600 bit 31 set = wake from sleep. Then checks PMU 0x10481198 [1:0],
  0x104811e0 [1:0] and 0x10482f08 bit 4, and calls 0x107c: checksum over iRAM 0x02021810 with the
  length from INFORM5 (0x814) through iROM, compared with INFORM6 (0x818). **Mismatch: spins
  forever (`b .`) before any watchdog is armed**: a hang there needs Power+VolDown.
- Then jumps to BL31 (0x02024010). No DRAM training: DRAM stays in self-refresh, retained by the
  MIF sequencer (PAD_RETENTION_LPDDR3 etc. in the SYS_PWR table).

## 4. Mainline (src/linux, a3y17lte) gaps and reuse

- Reuse: `exynos_sys_powerdown_conf()` + `struct exynos_pmu_conf` (drivers/soc/samsung/
  exynos-pmu.{c,h}, arm32 Exynos today); `psci_cpu_suspend_enter()`; our 7870 CPU PM notifier
  (clears CPUn_CONFIGURATION on CPU_PM_ENTER); samsung clk syscore save/restore of `clk_regs`;
  pinctrl `samsung,pin-con-pdn`/`pin-pud-pdn`; pinctrl pad retention (`samsung_retention_data`,
  `exynos_retention_disable` writes bit 28 like the vendor).
- Gaps: no `mem` suspend_ops (PSCI 0.1, no SYSTEM_SUSPEND); no 7870 SYS_PWR table; no 7870
  `retention_data`, so `exynos_pinctrl_set_eint_wakeup_mask()` only warns and never writes
  EINT_WAKEUP_MASK (0x60C, already the right register through exynos7_wkup_irq_chip); A3 DTS lacks
  the sleep pin table.

## 5. Risks

- Failed wake in BL1 = hang without watchdog. Real-sleep tests need the user at the phone.
- The modem (CP) and GNSS are never started under mainline. If the central sequencer waits on
  their state, entry stalls: check CP_STAT 0x38 / GNSS_STAT 0x48 first.
- STANDBYWFIL2 alone takes cluster 0 down at runtime (suspend.md): only together with the rest of
  the init, and recheck cluster idle afterwards.
- CMU restore into powered-off domains (G3D/DISPAUD/ISP/MFCMSCL) would abort; the vendor skips them.
  Mainline: every 7870 CMU has `clk_regs` (samsung clk syscore save/restore), and no 7870 DT uses
  power domains, so the domains stay on at runtime. Risk left: if the PMU does not bring the domains
  back on at wake, the syscore CMU restore aborts.
- GIC distributor save and our CPU_CONFIGURATION notifier run in `mem`: kernel/cpu_pm.c's syscore ops
  call cpu_pm_enter() + cpu_cluster_pm_enter() before `.enter`.
- Timer: the arch counter is fed by the MCT FRC. If the FRC loses its value in SLEEP, the counter
  restarts at 0: exynos4_frc_resume() only restarts it, and the arch timer is SUSPEND_NONSTOP (no
  `arm,no-tick-in-suspend`). Check counter continuity after the first wake.

## 6. Survey of the running system (2026-11-14, tools/probes/sleep-survey.sh)

Raw output: out/sleep-survey-20261114.txt (read only: peek + i2cget -f, Alpine, kernel a3y17lte).
- None of the vendor `syspwr_init` ran in S-Boot: L2_OPTION 0, UP_SCHEDULER 0, CPU/NONCPU
  DURATION0 0xffffffff, XXTI/TCXO/EXT_REGULATOR DURATION3 0xfff00000/0xfff03fff, feedback OPTIONs
  1 (SC counter; vendor wants SC feedback = 2), PS_HOLD_CONTROL 0x5300 (no HW trip bit 31),
  CPUCLx_PWR_CTRL3 0x802 (L2QACTIVE off), CENTRAL_SEQ_OPTION 0x01ff0000 / OPTION1 0, central
  sequencers disabled (0x200/0x240 = 0x10000). CPUn_OPTION 0x10010101 (SMPEN, STANDBYWFI, SC
  counter); CPU0 reads 0x01010109 (no SMPEN, STANDBYWFE + bit 3), presumably firmware-written.
- SYS_PWR table: 65 of 133 registers differ from the SLEEP column; all at reset values
  (0xffffffff = stay on, PAD_ISOLATION 0). The kernel must write the whole table.
- Wake masks: WAKEUP_MASK 0, WAKEUP_MASK2/3 0xffff00ff (= stock SLEEP), WAKEUP_MASK_MIF 0xd3,
  EINT_WAKEUP_MASK 0 (mainline never writes it: no 7870 retention data).
- CP_STAT 0x110, GNSS_STAT 0x110: nonzero, which the vendor treats as "may sleep".
- AUTOMATIC_DISABLE_WDT / MASK_WDT_RESET 0xff7fffff at runtime (bit 23 clear).
- **GPIO power-down: CON_PDN is 0 (= drive low in sleep) on almost every non-alive pin.** Only
  gpc1-0/1, gpd3-6, gpd4-0, gpm0-0/1 and gpr3 are set. Without the stock table, sleep would cut
  the Wi-Fi power switch (gpf3-0), the touch I/O supply (gpd1-6, stock = out 1) and pull gpd1-0
  low (stock name "pm-wrsti", PMIC warm reset). The DTS sleep pins are a hard prerequisite.
- S2MPU05: enable bits [7:6] = 3 (always on) on every BUCK and LDO, unlike the stock initial modes
  (BUCK1 and most LDOs 0x1 = off in sleep, BUCK2-5 0x2 = low power). Nothing switches off in sleep
  until the regulator step: safe first, savings later.

## 7. Entry hang: gated CMU_MIF top clocks (solved 2026-11-14)

Symptom: with the SYS_PWR table, options and sleep pins all equal to the vendor SLEEP setup,
`mem` never powered down; the debug watchdog (below) self-reset the phone ~10 s after the
PSCI call. PMUDBG after that reset (control: a plain runtime watchdog reset):

  | PMUDBG (PMU +)           | after the sleep hang | plain WDT reset |
  |--------------------------|----------------------|-----------------|
  | CENTRAL_SEQ_STATUS       | 0x002d0000           | 0               |
  | CENTRAL_SEQ_MIF_STATUS   | 0                    | 0               |
  | CPUCL0_CPU0              | 0x100b0000           | 0x105b0000      |
  | CPUCL0_L2 / CPUCL1_L2    | 0x10000000 / 0x10080000 | 0 / 0        |
  | CPUCL0_NONCPU / CL1      | 0x000e0000 / 0x005e0000 | 0x00080000   |

The CPU side reached its SLEEP targets; the central sequencer stopped at state 0x2d and never
started the MIF sequencer. (No decoding of the state numbers exists in the vendor tree.)

**Cause.** At suspend our drivers gate their top-level clocks. The vendor kernel force-enables
all of them before every non-AFTR sleep: topmux_enable() in pwrcal/S5E7870/S5E7870-syspwr.c sets
bit 21 of CMU_MIF 0x264..0x2cc (CLK_CON_MUX_CLKCMU_*) and bit 0 of 0x864..0x8cc
(CLK_ENABLE_CLKCMU_*, 27 registers). A snapshot of 344 CMU registers at sleep entry showed seven
enables at 0: FSYS_MMC0/1/2 (0x888/0x88c/0x890), FSYS_USB20DRD_REFCLK (0x89c),
PERI_UART_BTWIFIFM (0x8a4), PERI_UART_DEBUG (0x8a8), PERI_SPI_SENSORHUB (0x8c0). All mux gates
(bit 21) were already on, all 10 PLLs enabled and locked, no mux stuck. Forcing the enables on
(debug switch, nothing else changed) gave the first real sleep and RTC wake (status header).

**Fix** (own commit, upstream code): `mif_suspend_regs` in clk-exynos7870.c writes 1 to the 25
CLK_CON_GAT_MIF_CMU_* enables mainline defines in 0x864..0x8cc (0x894/0x898 UFSUNIPRO are not in
mainline and were on anyway), like `top_suspend_regs` in clk-exynos5433.c; the samsung clk
syscore resume restores the saved values. Whether all of them are needed or only the seven is
untested; the vendor and Exynos5433 force all. Verify on the phone: the debug line "DBG MIF
CLK_ENABLE ... off mask" must read 0 (without the fix: 0x00834e00). Verified 2026-11-14,
kernel #124 (commit + watchdog/abort debug only, no dbg_topgate): mask 00000000, "PM: woke up,
WAKEUP_STAT 80000001", "Timekeeping suspended for 16.800 seconds", wake IRQ 54 = gpa0-2 (S2MPU05,
RTC alarm), devices resumed in 3.6 s; then the same display failure as the first wake (DSI xfer
timeout -> vblank WARN -> flip_done -> DPM watchdog panic at +20 s). The screen was on at entry
(fb0 `blank` read 4, but the screen was lit: that attribute is not a reliable state).
Log: out/deep-sleep-run124-pstore-20261114.txt.

Shown NOT needed for entry (each tested with the gates forced on, or ruled out before):
BUS_PLL off / MIF mux / FSYS_BUS divider / DECON VCLK mux prep (vendor syspwr_prepare), media
power domains off before sleep (vendor genpd state), CP/GNSS central sequencers enabled, EINT
and wake mask changes, IDLE_IP_MASK, ALIVEIRAM_OPTION, USE_L2QACTIVE. BL31 afflvl 3 calls its
sleep routine (0x0202e8f0) directly, as for the vendor; it writes no clock or SYS_PWR register.

**Probes that found it** (debug-only, the full set is saved as
out/deep-sleep-debug-full-20261114.patch; the working tree now keeps only the first two):
- Watchdog armed right before the PSCI call (PMU 0x408/0x40c bit 23 cleared, WTCON 0x7739,
  WTCNT 0xabe0 = ~10 s): a stall self-resets (RST_STAT bit 23), a real power-down stops it.
- `exynos_pmu.dbg_abort=1`: a pending CNTV IRQ makes BL31 return -3 ("PM: sleep aborted"), so
  the whole entry path runs without sleeping (secure hooks, iRAM save, then GICC off, set/way
  clean, SMPEN = 0, WFI).
- PMUDBG (0x10480f00 central, 0xf04 MIF, 0xf08.. CPUs, 0xf30/34 L2, 0xf38/3c NONCPU) are the
  STATUS registers latched at a warm reset; they survive a watchdog reset, Power+VolDown clears
  them.
- CMU snapshot of the vendor cmusfr.h register list at entry, printed after resume (344 lines:
  floods the 16 KiB pstore, which is why it was removed).

## 8. Display after a DISPAUD power loss (solved 2026-11-14)

Symptom: after a real deep sleep the display never came back; with fbcon bound the resume hit
the 20 s DPM watchdog. Two symptoms: the first enable after the wake got no TE from the panel
and DSI commands timed out; a second enable (blank/unblank) sent exactly 7 frames and stalled.

**It is not about system sleep.** A local DISPAUD power-off/on alone reproduces both, in 30 s
and with no sleep: `echo 1 > /sys/module/exynos_pmu/parameters/dbg_abort` (debug build; BL31
refuses the sleep after the kernel has cycled DISPAUD, "PM: sleep aborted", timekeeping
suspended 0.002 s), display blanked, `s2idle.sh none mem quiet`, then unblank. Check that the
cycle happened ("PM: sleep aborted" in dmesg and DSIM CLKCTRL 0x14800010 = 0x000000ff): a
suspend refused earlier (alarmtimer -16) leaves DISPAUD untouched and the test means nothing.
Boot and runtime blank/unblank only work because the state S-Boot set up is still there.

### 8.1 Content firewall (the "7 frames, then stall")

- The 7 frames are background only: the plane is committed after the enable. The stall is the
  DECON's first memory read. Stalled state: DECON VIDCON1 (0x14830600) = 0x00006600 (ACTIVE,
  line 0), frame FIFO 0x1483052c = 0, frame counter 0x148307fc = 7 (it counts at 60 Hz when
  working), sysmmu STATUS 0x14860008 = 0xa for good (working: 0, rarely 0x8) and an empty sysmmu
  L1 TLB (0x14860750 select, 0x754/0x75c/0x764: all 0; working: 8 valid entries), bridge
  QCH_CTRL ASYNCM_LH_DISPAUD_MIF_D_RT (CMU_MIF 0x2020) busy bit 10 set for good. So the sysmmu's
  first page table walk never completes.
- Everything readable is equal to the working state: DECON 0-0x2000, DSIM, the whole sysmmu
  page, all of CMU_DISPAUD, DREX 0x10400000-0x400, SYSREG_MIF. Only SYSREG_DISPAUD 0x148f0600
  (DISPAUD_QOS_CON) differs, 0x00444444 after the power loss against S-Boot's 0x00444aaa (DECON
  AxQoS 0xA, vendor BTS default). Restoring it by hand changes nothing: QoS is not the cause.
- Cause: the display **content firewall** (CFW, clock gate CMU_DISPAUD 0x818
  "SECURE_CFW_DISP"), a secure block between DISPAUD and the memory bus. It loses its state with
  the domain and blocks every read until the EL3 firmware programs it again. The vendor DECON
  driver does that after every power-on, right after its runtime resume (decon_core.c, two
  call sites): `exynos_smc(MC_FC_SET_CFW_PROT 0x82002040, MC_FC_DRM_SET_CFW_PROT 0x10000000,
  DECON_CFW_OFFSET 3, 0)`, expected return 2 (SMC_TZPC_OK). MSCL and JPEG do the same with
  their own IDs (include/linux/smc.h PROT_*, "CFW permission").
- Proof: tools/probes/cfw-disp (one-shot module, makes that SMC) on the stalled display:
  returned 2, the stuck read completed at once, frame counter running, sysmmu STATUS 0. Issued
  before the enable (display blanked, the DISPAUD bus gates stay on), the second enable is clean.
- Fix: exynos7_drm_decon makes the call in atomic_enable after the runtime resume (7870 only).
  For upstream this is a vendor firmware call inside a DRM driver: expect the question whether it
  belongs in a power domain or firmware driver instead.

### 8.2 DSIM start order (no TE on the first enable)

- Trace (debug prints of the DSIM registers at each init step): after a power loss the DSIM
  starts from reset values (CLKCTRL 0xff, PLL/PHY registers 0, the PHY clock user muxes
  0x148d0224/0x228 mid-switch: 0x04201000, status 0). At boot and at runtime the PLL, PHY and
  CLKCTRL values of the previous session survive SWRST and the runtime suspend.
- Replaying the bring-up by hand (/root/dsim-steps.sh on the phone: one poke per named step,
  display blanked; the DSIM is readable while blanked):
  - mainline order (SWRST, PLL, CLKCTRL with esc/byte clocks and the HS request, PHY, CONFIG,
    lanes): INTSRC bit 0 at the lane enable. Bit 0 is ErrContentLP1 (LP1 contention) in the
    older Samsung DSIM headers. DPHY_STATUS 0x40f at once (clock lane HS-ready).
  - without SWRST: same. With the lanes enabled before the esc/byte clocks (vendor order): no
    contention flag, with or without SWRST. The HS request then takes the clock lane to
    HS-ready (0x40f) at once, also in the vendor order: that is normal.
- Kernel tests, first enable after a verified power cycle (tools/probes/ds-reduce.sh; #146/#147/
  #151 had a switch per sub-change, out/dsim-switch-build-151.patch):
  - PHYCTRL + PHY timing before the PLL enable (#143): no change. Not needed.
  - lanes before the esc/byte clocks (#144): contention flag gone, still no TE. Not needed for
    the display (PASS with the flag set); left out of the series.
  - panel commands in LP mode (#144, dbg_lpm): still no TE, so not the HS data path.
  - HS clock request at the first transfer instead of in the clock setup: **needed**.
  - command mode timings (porches, TE timers) at the end of the DSIM init instead of at the
    output enable: **needed**; standby (SFR_CTRL bit 4) at the first transfer instead of at the
    output enable: **needed**. Standby without the timings gives ABNORMAL_CMD_ST (INTSRC
    0x01400000), the timings without standby the plain failure.
  - trace prints on or off, a 1 ms hold before the HS request: no influence (not a timing race).
- What it comes down to: upstream runs samsung_dsim_init() in atomic_pre_enable on the 7870
  (samsung_dsim_hw_is_exynos() only covers 3250..5433), that is before the panel is powered. With
  the HS request in the clock setup the clock lane is in HS before the panel reset, and from a
  reset DSIM the panel then never syncs. A first clean version that did everything at the end of
  the init (still before the panel reset) failed for the same reason. Vendor kernel and S-Boot:
  PLL, lanes, stop state, esc/byte clocks, CONFIG and timings, panel reset, then HS request +
  standby, then the panel commands.
- Fix (7870 command mode only): no HS request in the clock setup, timings at the
  end of the init, HS request + standby at the first transfer (or at the output enable if the
  sink sent nothing). Verified on #152: boot, first enable after a power cycle (2x), runtime
  blank/unblank, real deep sleep with the display on and fbcon bound.
- M_PLL_CTRL1 is not needed: #153 with pll_ctrl1 = 0 passes twice with 0x98 = 0 after the power
  cycle. The commit was dropped from the series.
- Earlier "not sufficient" verdicts (deferred HS request, PHY before PLL, ...) were judged on a
  display that the firewall kept dark anyway and with the other defect still in: void.

### 8.3 Tools and traps

- /root/disp-all.sh (DECON 0-0x2000, DSIM, sysmmu, DISPAUD sysreg, all of CMU_DISPAUD),
  /root/mem-path.sh (DREX, SYSREG_MIF, SYSREG_DISPAUD), /root/dsim-steps.sh; dumps in
  out/ds-*-141-*.txt, working diff of #145 in out/deep-sleep-display-work-145.patch.
- Health check without looking at the screen: frame counter 0x148307fc rising by ~60/s, sysmmu
  STATUS 0x14860008 = 0, DSIM INTSRC 0x14800034 = 0x01080000, TE pin gpe0-2 high in 2-6 of 300
  samples of 0x139b01a4.
- A blank from the failed state takes ~30 s (command and vblank timeouts): bound the ssh call.
- Test with fbcon unbound first (no DPM watchdog panic): `echo 0 >
  /sys/class/vtconsole/vtcon1/bind`, `echo 1 > /sys/class/graphics/fb0/blank`.

### 8.4 Clean series (no debug code): 11 of 12 cycles, one unexplained failure

- #154 (and the same tree reordered as #155/#156): boot display fine; real `mem` with the display
  on + fbcon bound: 2 + 8 cycles in a row clean (16.7-17.1 s asleep, wake IRQ 54 = RTC alarm,
  60 fps, INTSRC 0x01080000, no errors), plus single cycles for the touch key tests.
- One cycle (the third on #154) never came back to userspace. The user did not touch the phone.
  The phone was later found freshly booted; the boot in between had panicked at 0.69 s:
  "exynos-sysmmu 14860000.iommu: 14830000.display-controller: [READ] PTW FAULT occurred at
  0x67092b00", "Unrecoverable System MMU Fault!"
  (out/deep-sleep-run154-cycle2-pstore-20261114.txt), then a watchdog reset and a good boot.
  That boot panic overwrote the log of the failed cycle. Treated as a fluke; if it repeats,
  tools/probes/mem-cycles stops at the failure and saves the pstore console.
- Not the watchdog racing the resume: BL31's wake code (0x0202fa64) stops the watchdog, clears
  PMU 0x408/0x40c bit 23 and re-arms it with WTDAT/WTCNT 0xff60, prescaler 0xee, divider 128,
  reset on: about 30 s at the ~67 MHz clock. The kernel's watchdog resume comes well inside that.
  It also means a kernel that hangs in resume is reset after ~30 s.
- The boot panic is its own, unexplained problem: the sysmmu translated while the DECON was
  still scanning out S-Boot's framebuffer (simple-framebuffer at 0x67000000). Normal boots have
  the same probe order and no fault. Seen once.
- An entry that stalls, or a wake that dies in BL1, is not covered by any watchdog on the clean
  series (the debug builds armed one before the PSCI call): that needs Power+VolDown.

## 9. Root causes and takeaways (2026-11-14)

Three independent defects stood between "the SoC powers down and wakes" and "the phone resumes".
None of them is a suspend/resume bug in the usual sense: each one is state that the bootloader
(or the previous session) had set up and that mainline never sets up itself.

| Symptom | Root cause | Fix |
|---|---|---|
| `mem` never powers down, sequencer stalls at 0x2d | drivers gate CMU_MIF top clocks at suspend; the PMU sequence needs all of them running (and the DSI PHY muxes on OSC) | clk suspend_regs, as the vendor topmux_enable() |
| after wake: 7 frames, then no frame completes | display content firewall lost with DISPAUD; every DECON/sysmmu memory read blocks until EL3 programs it again | firmware call at DECON enable, as the vendor |
| after wake: panel sends no TE, DSI commands time out | from a reset DSIM, mainline starts the HS clock before the panel is out of reset and programs timings/standby after the panel commands | HS request + standby at the first transfer, timings at init |
| "tm2-touchkey failed to read i2c data: -6" at resume | resume enables the IRQ before the supplies are back | power up, then enable the IRQ (section 10) |

What generalises to the next block that loses power (ISP, MFC/MSCL, G3D, audio in DISPAUD):

1. **A power domain that goes off is a cold boot for that block, without the bootloader.** Boot
   and runtime PM prove nothing about it: registers survive a software reset and a clock stop.
   Test bring-up from true reset on its own: power cycle the domain at runtime (PMU
   xxx_CONFIGURATION 0 / 0xf, vendor blkpwr sequence) instead of going through system sleep. The
   failures here reproduced in 30 s that way, against 5 min per kernel + sleep cycle.
2. **Diff against the vendor at the power-on boundary, not only register by register.** Reading
   every register showed "all equal" for both display defects: one was secure state (not
   readable), the other was order (not a value). The vendor's runtime-resume and enable paths
   list what the hardware needs after power-on: here an SMC, a QoS write (not needed), mux
   handling, and a strict bring-up order. Grep the vendor driver for exynos_smc() first:
   MSCL, JPEG, MFC and G3D have the same firewall call with their own IDs (include/linux/smc.h
   PROT_*), so each of them will need it after their domain was off.
3. **Find the stuck stage before trying fixes.** Counters and status bits located it: the DECON
   frame counter (0x7fc), VSTATUS, the frame FIFO level, the sysmmu STATUS and TLB readback, the
   bridge QCH busy bit. "Stuck at the first memory read with an empty TLB" pointed at the memory
   path, and the firewall was the only thing in it that could not be read back.
4. **With more than one defect, an end-to-end verdict is worthless.** Every earlier "not
   sufficient" was judged on a display that another defect kept dark. Judge each change on the
   earliest signal of its own stage (a status bit, a counter), and re-test old negatives once a
   blocker is gone.
5. **Replay a sequence by hand before rebuilding.** With the driver idle (display blanked) the
   DSIM bring-up could be poked step by step from the shell (dsim-steps.sh) and variants
   compared in seconds. For reductions, one build with a switch per sub-change (module
   parameters) replaced a build per variant.
6. **Check that the precondition happened.** Suspends were refused twice (alarmtimer -16), so
   the domain was never cycled and a "pass" meant nothing. The test now checks for "PM: sleep
   aborted" / "woke up" and for a reset-value register (DSIM CLKCTRL 0xff) and reports INVALID
   otherwise (ds-reduce.sh).
7. **Mind where upstream puts a step for this SoC.** The 7870 is not in
   samsung_dsim_hw_is_exynos(), so the DSIM init runs in pre_enable, before the panel is
   powered. A first clean rewrite put the fix where it looked natural and failed for exactly
   the original reason; the switch build passing with its prints off showed the difference was
   in the code, not in timing.
8. **A reset after a crash can cost the evidence.** The one failed cycle was followed by a boot
   that panicked and overwrote pstore. For long runs, stop at the first failure and save the log
   (mem-cycles); a second reset in a row loses it.

## 10. Touch keys at resume (fixed 2026-11-14)

- Symptom: "tm2-touchkey 0-0020: failed to read i2c data: -6" twice, 30 and 42 ms after every
  wake from `mem` (never in s2idle).
- Cause (upstream code, since the driver was added): tm2_touchkey_resume() calls enable_irq()
  first and then switches the supplies on and waits 150 ms. After `mem` an interrupt is pending
  when the IRQ is enabled, and the controller raises another while it starts; both run the
  handler against a controller that does not answer yet. Probe has the right order.
- Fix: "Input: tm2-touchkey - enable the interrupt after powering up on resume" (Fixes:
  72d1f2346ded). Verified: real `mem` cycle (17.6 s), no read error, the controller answers
  (register 0x07 reads 0). Not verified yet: a key press after resume (needs a finger), and
  whether the key backlight state matches /sys/class/leds/tm2-touchkey/brightness afterwards
  (the controller powers up with its own default; the driver does not write the state back).
- The module is loaded from the rootfs: after a driver change run tools/a3-sync-modules.
