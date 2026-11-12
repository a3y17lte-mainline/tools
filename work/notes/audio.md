# Audio on Exynos 7870 / Galaxy A3 2017: research for clean drivers

Researched 2026-11-12. Sources: vendor `sound/soc/codecs/cod3026x.c` (+ `.h`),
`sound/soc/codecs/exynos-audmixer-v2.c` (+ `exynos-audmixer-regs.h`; the A3 builds
`CONFIG_SND_SOC_EXYNOS_AUDMIXER_VER=2`), `sound/soc/samsung/universal7870-cod3026-v2.c` (the
machine driver actually built), `sound/soc/samsung/lpass-exynos7870.c`, `sound/soc/samsung/i2s.c`,
`drivers/soc/samsung/pwrcal/S5E7870/` (clocks), stock DT, stock `/system/etc/mixer_paths.xml`.
Nothing was run on the phone. Everything here except section 9 (board) is the same on every
7870 phone with the Samsung reference audio (J5 2017 included).

## 1. The chain

```
 RAM --ADMA(pl330 @14890000)--> I2S0 "MI2S_AUD" @148a0000 (master, 48 kHz, 16/24-bit, 32/64 fs)
                                   | internal, inside DISPAUD
                                   v
                       S1402X audio mixer @14880000 ("AUDMIXER", AP0 input; also CP, BT inputs)
                                   | external pins gpz1 ("i2s-pmic-bus"): BCLK/LRCK/SDO/SDI + MCLK
                                   | mixer is master towards the codec: 64 fs, 24-bit, MCKO = codec MCLK
                                   v
                       COD3026X codec (I2C hsi2c0 @0x04, IRQ gpa0-4)
                         outputs: SPK (mono class-D, the A3 loudspeaker), EP (earpiece), HP (stereo, class-G)
                         inputs:  MIC1 (main), MIC2 (2nd/top), MIC3 (headset mic), LINEIN (unused)
                         jack + buttons measured through the SoC ADC (ch2 ear, ch4 ground)
```

Other mixer ports, not needed for local playback/capture: CP (voice call from the modem, AIF2),
BT (gpz0, I2S/PCM to the QCA9377: Bluetooth SCO audio, AIF3), FM (gpz2, no FM chip on the A3),
I2S1 "MI2S_AMP" @148b0000 (RX-only feedback from a smart amp; the A3 has none: the `spk-amp`
I2C bus i2c@13860000 is enabled in the stock DT but has no children). Samsung's "EAX"/"seiren"/
compressed offload paths: skip.

The mixer only accepts 48 kHz or 192 kHz on the AP input (`audmixer_hw_params`); there is no
44.1 kHz family anywhere. Expose 48000 only at first (PipeWire resamples); 192 kHz (HQ mode)
later if wanted.

## 2. Blocks, addresses, clocks (vendor clock ids decoded from the vendor `enum exynos7870_clks`)

| Block | Reg | IRQ (GIC SPI) | Vendor clocks -> mainline clock ids (verify each gate when writing the DT) |
|---|---|---|---|
| CMU_DISPAUD | 148d0000 | | already in mainline (`cmu_dispaud`) |
| AUD sysreg "LPASS" | 148f0000 (DISPAUD_CFG at +0x1000) | | `fout_aud_pll` = AUD_PLL, `dout_sclk_mi2s` = `CLK_DOUT_DISPAUD_MI2S`, `aclk_aud`, `dispaud_decon` (vendor keeps the DECON clock on for audio: bus dependency) |
| ADMA (pl330) | 14890000 | 73 | `apb_pclk` = vendor `dispaud_mi2s_amp` group (APB_AUD_AMP + MI2S_AMP codclk). Odd; try `CLK_GOUT_DISPAUD_APB_AUD_AMP` |
| I2S0 (MI2S_AUD) | 148a0000 | 69 | `iis` = `dispaud_mi2s_aud` group = `CLK_GOUT_DISPAUD_APB_AUD` + `CLK_GOUT_DISPAUD_MI2S_AUD_I2SCODCLKI`; `i2s_opclk0/1` = `d1_i2s` = `CLK_DOUT_DISPAUD_MI2S` |
| I2S1 (MI2S_AMP) | 148b0000 | 71 | `APB_AUD_AMP` + `MI2S_AMP_I2SCODCLKI` (not needed on the A3) |
| S1402X mixer | 14880000 (0x200) | none | `audmixer_dout` = `d1_mixer` = `CLK_DOUT_DISPAUD_MIXER`; `audmixer_aclk` = `dispaud_mixer` = `CLK_GOUT_DISPAUD_MIXER_AUD_SYSCLK` |
| PDMA0 | 13450000 | 228 | `CLK_GOUT_FSYS_PDMA0_ACLK_PDMA0` (8 ch, 32 req; general peripherals, not audio) |
| PDMA1 | 13460000 | 229 | **secure** (`#dma-secure-mode`): never describe or touch |
| ADC | 10550000 | 395 | `gate_adcif` = vendor `mif_adcif` -> `CLK_GOUT_MIF_WRAP_ADC_IF_PCLK_S0` (+ `_OSC_SYS`?) |

Power domain: DISPAUD (PMU 0x10484020, vendor `pd-dispaud`) is shared with the display, so it is
on whenever the display is. Mainline has no 7870 power-domain node yet; nothing to do for
bring-up, but suspend work must keep display and audio votes together.

DMA requests on ADMA: I2S0 tx = 0, tx-sec = 1, rx = 2; I2S1 rx = 3. `#dma-cells = <1>`,
4 channels, 16 requests, `arm,primecell-periphid = <0x341330>` (stock DT; check whether mainline
pl330 needs it or reads the ID registers itself). The vendor puts the pl330 microcode in audio
SRAM (`#dma-mcode-addr = <0x3002000>`) for low-power playback: a Samsung fork feature, ignore;
mainline keeps microcode in DRAM, which the ADMA can reach.

## 3. Clocks: AUD_PLL is wrong in mainline

- Samsung CAL (`S5E7870-cmu.c:54`): `CLK_PLL(14310, AUD_PLL, ...)` = **pll_1431x**, fractional.
  Mainline `clk-exynos7870.c` registers `fout_dispaud_aud_pll` as `pll_1417x` (integer).
  CON0 0x148d01c0: enable 31, lock 29, M [25:16] (mask 0x3ff, spec max 511), P [13:8], S [2:0];
  CON1 0x148d01c4: K [15:0] signed. Same layout as mainline `pll_36xx` (M mask 0x1ff suffices).
  Fix: `PLL(pll_36xx, ...)` with a rate table. Limits from `pwrcal-pll.c` gpll1431X_spec:
  P 1..63, M 16..511, S 0..5, K -32767..32767, Fref 6..30 MHz, Fvco 600..1200 MHz.
- Rates (computed for 26 MHz OSC):
  - 98.304 MHz: P=3 M=91 S=3 K=-16898 (98 304 003 Hz). This is what the vendor uses
    (`AUD_PLL_FREQ 98304000`).
  - 196.608 MHz: P=3 M=91 S=2 K=-16898. 90.3168 / 180.6336 MHz (44.1 k family): P=3 M=83 S=3/2
    K=24206, only if ever needed (the mixer cannot do 44.1 kHz anyway).
  First read what S-Boot left in CON0/CON1 (section 10): if it already runs 98.304 MHz, the
  fix is only needed for correct rate reporting.
- Vendor rates: MI2S = AUD_PLL (div to <= 110 MHz -> 98.304 MHz), I2S then divides with
  RFS 512 -> 24.576 MHz RCLK at 48 kHz, BFS 32 (16-bit) or 64 (24-bit). Mixer sysclk
  (`CLK_DOUT_DISPAUD_MIXER`) = 24.576 MHz at 48 kHz, 49.152 MHz at 192 kHz.
- `mout_dispaud_mi2s` has a second parent `CLKIO_DISPAUD_AUDIOCDCLK0` (external 100 MHz pin
  clock in CAL): not used.

## 4. LPASS = resets + PMU bits, not a driver

The vendor `lpass-exynos7870.c` is a big framework (sub-IP registry, QoS, idle-IP votes,
proc file, sched tweaks). What the hardware actually needs:

- **Software resets**, DISPAUD sysreg 0x148f1000 (`EXYNOS7870_DISPAUD_CFG`): bit 3 ADMA,
  bit 2 AMP (I2S1), bit 1 MI2S (I2S0), bit 0 MIXER. 1 = in reset, 0 = running (vendor
  `lpass_reset_toggle`: set, 100 us, clear, done after every power-up of the domain).
  Model it as a reset controller: a tiny node `reset-controller@148f1000` (4 bytes; it does not
  overlap mainline's `syscon_disp` at 0x148f100c, which the display owns) handled by
  `reset-simple` with a new compatible (active-high, set = assert). Consumers: `resets = <...>`
  in the pl330, I2S and mixer nodes. (pl330 already supports `resets`/`reset-names`.)
- **PMU bits** (in the PMU syscon, 0x10480000):
  - +0x0080 (vendor `reg-alive` of the mixer: `<0x10480080 0 0>`, active-low): bit 0 must be 0
    while the mixer is in use and is set to 1 when it is off. Same register's bit 1 is the
    BT/FM input mux (`lpass_set_fm_bt_mux`; 7880 name AUD_PATH_CFG). Read it first.
  - +0x1340 GPIO_MODE_AUD_SYS_PWR_REG bit 0 and +0x3028 PAD_RETENTION_AUD_OPTION bit 28:
    pad retention around DISPAUD power-down. Only needed with power-domain/suspend work.
  Simplest clean model: the mixer driver gets `samsung,pmu-syscon` (phandle) and toggles PMU+0x80
  bit 0 in its runtime PM callbacks; pad retention goes to the future power-domain code.
- Keep `dispaud_decon` (the DECON clock) enabled while audio runs, as the vendor does: in
  practice the display keeps it on; note it as a dependency rather than add a hack.

## 5. I2S0: mainline `sound/soc/samsung/i2s.c` + one compatible

The vendor 7870 node is `samsung,i2s-v5` + `samsung,i2s-str` + `supports-rstclr` +
`supports-secdai` (+ esa-dma, rx-sram, amixer: Samsung fork features, ignore). The `i2s-str`
MOD register layout (`I2S_STR_MOD_*` in vendor `i2s-regs.h`) is: BFS [2:0], RFS [5:3],
SDF [7:6], TXR [9:8], RCLKSRC 10, SLAVE 11, CDCLKCON 12, LRP 15, 3-bit BFS/RFS masks: that is
exactly mainline's **`i2sv5_i2s1_regs`** (used by `samsung,exynos7-i2s1`).

New `samsung,exynos7870-i2s` dai data: `.quirks = QUIRK_SEC_DAI | QUIRK_NEED_RSTCLR`
(no PRI_6CHAN: the stock node lacks `supports-6ch`; no TDM; no IDMA),
`.pcm_rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_192000` (mixer limit), regs
`i2sv5_i2s1_regs`. Binding: add to `samsung,i2s.yaml` (clocks "iis", "i2s_opclk0",
"i2s_opclk1"; `#clock-cells = <1>` provider exists in mainline i2s.c). Secondary DAI (tx-sec) is
optional; start with the primary only.

Machine-side settings the vendor uses for 48 kHz: CPU DAI master (`CBS_CFS` from the codec's
view), I2S format, `SAMSUNG_I2S_OPCLK` = `MOD_OPCLK_PCLK`, `SAMSUNG_I2S_RCLKSRC_1`, RFS 512,
BFS 32 (16-bit) / 64 (24-bit); mixer told the same BFS via `set_bclk_ratio`.

## 6. ADC: `samsung,exynos7870-adc` (needed for jack buttons and battery temperature)

Vendor `exynos_adc.c` "v3" (stock compatible `samsung,exynos-adc-v3`) differs from mainline
`samsung,exynos7-adc`:
- data register at **+0x08** (`ADC_V3_DAT`), mainline exynos7 reads +0x0c (`ADC_V1_DATX`, which
  is `ADC_V3_DAT_SUM` here): mainline would return the wrong register;
- init: soft reset (CON1 bit 2), then `ADC_V2_CON1_SOFT_NON_RESET` (bit 1), CON2 C_TIME(6),
  INT_EN = 1; 8 channels, 12-bit (mask 0xfff).
Small patch: new data struct with its own `adc_isr` (reads +0x08), `init_hw`, 8 channels; DT node
with `#io-channel-cells = <1>`, IRQ SPI 395, clock (the binding wants "adc").
A3 channel use (stock DT): ch0 AP thermistor (`sec-ap-thermistor`, table in stock DT
`adc_array`/`temp_array`, 23 points from 90 C to -20 C) -> `generic-adc-thermal` +
thermal zone; ch1 battery temperature (`adc-temp`) and ch3 `adc-in-bat` (battery node) ->
the fuel gauge/charger side (battery temp limits for charging); ch2 earjack (`adc-ear`) and
ch4 ground/water detect (`adc-gdet`) -> codec.
Hardware ADC: no driver touches the phone-side chips: zero safety concern.

## 7. S1402X audio mixer: new ASoC component driver

Register file: `vendor sound/soc/codecs/exynos-audmixer-regs.h`, 8-bit registers at a 4-byte
stride (MMIO regmap: `reg_bits 32, val_bits 32, reg_stride 4`). Key registers (index x 4):
0x00 SOFT_RSTB (SYS_RSTB, DATA_RSTB bits), 0x01/0x02 IN1 (AP) CTL1/CTL2 (master/slave, XFS
32/48/64 fs, LRCK polarity, I2S/LJ/RJ, data length), 0x04/0x05 IN2 (CP), 0x07/0x08/0x09 IN3
(BT/PCM), 0x0a HQ_CTL (MCKO_EN = codec MCLK out, BCK4 mode, HQ_EN for 192 kHz), 0x0b/0x0c
SLOT_L/R, 0x0e TSLOT, 0x0f DIG_EN (MIX_EN), 0x10.. mixer/volume/ALC, 0x18 INAMP_CTL,
0x19 OUTAP1, 0x1a OUTCP1.

Boot init the vendor does once (`audmixer_init_mixer`): IN1/IN2/IN3 = 32 fs, LRCK normal, I2S,
16-bit; IN3 slave, 32-BCK slots; SLOT_L = slot 1, SLOT_R = slot 2, TSLOT = 2 slots;
INAMP 64 fs 24-bit; OUTAP1/OUTCP1 32 fs 16-bit; HQ_CTL = MCKO_EN (A3: no BCK4 MCLK mode);
DIG_EN = MIX_EN; then pulse DATA_RSTB. Power-on: PMU+0x80 bit 0 clear, sysreg reset release,
then SYS_RSTB/DATA_RSTB.

Clean driver shape: platform driver, regmap MMIO with cache, runtime PM (clocks + PMU bit +
reset), DAIs AP0 (+ later CP0, BT), DAPM widgets for the mixer enables that the HAL toggles
("AudioMixer Mixer En", "CH1 Mixer En", "CH1 DOUT Select" = DMIX_OUT/AIF4IN, RMIX1/2,
SRC2/3) instead of raw enum controls, `set_bclk_ratio`/`hw_params` for XFS and data length,
and a **clock provider** (`#clock-cells = <0>`) for the codec MCLK so the codec can simply
`clk_prepare_enable()` it instead of calling into the mixer (vendor `audmixer_get_sync()`).
The codec probably needs that MCLK even for I2C register access (vendor enables it before every
codec register sync).

## 8. COD3026X codec: new ASoC codec driver

### Bus and supplies

- On **hsi2c0 (10510000), address 0x04**, sharing the bus with the S2MPU05 PMIC (0x66). The
  stock DT marks both with `ten-bit-address`: Samsung's i2c-core sets I2C_CLIENT_TEN, and their
  hsi2c "batcher" driver sets `HSI2C_10BIT_ADDR_MODE` for reads only. Mainline already talks to
  the PMIC with plain 7-bit 0x66 (regulators work), so try plain 7-bit 0x04 first. 0x04 is a
  reserved 7-bit address: the i2c core accepts it for a DT-instantiated client (only 0x00 is
  rejected), `i2cget` needs `-a`. If 7-bit reads NACK, test 10-bit (`I2C_M_TEN`; mainline
  i2c-exynos5 would need 10-bit support checked).
- `vdd-supply` = LDO25 (0.8-2.375 V range; our DTS has it always-on; read its voltage in
  regulator_summary). The vendor only enables it, never sets a voltage.
- IRQ: gpa0-4, level-low, wake-capable (jack/button/water events).
- MCLK: from the mixer (section 7).
- No chip-ID register. Registers 0x00-0xde, 8-bit (`regmap 8/8`). 0x01-0x05 IRQ pending
  (read probably clears: do not read them by hand), 0x06-0x0a IRQ masks, 0x0b-0x0d STATUS,
  0x10-0x18 power-down/auto-power/ref, 0x20-0x26 ADC vol/mix, 0x30-0x38 output vols/ctrl,
  0x40 DIGITAL_POWER, 0x41 FORMAT, 0x42 ADC1, 0x50-0x53 DAC/MQS, 0x54-0x5c DNC (dynamic
  noise/limiter), 0x5d-0x5f SPK limiter, 0x70-0x8b clocks/chopping/charge pump/trims/jack det/
  mic bias, 0xd0-0xde **OTP trim registers** (15 bytes: read and saved at first power-up, written
  back after every codec power cycle, never "programmed").

### Init (what the vendor does with no firmware file; the A3 DT has no `update-firmware`)

On every enable: pinctrl default, vdd on, MCLK on, `EN_PDB_JD_CLK` in 0x81, wait 15 ms, sync the
register cache, then on first probe save OTP 0xd0..0xde, otherwise restore them. Then once
(`cod3026x_post_fw_update_failure`): IRQ masks 0x06=0x80 0x07=0xc0 0x08=0xc0 (internal JD);
0x81=0x42, 1 ms, 0x81=0x43; 0x85=0xff; 0x86=0xff; 0x87=0x0f; 0x88=0xf2; 0x8b=0x0b;
0x84 JD pop 2000k + JD threshold (default `CTRV_JD_VTH_150K`); 0x42=0x0c (ADC HPF 100 Hz);
0x5a=0x1a (DNC start gain); 0x77=0x02 (CP freq); 0xdd=0x82 (SPK); 0x75=0x11, 0x78=0x01
(MIC3N trim); clear HP/EP/SPK chop bits in 0x76; mic bias voltages (0x18 MCB1, 0x82 MCB2 + LDO);
0x70 ADC chopping clock = 1/4. These are "magic defaults from Samsung's tuning": keep them as a
documented `reg_sequence` table, not open-coded writes.

A3 DT values: `mic-bias1-voltage = <1>`, `mic-bias2-voltage = <1>`,
`mic-bias-ldo-voltage = <3>` (codes, see `CTRV_MCB*` in cod3026x.h), `use-det-adc-mode`,
`use-det-gdet-adc-mode`, `mic-adc-range = <1347>`, `mic-det-delay = <550>` (ms).

### Audio format

Codec DAI is always slave, I2S, 24-bit, 64 fs (the mixer converts). `hw_params` only matters for
192 kHz (MQS mode in 0x53 + DNC off during the switch + digital reset via 0x40). Formats I2S and
left-justified, all four clock polarities.

### DAPM (from the vendor tables; the A3 HAL uses these control names)

Outputs: `SPKOUTLN` <- "SPK" switch ("SPK On", 0x76) <- SPKDRV <- "SPK Mixer" (DACL/DACR/ADCL/ADCR);
EP same pattern ("EP On"); HP stereo ("HP On", `hpdrv_ev` does the class-G charge pump and
offset sequence). Inputs IN1L/IN2L/IN3L/IN4L -> MIC1/MIC2/MIC3/LINEIN PGAs ("MICn On") ->
ADCL/ADCR mixers ("ADCL Mixer MIC1L Switch", ...). Controls used by the stock HAL: "Chargepump
Mode" (VDD vs CLASS-G-A), "DNC Max Gain", "DNC ZCD Enable/Timeout", "MonoMix Mode",
"Speaker Volume", DAC/ADC volumes.
A3 routing from `mixer_paths.xml`: speaker = SPK (the "SPK On" lines are commented out in the A3
file, so the speaker path is on by default/driven by DAPM), handset = EP, headset = HP;
main mic = MIC1, 2nd mic = MIC2 (dual-mic noise reduction = MIC1 L + MIC2 R), headset mic =
MIC3. Mic biases: MIC1/2 internal bias, MIC3 = headset bias (machine `mic-bias-mode = <1 1 2 0>`
for MIC1, MIC2, MIC3, LINEIN; no external bias GPIOs on the A3).

### Jack and buttons

Insertion: codec internal jack detect, IRQ. 3/4-pole and buttons: the vendor powers the mic
bias, waits `mic-det-delay`, then reads SoC ADC ch2 (`adc-ear`) several times and averages.
Button zones (stock `but-zones-list`, ADC raw ranges):
KEY_MEDIA (226) 0-210, KEY_VOICECOMMAND (582) 211-291, KEY_VOLUMEUP (115) 292-486,
KEY_VOLUMEDOWN (114) 487-1000. ADC ch4 (`adc-gdet`) is ground/water detection (Samsung's
"water in the jack" feature): optional, skip at first.
Clean implementation: `snd_soc_jack` with SND_JACK_HEADSET + 4 buttons, an IIO consumer
channel (`io-channels`), thresholds from DT (a new binding property or reuse an existing
headset-button pattern), threaded IRQ + one delayed work for debounce. No input device or
sysfs of its own (the vendor's `jack_joshua.c`/`SEC_HS_SYSFS` stays out).

### Do not copy

Global singletons, `audmixer_get_sync()` cross-driver calls, 5 single-thread workqueues,
wakelocks, water-detect polling, the regmap "firmware update" path, `cod3026x_dai_set_fmt`
(it overwrites `fmt` before switching on the inversion bits: a vendor bug), `cod3026x_sys_reset`
(passes `*_SHIFT` values as masks), dev_err used as debug output, CP-call special cases in
suspend.

## 9. Machine driver / card

The vendor card (`universal7870-cod3026-v2.c`) uses one DAI link with CPU = I2S0 and **two
codecs** = {mixer AP0, cod3026x-aif}, plus the mixer as an aux device. Mainline-friendly options:
1. `simple-audio-card` with a multi-codec link (`sound-dai = <&audmixer 0>, <&codec 0>`) and
   `bitclock-master`/`frame-master` on the CPU. Mainline samsung i2s.c `config_setup()` defaults
   to BFS = 2 x sample width and RFS 256 (BFS 16/32) or 384: for 16-bit that gives BFS 32 /
   RFS 256 (RCLK 12.288 MHz), and the mixer's boot default IN1 XFS is 32 fs too, so S16_LE at
   48 kHz should work with no machine code. 24-bit needs BFS 64 on both sides (mainline would
   pick 48 and rejects RFS 256/512 with 24-bit), and simple-card never calls `set_bclk_ratio` on
   the mixer: either restrict the card to S16_LE at first, or let the mixer derive XFS in its own
   `hw_params`, or use option 3.
2. `audio-graph-card2` (same idea, OF graph).
3. A small `samsung,exynos7870-audio` machine driver (like `tm2_wm5110.c`/`midas_wm1811.c`) if
   clock setup needs code. Prefer 1 or 2.
Userspace: ALSA UCM2 for pmOS (HiFi verb: Speaker/Earpiece/Headphones, Main/Secondary/Headset
mic); keep control names stable so an Android audio HAL can reuse them later.

## 10. Read-only checks to do first (phone not touched during this research)

Caution: reading a block whose clock is gated (or which is held in reset) can hang or abort.
First look at `/sys/kernel/debug/clk/clk_summary` and only `peek` blocks whose gates show enabled:
- CMU_DISPAUD (always readable, display uses it): AUD_PLL `peek 0x148d01c0` (CON0),
  `peek 0x148d01c4` (CON1: K), MUX AUD_PLL 0x148d0204, MI2S mux/div 0x148d022c/0x148d040c,
  MIXER div 0x148d0410, gates 0x148d0814 (APB_AUD/AMP/DISP), 0x148d082c (MI2S codclk),
  0x148d0830 (MIXER sysclk). Tells what S-Boot set up (S-Boot plays no sound, so probably off).
- DISPAUD_CFG `peek 0x148f1000` (in the same sysreg page as the display's 0x148f100c, so the
  sysreg itself is clocked): which audio IPs are in reset.
- PMU `peek 0x10480080` (mixer alive bit 0, BT/FM mux bit 1).
- Only if the gates above are on: mixer 0x14880000.., I2S0 0x148a0000 (CON +0, MOD +4).
- Codec: `i2cget -f -y <hsi2c0 bus> 0x04 0xd0` ... `0xde` (OTP trims) and 0x0b-0x0d (status)
  with `-a`; expect NACK/garbage while the codec has no MCLK. Never read 0x01-0x05 by hand.
  i2c bus numbers change between boots: find hsi2c0 by `10510000` in /sys/bus/i2c/devices.
- ADC: check the ADC gate in clk_summary first; it is not described in our DT yet, so it is
  probably gated: skip until the driver exists.

## 11. Effort and order

1. AUD_PLL fix (clk), ADC variant + node, ADMA + reset node, I2S compatible: small, each its own
   upstream patch, testable independently (ADC: read channels; ADMA: dmatest; I2S: needs 2.).
2. S1402X driver + binding.
3. COD3026X driver + binding: the bulk (DAPM + power sequences + jack).
4. Card in DT (simple/graph) + UCM in pmaports. Speaker playback is the first milestone, then
   EP/HP, mics, jack + buttons, BT SCO through the mixer's BT port, then 192 kHz.
Tracker: new groups for each of 1's patches, the mixer, the codec, and the A3 sound DT.
