# Sensor hub (STM32F410 "SSP") on the Galaxy A3 2017: research for a clean driver

Researched 2026-11-12 from the vendor driver (`src/vendor-kernel/drivers/sensorhub/stm32/`,
config `CONFIG_SENSORS_SSP_STM32` + `_2017A3`), the stock DT, the stock sensors HAL
(`/system/lib/hw/sensors.universal7870.so` from `work/system/SYSTEM.img`), the hub firmware blob and
mainline `drivers/iio/common/ssp_sensors/`. Nothing was run on the phone.

## 1. Hardware

All A3-specific (the J5 2017 has no hub). The AP sees only the MCU; the sensors hang off the MCU's
own I2C buses and are invisible to Linux.

| Sensor | Part (HAL name / FW strings) | Android type id used on the wire |
|---|---|---|
| Accel + gyro | ST LSM6DSL (FW drives it as "K6DS3"; register-compatible K6DS3TR) | 1 accel, 4 gyro, 16 gyro uncal |
| Magnetometer | AKM AK09916C | 2 mag, 14 mag uncal, 30 geomag power |
| Barometer | ST LPS25H (`ssp-pressure-type = <1>` = STM; FW: "Barometer_LPS25H") | 6 |
| Light + prox (+RGBC) | AMS TMD3725 | 5 light, 8 prox, 29 prox raw, 34 light CCT |
| Virtual (MCU-computed) | rotation vector, game RV, geomag RV, step det/counter, sig motion, tilt, pickup | 11, 15, 20, 18/19, 17, 22, 25 |
| IR LED "MOBEAM" barcode emulation | via the TMD3725 IR LED | 33 (ignore) |

The fingerprint sensor (ET5xx) is on another SPI (100c0000) and goes through the secure world:
out of scope, like the secure pinctrl banks.

### Wiring (stock DT `spi@13930000` / `STM32F@0`)

| Signal | Pin | Direction | Vendor DT flags | Notes |
|---|---|---|---|---|
| SPI SCK | gpc4-2 | | func 3 | mainline group `spi4_bus` (gpc4-2, gpc5-0, gpc5-1) |
| SPI MOSI | gpc5-1 | | func 3 | |
| SPI MISO | gpc5-0 | | func 3 | |
| SPI CS | gpc4-3 | | `cs-gpio` in vendor | mainline group `spi4_cs` (output) or `spi4_cs_func` |
| MCU_INT1 (data ready IRQ) | **gpa0-7** | MCU->AP | falling edge, no pull | the IRQ; gpa0 is an alive/wakeup EINT bank |
| MCU_INT2 (handshake ack) | gpd3-7 | MCU->AP | input, no pull | polled |
| AP_INT (handshake req) | gpd4-2 | AP->MCU | output, idle high, pull-up | |
| MCU nRST | gpd2-3 | AP->MCU | output, low = reset | gpd2 is also mainline `uart0_bus`, but serial0 is unused here (BT is serial1 = gpb0) |
| MCU BOOT0 | gpd4-3 | AP->MCU | output, low (pull-down in pinctrl) | high only for the ST system bootloader |

Bus: SPI mode, 5 MHz in DT (`spi-max-frequency = <5000000>`, cpol+cpha) but the driver forces
**SPI_MODE_1** (CPHA only) and 4.8 MHz in normal operation; the STM32 ROM bootloader is used in
**SPI_MODE_0**. No DMA on this port (stock DT gives `dma-names` but no `dmas`): PIO is fine.
No regulators in the node: the MCU is on an always-on rail.

Stock DT board data (A3-specific, needs DT properties in the clean binding):
`ssp,acc-position = <5>`, `ssp,mag-position = <7>` (MCU-side axis remap indices, sent as-is),
prox thresholds hi 0x37 / lo 0x28 / detect-hi 0xfa / detect-lo 0x82,
`ssp-mag-type = <1>` (AKM) with `ssp-mag-array` = 27 zero bytes (AKM "PDC" matrix, unused/zero on
the A3), `ssp-acc-type = <1>`, `ssp-pressure-type = <1>` (LPS25H). The light coefficients come
from the driver's `default_project` table: `{-830, 1100, -1180, 1000, 925, 3521, 2095}`
(R, G, B, C, DGF, CT, CT_OFFSET), sent once at init.

### The SPI controller: naming trap

The vendor DT alias calls 13930000 "spi2", but in mainline naming it is **SPI4**:
- CMU: vendor `SPI_SENSORHUB` = CMU_MIF mux 0x2c0 / div 0x4c0 / gate 0x8c0 and CMU_PERI PCLK
  BUS1 bit 8 + EXT_CLK 0x84c = mainline `CLK_GOUT_PERI_SPI4_PCLK` + `CLK_GOUT_PERI_SPI4_SPI_EXT_CLK`
  (parent chain `gout_mif_cmu_peri_spi4`, which has CLK_SET_RATE_PARENT: good, the driver sets
  the rate on the ext clock).
- Mainline mapping of the other ports, for completeness: SPI0 = ESE (secure, gpc7, leave alone),
  SPI1 = REARFROM, SPI2 = FRONTFROM, SPI3 = VOICEPROCESSOR.
- Pins: mainline groups `spi4_bus`/`spi4_cs` (gpc4/gpc5); mainline's `spi2_bus` is gpf4 = a
  different port.
- IRQ: GIC SPI 455 (0x1c7). Stock `samsung,spi-fifosize = <0x100>` on all five 7870 ports.

Mainline `spi-s3c64xx`: `samsung,exynos7-spi` assumes FIFO masks {0x1ff,0x7f,0x7f,0x7f,0x7f}
indexed by port id (the `spi4` alias): a 64-byte FIFO on port 4. That works (the driver never
fills more than it thinks exists) but it is wrong; add a `samsung,exynos7870-spi` port config
with `fifo_depth = 256` (all ports), `rx_fifomask = S3C64XX_SPI_ST_RX_FIFO_RDY_V2` (bits 23:15)
and `tx_fifomask = S3C64XX_SPI_ST_TX_FIFO_RDY_V2` (bits 14:6): that is exactly the vendor's
`rx_lvl_offset = 15` with mask `(fifosize << 1) - 1` = 0x1ff, and TX at shift 6. Plus
`tx_st_done = 25`, `clk_div = 2` (vendor sets the ext clock to 2 x speed), `high_speed`,
`clk_from_cmu`, `QUIRK_CS_AUTO`. That is the exynos850 config with fifo_depth 64 -> 256 and
clk_div 4 -> 2 (loopback support unknown: leave `has_loopback` off). With `fifo_depth` in the port
config the alias no longer matters. Binding: add the compatible to `samsung,spi.yaml`.

## 2. Protocol ("SSP", Samsung's 2017 STM32 firmware)

It is the same framing as mainline `ssp_sensors` (written for the 2014 Gear 2 "rinato" firmware),
with a few differences in payloads. All multi-byte fields are little-endian.

### Transport

Every AP->hub message starts with a 9-byte packed header:
`u8 cmd, u16 length, u16 options, u32 data`.
`options & 3` = message type: 0 AP2HUB_READ, 1 AP2HUB_WRITE, 2 HUB2AP_WRITE, 3 READY; bit 2
(value 4) = RETURN (the hub sends a 1-byte status back after a write).

Sending (both drivers, `do_transfer` / `ssp_do_transfer`), under one mutex:
1. AP_INT := 0; wait until MCU_INT2 == 0 (poll 3 ms, give up after ~1.5 s).
2. SPI write the 9 header bytes.
3. If `length != 0`, put the message on the pending list (matched later by `options`).
4. AP_INT := 1; wait until MCU_INT2 == 1.
5. Return. Payload in either direction moves later, driven by the hub's IRQ.

On MCU_INT1 falling (threaded IRQ; the vendor ignores it if the line already went back high):
1. SPI read 4 bytes: `u16 options, u16 length`.
2. type READ/WRITE: find the pending message with the same `options`;
   READ -> SPI read `msg->length` bytes into its buffer; WRITE -> SPI write its payload, and if
   RETURN was set re-queue it as READ|RETURN with length 1 for the status byte; then complete.
   Unknown `options`: read and discard `length` bytes.
3. type HUB2AP_WRITE: SPI read `length` bytes = a data frame (below).

A command with `length == 0` (e.g. AP state notifications, `data` carries the argument) is
fire-and-forget after step 4.

### Commands the init sequence needs (vendor `initialize_mcu`, order matters)

| Cmd | Name | Dir/len | A3 value / meaning |
|---|---|---|---|
| 0x0f | WHOAMI | read 1 | must be **0x55** (retry 3x, 5 ms apart; first transfer after reset may fail) |
| 0xf1 | SENSOR_FORMATION | write 3 | `{acc_pos, acc_pos, mag_pos}` = `{5, 5, 7}` (gyro uses the accel position) |
| (0x7d) | SET_6AXIS_PIN | write 1 | only if `ssp,acc-dot` in DT: **not on the A3**, skip |
| 0xf4 | SENSOR_SCANNING | read **8** | u64 bitmap of present sensors, bit n = type n (mainline reads 4: wrong for this FW) |
| 0xfd | SET_MAGNETIC_STATIC_MATRIX | write 27 | AKM path: the 27-byte "PDC" array (all zero on the A3); only if bit 2 (mag) is in the scan bitmap |
| 0x49 | SET_LIGHT_COEF | write 28 | 7 x s32 light coefficients above; only if bit 5 (light) present |
| 0xf0 | FIRMWARE_REV | read 4 | u32 decimal date-ish, e.g. 18011800; 99999 or 0xffffff = invalid (no app FW) |

Then per-sensor: on first enable of the proximity sensor send 0xf2 PROXTHRESHOLD, write 4 bytes
`{hi, lo, detect_hi, detect_lo}` (TMD3725 path: 4 bytes, values truncated to u8).

Other commands worth supporting: 0xd0 SHUTDOWN, 0xd1 WAKEUP, 0xd2 SLEEP, 0xd3 RESUME,
0xd4 SUSPEND (send SUSPEND in system suspend, RESUME after; the hub keeps running and can wake
the AP through MCU_INT1, e.g. for sig-motion), 0xd6/0xd7 POW_CONNECTED/DISCONNECTED,
0xdd BATCH_FLUSH (read 1, data = type), 0xdf BATCH_COUNT (read 4), 0xfe SET_TIME (write 12:
h, m, s, am/pm, wday, mon, mday, yy, u32 ns; cosmetic, for the hub's logs), 0xdb SET_DUMPMODE.
Calibration: 0xce SET_ACCEL_CAL (3 x s16, the vendor reads them from /efs/FactoryApp; we would
expose IIO `calibbias` and let userspace persist), 0xcd SET_GYRO_CAL.

### Enable / rate / disable (vendor `ssp_sysfs.c`)

- ADD (0xa1), payload after the 1-byte type: `s32 delay_ms, s32 batch_latency_ms, u8 batch_opt`
  (10 bytes in total including the type). Delay is clamped to 200 ms max.
- CHANGE_DELAY (0xa4): same payload, when already running.
- REMOVE (0xa2): `{type, s32 delay_ms}`.
- Instructions for a type not in the scan bitmap are refused by the driver (types <= 0xa4 only).

### Data frames (HUB2AP_WRITE): the main difference from mainline

A frame is a sequence of records, each starting with a 1-byte tag:

| Tag | Record body |
|---|---|
| 0x37 BYPASS_DATA | `u8 type, u16 count`, then `count` x (`payload[len(type)]`, `u32 ts_delta_us`) |
| 0x03 DEBUG | `u8 len`, ASCII (`u16 len` only on the A3 "premium" FW, not ours) |
| 0x01 LIBRARY | `u16 len`, opaque "scontext" bytes (skip) |
| 0x04 BIG_DATA | `u8 big_type, u32 length, u32 addr` (crash dump / library read: skip) |
| 0x05 META | `u8 what, u8 type` (flush-complete marker for batched sensors) |
| 0x06 TIME_SYNC | none: restart timestamp tracking |
| 0x07 RESET | none: the MCU rebooted; re-run the init sequence and re-enable sensors |
| 0x08 GYRO_CAL | 3 x s16 new gyro bias |
| 0x45 GYRO_CAL_EVENT | none |
| 0xdd DUMP | rest of frame (stop parsing) |

Mainline's parser assumes `0x37, type, payload, u32 time` (no count, single event, rinato type
numbering). The A3 FW batches: `count` can be > 1 and every sample carries a microsecond delta.
Timestamps: the vendor keeps `last_ts[type]` in boottime ns, sets it to "now" on the first sample
after enable/resume, then adds the deltas and clamps to "now". Use the same idea, in
`iio_get_time_ns()`'s clock.

Payload sizes and meaning (`ssp_sensorlist.h`, `struct sensor_value`, HAL `sensor_t` tables):

| Type | Bytes | Layout | Unit (from the HAL resolution) |
|---|---|---|---|
| 1 accel | 6 | 3 x s16 | 1/4096 g per LSB (HAL res 0.0023942 m/s^2 = 9.80665/4096; range +-8 g) |
| 4 gyro | 6 | 3 x s16 | HAL res 0.000610865 rad/s = 0.035 dps/LSB (+-1000 dps); the vendor `gyro_dps` default says 2000 dps: verify at rest/with a known rotation |
| 2 mag | 7 | 3 x s16 + u8 accuracy | calibrated by the MCU; HAL res 0.06 uT/LSB (AK09916 raw is 0.15 uT/LSB: verify against Earth field ~50 uT) |
| 14 mag uncal | 12 | 3 x s16 uncal + 3 x s16 offset | same unit |
| 16 gyro uncal | 12 | 3 x s16 + 3 x s16 bias | same as gyro |
| 6 pressure | 6 | s32 pressure + s16 temperature | LPS25H: pressure probably in 1/4096 hPa (LPS25H native) or Pa*100; HAL res 1 hPa says nothing. Verify: ~1013 hPa at sea level, ~-0.12 hPa/m. Temperature likely 1/100 C |
| 5 light | 18 | u32 lux, s32 cct, u16 r, g, b, c(w), u8 atime, u8 again (&3) | lux already computed on the MCU with the coefficients |
| 34 light CCT | 18 | same | |
| 8 prox | 2 | u8 near(1)/far(0)?, u8 raw | HAL maxRange 8, i.e. 0 = near, 8 = far after HAL mapping: verify the polarity of byte 0 by covering the sensor |
| 29 prox raw | 1 | u8 | factory |
| 11/15/20 rotation vectors | 17 | 4 x s32 quaternion + u8 accuracy | |
| 17, 18, 22, 25 | 1 | u8 event | |
| 19 step counter | 4 | u32 steps since last report (vendor accumulates into u64) | |

The firmware decides the Samsung-private type numbers (27 + n: 28 accel-int, 29 prox raw,
30 geomag power, 31 interrupt gyro, 32 scontext, 33 mobeam, 34 light CCT). Read the scan bitmap
at first contact and log it; that is the ground truth for this FW.

## 3. Firmware (the MCU's flash)

- `work/firmware/vendor-kernel/ssp_stmf410_a2017.fw` (114467 bytes, sha256 in the README) is a
  raw STM32 flash image for 0x08000000 (vector table: SP 0x20000848, reset 0x0801bbdd), FW
  revision 18011800 per `ssp_firmware.h`. The stock ROM's kernel may have flashed a newer one,
  so the chip's `FIRMWARE_REV` can differ: that is fine.
- The vendor driver re-flashes on every boot whose revision differs, through the **ST system
  bootloader over SPI** (AN4286 protocol: BOOT0=1, pulse nRST, switch to SPI mode 0, sync byte
  0x5a, ACK 0x79 / NACK 0x1f, extended erase 0x44 with `ff ff 00` = mass erase, write memory
  0x31 in 256-byte blocks with XOR checksums, GO 0x21 to 0x08000000).
- **Safety**: flashing the MCU is a persistent chip write and a half-flashed MCU loses all sensors
  until re-flashed. The clean driver must never flash on its own. It should accept whatever
  revision is running (log it), refuse to run only when WHOAMI fails or the revision reads
  invalid, and leave flashing to an explicit, separately approved step (a userspace tool through
  spidev, or a driver sysfs trigger that is off by default). Any flash needs the user's yes,
  like the charger writes.
- Mainline `ssp_dev.c` refuses to probe if the revision differs from a compiled-in number and has
  no flashing code at all (`"Firmware version not supported"`): that check has to become
  "warn and continue" for the A3 variant.

## 4. Mainline: extend `ssp_sensors` or write a new driver?

Recommendation: **extend `drivers/iio/common/ssp_sensors/`** with a per-firmware "protocol
variant" chosen by compatible, because Jonathan Cameron (IIO maintainer) will ask why a second
driver for the same protocol exists. Work needed:

1. Fix the `__packed__` bug (see next-areas.md) as a separate patch first.
2. `struct ssp_sensorhub_info` grows: type-id map (enum `ssp_sensor_type` -> wire id), frame
   format flag (batched with u16 count + per-sample u32 delta vs. rinato), scan bitmap width
   (8 bytes), firmware revision check policy (exact match vs. "any valid"), and an optional
   init hook (formation/positions, light coefficients, mag matrix from DT instead of the
   compiled-in rinato table).
3. New compatible, e.g. `samsung,sensorhub-a3y17lte` (the existing ones are per product:
   `samsung,sensorhub-rinato`, `-thermostat`), extending `samsung,sensorhub-rinato.yaml` with
   optional properties for positions, prox thresholds and light coefficients. Note the binding
   requires `mcu-reset-gpios`, `ap-mcu-gpios`, `mcu-ap-gpios` and `interrupts`; add an optional
   `boot0-gpios`. MCU_INT1 is the `interrupts` (IRQ_TYPE_EDGE_FALLING), MCU_INT2 = `mcu-ap-gpios`,
   AP_INT = `ap-mcu-gpios`.
4. GPIO polarity: mainline's reset toggle does `gpiod_set_value(rst, 0)` then `(rst, 1)`, i.e. it
   treats the descriptor as "1 = running". With `GPIO_ACTIVE_HIGH` in DT that matches the vendor
   raw levels (0 = reset). Either describe it that way (odd but works with the existing driver)
   or fix the driver to assert/deassert with `GPIO_ACTIVE_LOW` (cleaner, but changes rinato's DT
   semantics: better leave rinato alone and keep ACTIVE_HIGH).
5. New MFD cells / IIO drivers next to the existing `iio/accel/ssp_accel_sensor.c` and
   `iio/gyro/ssp_gyro_sensor.c`: `ssp-magnetometer` (iio/magnetometer), `ssp-pressure`
   (iio/pressure, with temperature channel), `ssp-light` (iio/light: illuminance processed +
   intensity R/G/B/clear raw + CCT), `ssp-proximity` (iio/proximity, with an IIO event for
   near/far so it can wake userspace). Each provides `scale` (missing even in the mainline
   accel/gyro drivers: add it, iio-sensor-proxy needs it) and `sampling_frequency`.
6. Accel/gyro "mount matrix": the MCU already remaps axes using `acc-position`; expose
   `mount-matrix` = identity unless testing shows otherwise. Check with iio-sensor-proxy in
   Phosh: rotation must follow the phone.
7. Virtual sensors (rotation vector, step counter, sig motion...): later, optional. Skip MOBEAM,
   scontext/library, big-data and factory commands entirely.
8. Suspend: send SUSPEND/RESUME; keep MCU_INT1 as a wakeup IRQ only if a wake-capable sensor
   (prox, sig motion) is enabled.

Alternative if the maintainers dislike variants: a new `samsung,ssp-2017` core in the same
directory sharing the transport file. Decide after an RFC.

Effort: transport and init are small (the protocol is simple); most work is the four new IIO
drivers and the units.

## 5. Bring-up plan (low risk first)

0. Read-only phone checks (no driver, `peek` only; both pin controllers are in use already, so
   clocked). Bank layout: +0 CON (4 bits/pin: 0 input, 1 output, 2+ function, 0xf EINT),
   +4 DAT, +8 PUD, +0xc DRV (offsets from `exynos7870_pin_banks*` in pinctrl-exynos-arm64.c):
   - gpa0 (alive 0x139f0000 + 0x40): `peek 0x139f0040` CON, `peek 0x139f0044` DAT bit 7 =
     MCU_INT1 (idle high expected), bit 4 = codec IRQ.
   - gpd2 (top 0x139b0000 + 0x120): `peek 0x139b0120`, `peek 0x139b0124` bit 3 = nRST.
   - gpd3 (+0x140): `peek 0x139b0144` bit 7 = MCU_INT2.
   - gpd4 (+0x160): `peek 0x139b0160`, `peek 0x139b0164` bit 2 = AP_INT, bit 3 = BOOT0.
   - gpc4 (+0x060) / gpc5 (+0x080) CON: are the SPI pins still in SPI function (3) from the
     bootloader or in reset state (input)?
   Is nRST an output driven high by S-Boot, or an input on an external pull? If the MCU is held in
   reset now, nothing works until a driver releases it (a GPIO write: harmless, but ask first).
1. DT: SPI4 controller node (dtsi, `status = "disabled"`) + board: enable it and add the hub node
   with its final compatible. Before the driver exists nothing binds, so for a local test bind
   spidev by hand: `echo spidev > /sys/bus/spi/devices/spi4.0/driver_override` and
   `echo spi4.0 > /sys/bus/spi/drivers/spidev/bind` (needs CONFIG_SPI_SPIDEV=m; no fake
   compatible in DT). GPIOs via libgpiod (`gpioset`/`gpioget`, Alpine package libgpiod), leaving
   nRST/BOOT0 untouched unless step 0 showed the MCU is held in reset. Do the handshake in a small
   C or Python script, not shell (timing). spidev in SPI mode 1, 4.8 MHz, CS per transfer.
   A userspace WHOAMI test does: AP_INT low, wait MCU_INT2 low, write header
   `0f 01 00 00 00 00 00 00 00` (cmd 0x0f, length 1, options 0 = READ, data 0), AP_INT high, wait
   MCU_INT2 high, wait MCU_INT1 low, read 4 bytes (expect options 0x0000, length 0x0001), read 1
   byte, expect 0x55. Then FIRMWARE_REV (0xf0, len 4) and SCANNING (0xf4, len 8). All three are
   reads: no state changes on the hub.
2. Driver with accel only (rotation in Phosh), then light/prox (auto-brightness and screen-off in
   calls), then mag + pressure.
3. Tracker: new upstream groups for (a) SPI 7870 port config + dtsi SPI nodes, (b) the ssp
   `__packed` fix, (c) ssp variant + bindings + new IIO drivers, (d) A3 DT.

## 6. Things not to copy from the vendor driver

wakelocks and `wake_lock_timeout` on prox events (use `pm_wakeup_event` only if needed), the
`/sdcard/ssp/*.bin` UMS firmware path and `filp_open` from the kernel, `/efs` calibration files,
the sysfs `enable` bitmask ABI and `sensors_event_class` devices, the misc devices
(`batch_io`, `shtc1`, `ssp_sensorhub` library pipe), the "IIO device as a byte pipe" with one
fake timestamp channel per sensor, debug timers that reset the MCU, the big-data dump tasks, the
factory selftests, `CONFIG_SENSORS_SSP_*` per-chip Kconfig symbols (detect from the scan bitmap
instead), `gpio_to_irq` + legacy GPIO numbers.

## 7. Implementation status (2026-11-13: working, all six sensors)

Kernel (src/linux a3y17lte, 22 commits after "Add the USB-C port controller", squashed 2026-11-13;
pre-squash tip tagged a3y17lte-pre-ssp-squash):
- pinctrl: wakeup EINT digital filter on (Fixes:). THE transport bug: without it, glitches on
  MCU_INT1 (gpa0-7) start transfers the hub never set up; it then clocks out dummy bytes
  (headers `02 02 02 02`, "unknown frame type 0x02") and stalls ~10 s, 3-4 times per 90 s.
  The vendor kernel enables the filter. Registers 0x139f0800+ = 0xc0c0c0c0 when on.
- spi: binding + s3c64xx 7870 port config; dtsi SPI4 with the mux on OSC (26 MHz -> ext 8.67 MHz
  -> SCK 4.33 MHz). From the 799.5 MHz bus PLL the divider bottoms out at 6.25 MHz SCK.
- ssp mainline fixes (separate, Fixes:-style): `__packed`, restore only enabled sensors after a
  hub reset, unregister consumers on removal (use-after-free on rmmod while samples arrive),
  SPI ID table, platform ID tables for accel/gyro (they never autoloaded), no CHANGE_DELAY to
  stopped sensors.
- ssp A3 variant: timestamp in the core, hub-info firmware differences, binding, A3 support
  (batched frames, init sequence, BOOT0 low, reset + 500 ms at probe), accel/gyro scale, raw
  reads + read_raw/write_raw helpers, the other sensors (sample sizes, prox thresholds).
- New IIO drivers: ssp_magn_sensor, ssp_pressure_sensor, ssp_light_sensor, ssp_prox_sensor.
- Board DTS: hub node (reset described ACTIVE_HIGH, see section 4 point 4).
Config: SPI_S3C64XX=y, IIO_SSP_SENSORHUB/COMMONS=m, SPIDEV=m. Alpine needs mdev + hwdrivers in
sysinit for modalias autoload (build-rootfs and a3-sync-modules set them since 2026-11-13).

Units as implemented (sysfs value x scale; verified against reality where noted):
- accel 1/4096 g (z ~4018 flat), gyro 35 mdps/LSB.
- mag scale 0.0006 G = 0.06 uT/LSB (HAL). Rotation test: horizontal circle radius 386 LSB,
  i.e. 0.050-0.053 uT/LSB against the local ~20 uT horizontal field; accuracy byte stayed 0
  (hub never calibrated in the test). Recheck once calibration converges.
- pressure 1/4096 hPa (raw 4031891 = 984 hPa); temperature (raw + 20400) / 480 C (LPS25H).
- light: lux and CCT processed on the hub; RGBC raw. Prox: raw count (cover/uncover seen).

Tests: 90 s all-sensor capture without gaps or hub errors on the final series; module reload
clean (no Oops); everything autoloads at boot on Alpine (#93, 2026-11-13).

Tools: tools/probes/ssp-probe (spidev, read commands only; -r releases nRST), ssp-pins.sh.
SPI4 registers as bus 0 (no alias): device spi0.0.

Open: iio-sensor-proxy/Phosh rotation check (mount matrix = identity assumed); prox near/far
as an IIO event; suspend (SUSPEND/RESUME commands, wake IRQ); virtual sensors (step counter,
sig motion); calibration (calibbias). Firmware stays 16112800 (never flashed).

First contact details (spidev, before the driver):
- Pins after boot at reset state (input) except SPI once the controller probes. nRST read high on
  one boot, 0 on another: the driver pulses reset at probe anyway. BOOT0 reads 0 (pull-down).
- Mode 1 at 4.8 MHz works. The first WHOAMI timed out at the MCU_INT2 handshake; the hub then
  sent an unsolicited `07` (RESET) frame and the retry returned 0x55.
- FIRMWARE_REV = 16112800 (older than the vendor blob's 18011800; not flashed, accepted).
- SENSOR_SCANNING = 0x00000000f24f4176: 1 accel, 2 mag, 4 gyro, 5 light, 6 pressure, 8 prox,
  14 mag uncal, 16 gyro uncal, 17 sig motion, 18 step det, 19 step counter, 22 tilt, 25 pickup,
  28 accel int, 29 prox raw, 30 geomag power, 31 interrupt gyro. No rotation vectors (11/15/20)
  in this firmware.
