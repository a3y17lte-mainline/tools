# Deep sleep (suspend-to-RAM, `mem`): how it works on the 7870

Status 2026-11-14 (evening): **entry, RTC wake and display resume work** (kernel #145, debug
build). Entry hang = gated CMU_MIF top clocks (section 7). Display after sleep = two independent
defects that any DISPAUD power loss exposes (section 8): the display content firewall has to be
set up again through the EL3 firmware, and the DSIM has to start the HS clock only after the
panel reset. Verified by registers on the phone (60 Hz frame counter, sysmmu idle, DSIM INTSRC
0x01080000, TE present), with the display blanked and with it on + fbcon bound (no panic), with
and without the debug DISPAUD power cycle. Open: the user's visual confirmation, reducing the
DSIM change to the minimal set and cleaning the debug code out, tm2-touchkey reads (-6) twice
during resume, wake sources, sleep modes of the S2MPU05 rails, drain numbers.
Kernel commits (a3y17lte, not pushed): f56de1bae4a3 pinctrl pad retention, c8426396033e A3
power-down pin states (verified: CON_PDN/PUD_PDN = stock), 53f0f832d75a PMU system sleep + fixup
c68dd03b80bc (CPU0 local PWR_CFG kept on in sleep, as the vendor), 1e2e1051e92b clk: PHY user
muxes to OSC for suspend, 7b2d45d8a4ae clk: CMU_MIF top clock enables on for suspend (verified
run 124), 0c1e0e72b5eb imagis power cycle at probe, 16dabf2568fe DSIM M_PLL_CTRL1. The fixup and
1e2e1051e92b's FSYS/ISP parts are not verified individually. With the suspend ops in place
`mem_sleep` defaults to `deep`.

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
- Kernel tests, first enable after a verified power cycle:
  - PHYCTRL + PHY timing before the PLL enable (#143): no change.
  - lanes before the esc/byte clocks (#144): contention flag gone, still no TE.
  - panel commands in LP mode (#144, dbg_lpm): still no TE, so not the HS data path.
  - plus the HS clock request (and standby) only at the first transfer, after the panel has
    been powered and reset with the lanes in stop state (#145): **works**.
- So mainline requests the HS clock before the panel is out of reset; from a reset DSIM the
  clock lane then really is in HS when the panel starts, and the panel never syncs (no TE). The
  vendor kernel and S-Boot: PLL, lanes, wait for stop state, esc/byte clocks, CONFIG, panel
  reset, then HS request + standby.
- Not known yet: which of the #145 changes are needed beyond the late HS request (lanes before
  clocks, DSIM init before the panel power, timings at init, clearing a stale HS request).
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
