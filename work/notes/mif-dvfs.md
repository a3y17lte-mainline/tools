# Bus DVFS: MIF (DRAM controller), INT, DISP (research 2026-11-14)

Status: Stage 0 (research, read-only survey) done. No driver code yet.
Plan: Stage 1 INT + DISP (exynos-bus, passive from cpufreq, BUCK2 voting) -> Stage 2 MIF
(exynos7870-dmc devfreq driver) -> Stage 3 drop the 750 mV BUCK2 floor.

Sources: vendor `drivers/soc/samsung/pwrcal/S5E7870/{S5E7870-dfs.c,S5E7870-drampara.c,S5E7870-asv.c}`,
`drivers/devfreq/exynos/exynos7870_bus_{mif,int,disp}.c`, stock DT `devfreq_*@170000x0`, the ECT
(`tools/ect-parse out/ect-a3y17lte.bin mif int disp minlock timing`, saved in
`out/ect-a3y17lte-mif.txt`), and a peek survey of this unit (`out/mif-survey-20261114.txt`).

## 1. Domains and levels (ECT DVFS block)

All mux/div members live in CMU_MIF (0x10460000), which is the 7870's CMU_TOP.

| Domain | Levels (MHz) | Members | Boot | ECT resume idx |
|---|---|---|---|---|
| MIF | 902 836 728 676 546 451 275 | MEM_PLL (1804.4 1670.5 1456 1352 1092 902.2 549.25), MUX MIF_BUSD/CCI, DIV PHY_CLKM/BUSD/APB/CCI/BUSP/HSI2C | 902 | 5 (451) |
| INT | 400 334 200 134 | MUX+DIV CLKCMU_MFCMSCL_MSCL, _MFC, FSYS_BUS, PERI_BUS (PERI div 11 at every level) | 400 | 3 (134) |
| DISP | 200 134 | MUX+DIV CLKCMU_DISPAUD_BUS (div 3 / 5) | 200 | 1 (134) |
| CAM | 533 334 167 | ISP; vendor `use_reg_dummy` (no real vote); no driver here | - | - |

The stock DT lists MIF 825 MHz where the ECT says 836 MHz; the ECT is what the CAL actually uses.

All of MIF, INT, DISP, CAM and G3D are on BUCK2.

## 2. ASV (this unit)

Fuse words via `tools/probes/asv-fuse` (2026-11-14): 0x00090009 0x00090007 0x00070009 0x00091e85.
Layout (vendor `struct asv_tbl_info`, 16 bits per domain: group[3:0], modified group[7:4] signed,
ssa10[9:8], ssa11[11:10], ssa0[15:12]): word0 cpucl0/cpucl1, word1 g3d/mif, word2 int/disp, word3
table version[6:0], fused[7], asb_pgm[15:8], cp group.

This unit: CPU 9/9, G3D 7, **MIF 9, INT 9, DISP 7**, table version 5, all SSA 0.

Voltages (ECT ASV table 5):
- MIF g9: 902 687.5 / 836 675 / 728 631.25 / 676 618.75 / 546-275 600 mV
- INT g9: 400 712.5 / 334 662.5 / 200 612.5 / 134 600 mV
- DISP g7: 200 656.25 / 134 637.5 mV

So BUCK2 needs at most 712.5 mV at the current all-max state, and 600 mV with everything at its
lowest level. Today it sits at the 750 mV DT floor.

## 3. MINLOCK (CPU frequency -> MIF minimum), identical for both clusters

1586/1482 -> 902, 1352/1248 -> 836, 1144/1014 -> 728, 902 -> 676, 839/757 -> 546, 676/546 -> 451,
449/343 -> none (275). Stock MIF follows only QoS (polling 0, PPMU code `#if 0`), with this table as
the main input; display BTS, audio, UART, USB add their own minimums.

## 4. DRAM timing (ECT TIMING block)

Eight tables keyed by memory type = PMU DREX_CALIBRATION2 (0x104809a8) & 0xffff. **This unit: 0xff1f**
(raw 0x000fff1f). 33 parameters per level; see `out/ect-a3y17lte-mif.txt`.

Key fact: **the DRAM timing registers are the same at every MIF level** (ROW0/1, DATA_ACLK/MCLK,
POWER0/1, ETC1, PAUSE_MRS0-4, PHY DVFS_CON1-3). The tables only differ by memory type in ROW1/POWER0
(0x1b vs 0x1f, density/tRFC). Per level only these change:

| Level | 902 | 836 | 728 | 676 | 546 | 451 | 275 |
|---|---|---|---|---|---|---|---|
| tick_granularity | 9 | 8 | 7 | 7 | 6 | 4 | 3 |
| phy_dvfs_con0_set1 | 0x8a000000 | 0x8a000000 | 0x02000000 | 0x02000000 | 0x8a000000 | 0x8a000000 | 0x8a000000 |
| phy_dvfs_con0_set0 | 0x48000000 | 0x49000000 | 0x01000000 | 0x01000000 | 0x49000000 | 0x49000000 | 0x01000000 |

(set0/set1 masks: con0 0x50ffffff / 0xa0000000.) So a driver can keep the timings S-Boot programmed
into set 0 and copy them into set 1. No JEDEC timing computation is needed, and the DT needs only the
per-level tick/PHY values.

## 5. Live state on this unit (peek, 2026-11-14, Alpine, rc2)

- DREX 0x10400000: MEMORY_CONTROL0 0x10b00157, POWER_DOWN_CONFIG 0x03ff000f (dsref 0x3ff = >800 MHz),
  CG_CONTROL 0x0707071f (bit 24 PHY CG = 0?), TIMING_SET_SW 0 (pause-triggered set switch),
  PAUSE_MRS0-4 all 0 (the CAL writes them on the first DFS).
- Timing set 0 = ECT 0xff1f level 0 exactly, **except ETC1: live 0x571b25b3, ECT 0x575b25b3 (bit 22)**.
  Keep the live value (copy from set 0), don't write the ECT ETC1.
- Timing set 1 holds S-Boot leftovers that match no ECT level (ROW0 0x00050915, ROW1 0x2b63090a,
  POWER0 0x06767237, ETC1 0x57060503, DQS_OSC 0x6576): probably a training/boot frequency. Never
  switch to set 1 without rewriting it first.
- DREX_PF tick S0/S1 0x10 (the CAL writes the same tick value there).
- PHY 0x10440000: CAL_CON0 0x04600000, DVFS_CON0 0x48000000 (= level 0 set0), CON2 0x000e0021,
  CON3 0x90700000, CON1 0x00000000 (offset 0xe0), CON4/5 0x00ffffff/0x07070707.
- CMU_MIF: MEM_PLL CON0 0xa15b0500 (P5 M347 S0 = 1804.4 MHz), BUS_PLL CON0 0xa0f60400 (P4 M246 S0
  = 1599 MHz), PHY_CLK2X mux 0 (MEM_PLL), BUSD/CCI mux 2, divs PHY_CLKM 1, BUSD 1, APB 3, CCI 2,
  BUSP 1, HSI2C 9: all = MIF level 902. CMU+0x1004 (DMC timing set for pause) 0, PAUSE (0x1008) 1,
  status bits 17:16 = 0.
- INT dividers/muxes = level 400; DISPAUD_BUS div 3 = DISP 200.

## 6. Vendor MIF DFS sequence (pwrcal)

Two steps per change, always through the switching clock (PHY_CLK2X mux = 1):
1. `dfsmif_transition_switch(from -> switch)`: DREX CG_CONTROL bit 24 = 0 (PHY clock gating off),
   CMU PAUSE enable, dividers (raise first), muxes, then `pwrcal_dmc_set_dvfs(switch, set)`. The set
   alternates 0/1 on every call. Then the PHY_CLK2X mux goes to the switch clock and the code waits
   for PAUSE status (0x1008 bits 17:16) to read 0. Dividers are lowered afterwards.
2. `dfsmif_transition(switch -> to)`: MEM_PLL relock to the target (the PLL is not in use while on the
   switch clock), dividers/muxes, `pwrcal_dmc_set_dvfs(to, other set)`, PHY_CLK2X mux back, wait for
   PAUSE, dividers. Then CG_CONTROL bit 24 = 1 and the DSREF cycle in POWER_DOWN_CONFIG[31:16]: 0x3ff
   above 800 MHz, 0x1ff above 400 MHz, 0x90 otherwise. Bit 4 of MEMORY_CONTROL0 is cleared around
   that write.

`pwrcal_dmc_set_dvfs(rate, set)`:
- Set 0: PHY DVFS_CON0[25:24] = 0 (normal) at level 0, else 1 (DVFS0). TIMING_SET_SW = 0 and
  CMU_MIF+0x1004 = 0. Then it writes the set-0 DREX registers and PF tick, ORs the PHY CON0-3 set0
  values (masked), and writes PAUSE_MRS0-4.
- Set 1: PHY DVFS_CON0[25:24] = 2 (DVFS1), CMU_MIF+0x1004 = 1, and the set-1 registers.
- `pwrcal_dmc_set_pre_dvfs`/`post_dvfs` (VTMON) are empty on the 7870.

The vendor devfreq raises the voltage before the change and lowers it after. During the switch it
uses the voltage of the level above 667 MHz (`sw_volt_table`), and it posts the new MIF frequency
to the modem mailbox (no modem here).

Open questions before Stage 2:
- Switching clock (resolved 2026-11-14): PHY_CLK2X parent 1 = MUX_CLK_MIF_PHY_SWITCH (0x214, sel 0 =
  MEDIA_PLL, 1 = MEDIA_PLL/2). MEDIA_PLL CON0 0xa0cd0400 = P4 M205 S0 = 1332.5 MHz, i.e. the
  vendor's "1334 MHz" switch -> 666 MHz MIF. MEDIA_PLL is fixed (shared with other CMU outputs), so
  the switch path needs no PLL change. (0x10460180 is an unmapped hole: peek SIGBUS, harmless.)
- How the CMU PAUSE handshake interacts with the PMU sleep sequence (`mem`). The suspend path should
  go back to the boot level and timing set 0 first.

## 7. Peripheral QoS in the vendor kernel (for later interconnect work)

BTS (display/media) -> MIF + INT, lpass (audio) -> MIF + INT + CPU, samsung UART (BT) -> MIF + CPU,
dwc3 (USB) -> INT (`int_min_lock`), GPU -> MIF via gpu_pmqos.
