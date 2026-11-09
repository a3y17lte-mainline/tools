# S6E3AA2 (A3 2017 panel): datasheet notes

The panel driver is written from this file, not from the vendor code. Sources are cited as:
- V = vendor-kernel/drivers/video/fbdev/exynos/decon_7870/panels/s6e3aa2_a3y17_lcd_ctrl.c
- P = …/s6e3aa2_a3y17_param.h
- A = …/dynamic_aid_s6e3aa2_a3y17.h
- DT = work/stock-dt/stock-02.dts (rev02, this unit)
- HW = read from the phone (2026-11-09)

## Identity
- DDI Samsung S6E3AA2, panel module AMS474MM01 (V: DPUI_MODEL_NAME, vendor "SDC").
- A sibling module exists (s6e3aa2_ams474kf09_*) with its own tables: the ID read tells them apart.
- ID: DCS read 0x04, 3 bytes (P: LDI_REG_ID/LDI_LEN_ID). An all-zero ID means no panel (V: read_id).

## Link (DT s6e3aa2_hd node, HW DSIM dump)
- MIPI DSI **command mode** (DT mode = 2 = DECON_MIPI_COMMAND_MODE).
  - HW DSIM_CONFIG = 0xa440747f: VIDEO bit 18 clear, non-continuous clock lane (bit 31), burst bit,
    RGB24, 4 data lanes.
- 720×1280 at 60 Hz (HW DSIM_RESOL 0x050002d0). Physical size 59×105 mm (DT size).
- Porches (DT `timing,v-porch = <10 3 1>` = **vbp 10, vfp 3, vsa 1**; h 1/1/1). Confirmed by U-Boot's
  DECON VIDTCON0 = 0x00090002 (VBPD 9, VFPD 2). DECON and DSIM must both be programmed with them in
  command mode (see Host requirements).
- HS clock: PMS 3/226/2 from 26 MHz = 489.67 MHz/lane (DT timing,pms; HW PLLCTRL 0x00806712). Escape 16 MHz.
- Vendor DSIM in command mode sets only: non-continuous clock + mflush (vendor dsim_reg_7870.c).
- TE: gpe0-2, pin function 2 = DECON hardware TE input (DT decon_te_on).
  - The panel emits TE after DCS 0x35 0x00 (tear on, V-blank only) (P: SEQ_TE_ON).

## DECON command-mode registers (vendor regs-decon.h, HW dump after U-Boot)
- VIDOUTCON0 0x004: bit 24 DISP_IF_0_ON, bit 23 I80 interface.
  - HW = 0x01800000.
- VIDINTCON0 0x500: bit 17 INT_I80_EN. VIDINTCON1 0x504: bit 2 INT_I80 pending (write 1 to clear).
  - HW VIDINTCON0 = 0x00028803.
- TRIGCON 0x6B0:
  - bit 0 SWTRIGEN, bit 1 SWTRIGCMD;
  - bit 2 HWTRIG_INV, bit 3 HWTRIGEN;
  - bit 4 HWTRIGMASK_DISPIF0, bit 6 HWTRIG_AUTO_MASK, bit 13 TRIG_SAVE_DISABLE_SYNCMGR.
  - HW = 0x2058 (= 3 | 4 | 6 | 13: what our U-Boot sets).
- DECON_UPDATE 0x710 bit 0 STANDALONE_F.
- Frame buffer left by S-Boot: 0x67000000, 720×1280 ARGB8888 (HW 0x100).

## Power, reset (DT decon_board)
- Supplies: LCD_3P0 = S2MPU05 LDO29, 3.0 V; LCD_1P8 = LDO30, 1.8 V.
  - On: 3P0, then 1P8, then wait 10 ms.
  - Off: reset low, 1P8 off, 3P0 off.
- Reset gpd3-4, active low. Sequence:
  - wait 1 ms;
  - release, 5 ms;
  - assert, 5 ms;
  - release, 10 ms.
- PCD (panel crack detect) gpd3-5: not used.

## DCS / manufacturer commands
- Test keys: level 2 F0 5A 5A / A5 A5, level 3 FC 5A 5A / A5 A5 (P).
  - Same as panel-samsung-dsi.h lvl2/lvl3.
- Sleep out 0x11, sleep in 0x10, display on 0x29, display off 0x28 (standard DCS).
- PCD setting: CC 5C ("PCD set det low") (P).
- ERR_FG setting: ED 44 (P).
- Gamma: CA, 35 params (P: SEQ_GAMMA_CONDITION_SET):
  - V255 R/G/B as 9-bit values (msb byte, lsb byte);
  - then V203…V1 (8 levels × 3 colours, 1 byte each);
  - then VT packed as R<<4|G, then B.
  - Reference/default content: 01 00 01 00 01 00, 24 × 0x80, 00 00.
- AID/AOR: B1, 11 params (P: SEQ_AID_SETTING).
  - Params 1–9 are fixed: FF 20 1A 33 5E 8C B3 D9 FF.
  - AOR is an 11-bit count of dark lines per frame: p10[7:5] = AOR[10:8], p11 = AOR[7:0] (every vendor
    table entry has p10[4:0] = 0). 1293 lines (≈ one frame) at 2 nit, 12 at 420 nit.
- ELVSS: B5, 30 params (P: SEQ_ELVSS_SET).
  - p1 = 0xA0.
  - p2 = MPS_CON: 0x0C below 41 nit, 0x1C from 41 nit.
  - p3 = ELVSS dim offset 0x41..0x54 per luminance step (P: elvss_mpscon_offset_data).
  - p23 = temperature compensation.
  - p30 = TSET: temperature, |°C| with bit 7 = negative; 25 °C = 0x19.
- Gamma/AOR/ELVSS latch: F7 03 (P: SEQ_GAMMA_UPDATE). Send it after changing CA/B1/B5.
- ACL:
  - 55 00 = off, 55 02 = on (8 %).
  - OPR settings B4 (frame average / start point / percent).
  - Off at the vendor default for our purpose.
- Read-only registers:
  - MTP C8, 35 bytes: per-panel gamma offsets, same layout as CA; the VT nibbles are in bytes 33–34.
  - Coordinate/date A1 (4 + 7).
  - Chip ID D6 (5).
  - Manufacture info C9.
  - HBM data B3 (31).
  - ELVSS readback B5.
- Status reads (diagnostics only):
  - 0A power mode, expected 0x9E: bit 7 booster ok, bit 2 display on.
  - 0E signal mode, expected 0x80.
  - 05 DSI error count.
  - EE ESD error: bit 2 VLIN3, 3 ELVDD, 6 VLIN1.

## Sequences (V: s6e3aa2_init / s6e3aa2_exit / displayon)
- On:
  - sleep out, 20 ms;
  - (read ID);
  - keys on (F0, FC);
  - CC 5C, ED 44;
  - brightness (CA, B1, B5, F7, then ACL);
  - keys off;
  - 35 00 (TE on);
  - 120 ms;
  - then display on, after the first frame.
- Off: display off, 10 ms, sleep in, 150 ms.

## Brightness model (V: low_level_set_brightness, P/A tables; implemented 2026-11-09)
- The vendor maps UI 0–255 (365 with HBM) to 74 luminance steps, 2–420 nit, then HBM 465–600 nit.
- Per step it sends:
  - a gamma table CA, computed at runtime from the MTP ("dynamic AID": per-panel, not static);
  - an AOR value;
  - an ELVSS offset.
- The AOR is NOT monotonic over the range: it is constant 0x20C2 over UI ~104–128, while gamma alone
  dims there. So vendor AOR values only make sense together with the per-step gamma.
- Implemented as in the reference (the AOR-only "v1" idea was dropped; it was never the real problem):
  - 74 luminance steps (2–420 nit), each with a grey mapping (`m_gray`), colour offsets and ELVSS
    MPS/offset; 256 brightness levels map to (step, AOR) exactly like the vendor UI scale.
  - Per-step gamma is computed once from the panel's MTP (C8): tap voltages from MTP → voltage of the
    step's grey levels (linear between taps) → register value relative to MTP (+offset, clamp).
    VREG 6.0 V; tap formula v = ref − (ref − upper)·(setting + base)/span, base/span per tap
    (VT 0/860 via a 16-entry fraction table, V1/V7 0/256, V11–V203 64/320 referenced to VT, V255 129/860).
  - Verified byte-exact against a Python model on this unit's MTP (00 50 00 20 00 79 57 58 5d …).
  - ELVSS: read B5 (30 bytes) once, rewrite with p1 0xA0, MPS/offset per step, TSET 0x19 (25 °C).
  - At 420 nit the result is the default table (01 00 ×3, 0x80 …); S-Boot itself uses that table with
    AOR 741 for its logo (its init table is in sboot at 0xa3420, next to `s6e3aa2_read_id`).

## Host requirements in command mode (found 2026-11-09)
- DSIM (7870): CMD_TE_CTRL0 (0x7C) = stable_vfp (xres·3/100) | vsync_tout (vfp·xres·3/100) << 0;
  CMD_TE_CTRL1 (0x80) = te_protect_on (hs_MHz·158/8) << 16 | te_tout (hs_MHz·180/8). Left at 0 (mainline),
  every frame ends in INTSRC ABNORMAL_CMD_ST and the panel gets no data.
- DSIM porches (VPORCH VBP, HPORCH, SYNC VSA/HSA) must be set in command mode, matching DECON
  VIDTCON0–3. Neither set → panel dark; only DSIM set → ABNORMAL + garbage on screen.
- DECON: VIDCON1 VCLK = RUN_VDEN_DISABLE in command mode (vendor); TRIGCON hardware TE trigger.
- Not needed (tested): U-Boot's DSIM CONFIG bits (FLUSH_VS, Q_CHANNEL…), its PLL PMS (3/226/2 vs
  mainline 3/113/1) and PHY timings.
- Status registers that were useful: DCS 0x0A (0x9C = ok after display on), 0x0D (bit 3 = all pixels
  off, seen while frames were broken), 0x0E (0x80 = TE on; bit 0 = DSI error), 0x05 (DSI error count),
  DSIM INTSRC (0x34: bit 24 FRAME_DONE, 22 ABNORMAL, 19 HSYNC_TOUT — U-Boot's working state shows
  FRAME_DONE+HSYNC_TOUT). Panel ID on this unit: 46 00 02.
- A "white" screen at AOR 0 is the panel's own stale frame memory, not our data: always test with
  content that can't be confused with it (fill black and see if it changes).

## Never
- No OTP/MTP program commands: only the volatile registers above are written.
- No mDNIe, ALPM/HLPM (BB/53), LTPS EQ (FD), HBM, temperature compensation, factory sysfs.
