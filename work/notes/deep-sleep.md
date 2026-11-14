# Deep sleep (suspend-to-RAM, `mem`): how it works on the 7870

Status 2026-11-14: researched (vendor kernel + BL1/BL31 RE), not implemented. Plan order:
survey (read only) -> pinctrl pad retention -> DTS sleep pins -> PMU sleep table -> suspend ops ->
staged tests (pm_test platform/core, then real mem) -> clocks/regulators/measurement.

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
