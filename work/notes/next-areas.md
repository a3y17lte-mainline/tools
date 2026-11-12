# Next areas: sensor hub, audio and their shared prerequisites (research 2026-11-12)

Research only, nothing was written to the phone and the phone was not touched. Detailed notes:
- `sensorhub.md`: STM32F410 "SSP" sensor hub on SPI (accel/gyro, mag, baro, light/prox).
- `audio.md`: S1402X audio mixer + I2S + ADMA + COD3026X codec, jack via the SoC ADC.

Sources used (all local): vendor kernel `src/vendor-kernel` (lineage-24.0), stock DT
`work/stock-dt/stock-02.dts` (rev02 = this unit), stock ROM `work/system/SYSTEM.img`
(sensors HAL, audio HAL, mixer_paths.xml), the sensor hub blob `work/firmware/vendor-kernel/`,
and mainline `src/linux` branch a3y17lte (read via `git show a3y17lte:...`, the work tree may
carry someone else's uncommitted display work).

The rule for both areas: use Samsung's code as a register/protocol reference only. Every driver
below is a clean mainline-style driver (regmap, DAPM, IIO, gpiod, devm, runtime PM), no wakelocks,
no sysfs/misc-device side channels, no /efs or /sdcard file access from the kernel, no
"factory" attributes.

## What is A3-specific vs. reusable for every 7870 phone

| Piece | Scope | Notes |
|---|---|---|
| SPI controller (spi-s3c64xx, 7870 port config) | every 7870 | 256-byte FIFO on all 5 ports per stock DT |
| ADC (exynos_adc "v3" variant) | every 7870 | battery/AP thermistors, jack detection |
| PL330 ADMA (audio DMA) + PDMA0 | every 7870 | mainline pl330 as-is |
| AUD_PLL clock fix (pll_1431x, not 1417x) | every 7870 | bug in mainline clk-exynos7870.c |
| DISPAUD sysreg resets (ADMA/AMP/MI2S/MIXER) | every 7870 | sysreg 0x148f1000 bits 3..0 |
| I2S (samsung i2s.c, new 7870 compatible) | every 7870 | register layout = mainline `i2sv5_i2s1_regs` |
| S1402X audio mixer driver | every 7870 (and 7880?) | new ASoC component driver |
| COD3026X codec driver | every 7870 phone (Samsung's 7870 reference codec) | new ASoC codec driver |
| Machine driver | generic `simple-audio-card`/`audio-graph-card2` if possible; else a small samsung 7870 card | per-board routing in DT |
| Sensor hub (STM32F410, "SSP" protocol, Samsung FW) | A3 2017 (and A5/A7 2017 on Exynos 7880 with the same hub) | extends mainline `iio/common/ssp_sensors` |
| Sensor positions, prox thresholds, light coefficients | A3 only | DT properties |

J5 2017 (already upstream) has no sensor hub (sensors on I2C), but it does have the same
COD3026X + S1402X audio chain, so all audio work except board DT applies to it too.

## Suggested order

1. **Read-only checks on the phone** (when it is free; the display work is using it now). Listed at
   the end of `sensorhub.md` and `audio.md`. All are `peek` reads of registers that are already
   clocked, or sysfs/debugfs reads. No pokes, no I2C writes.
2. **Shared infrastructure** (small, upstreamable, low risk, no phone writes):
   a. ADC: new `samsung,exynos7870-adc` variant (data at +0x08, see audio.md section 6), DT node.
      Gives battery temperature on the side (stock DT: ch1 battery temp, ch3 battery voltage-in,
      ch0 AP thermistor with a calibration table in the stock DT).
   b. SPI: `samsung,exynos7870-spi` port config + the SPI4 node (sensor hub port).
   c. AUD_PLL as `pll_36xx` (register-compatible with Samsung's pll_1431x) + rate table.
   d. pl330 ADMA node and the DISPAUD reset provider.
3. **Sensor hub**: first a spidev + libgpiod WHOAMI test (read-only protocol commands), then the
   kernel driver. Independent of audio. Gives screen rotation in Phosh (iio-sensor-proxy) and
   auto-brightness, which matter more for daily use than audio.
4. **Audio**: I2S + ADMA + mixer + codec playback to the speaker first, then earpiece/headphone,
   mics, jack detection, buttons.

Both areas can run in parallel after step 2 (different files, different subsystems).

## Bugs found in mainline along the way (each a small upstream patch)

- `drivers/clk/samsung/clk-exynos7870.c`: CMU_DISPAUD `fout_dispaud_aud_pll` is registered as
  `pll_1417x` (integer PLL). Samsung's CAL (`pwrcal/S5E7870/S5E7870-cmu.c:54`) says AUD_PLL is a
  `pll_1431x`: fractional, K in CON1 bits [15:0] (signed), M [25:16] (10 bits but spec max 511),
  P [13:8], S [2:0], enable bit 31, lock bit 29. Register-compatible with mainline `pll_36xx`.
  Without K the readback rate is wrong and 98.304 MHz (48 kHz family) cannot be set.
- `drivers/iio/common/ssp_sensors/ssp_dev.c:30`: `struct ssp_instruction {...} __packed__;`
  declares a global variable named `__packed__` instead of packing the struct, so
  `sizeof(struct ssp_instruction)` is 12, not 9, and every ADD/CHANGE_DELAY instruction sends 3
  extra bytes. Present since the driver was added (50dd64d57eee, 2015). Fix: `__packed`.
  Also `ssp_send_instruction()` allocates `length + 2` where Samsung's newer driver uses
  `length + 1` (type byte + payload); check against the A3 firmware before changing.
