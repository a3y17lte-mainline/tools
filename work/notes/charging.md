# S2MU004 "proper charging": cable detection, currents, limits, OTG (research 2026-11-12)

Research only: nothing was written to the phone. Covers what is left of the S2MU004 after the
fuel gauge and the charger watchdog (both working, see CLAUDE.md "Battery / charger"):
- charger-type detection (the MUIC block in 0x3d);
- input and charge current per cable;
- battery temperature limits;
- the OTG boost (VBUS out, needed by usb-c.md);
- Samsung AFC 9 V (optional).

Sources:
- vendor `drivers/battery_v2/s2mu004_charger.c` + `include/charger/s2mu004_charger.h`,
  `drivers/muic/s2mu004-muic.c` + `s2mu004-muic-afc.c`, `include/linux/muic/s2mu004-muic*.h`,
  `include/linux/mfd/samsung/s2mu004-private.h`, `sec_battery.c` (policy, read for numbers only);
- stock DT `battery {}`, `s2mu004-fuelgauge@3B`;
- mainline/our branch: `drivers/power/supply/s2m-charger.c` (S2MU004 + S2MU005 variants),
  `include/linux/mfd/samsung/s2mu004.h`, `drivers/mfd/sec-common.c`, the S2MU005 extcon binding;
- Kaustabh Chakraborty's extcon series on the list:
  [PATCH v9] extcon: add support for Samsung S2M series PMIC extcon devices,
  https://lkml.iu.edu/2606.2/00481.html (2026-06-16). Its MFD/LED/charger parts are already in
  our tree; the extcon driver (`drivers/extcon/extcon-s2m.c`) was not merged as of v9.

**Every write to the charger block needs the user's approval** (CLAUDE.md safety rule). That covers
input/charge current, float voltage, OTG mode and AFC (which raises VBUS to 9 V). The MUIC
switch/BC1.2 registers are in the same I2C device: treat them the same way.

## 1. Register layout: S2MU004 vs S2MU005 MUIC

The MUIC bit definitions are identical to the S2MU005's (mainline s2mu005.h). Only the addresses
differ, so the S2MU004 is a register-offset variant of extcon-s2m, not a new driver.

| function | S2MU004 | S2MU005 |
|---|---|---|
| MUIC INT1 / INT2 | 0x06 / 0x07 | 0x04 / 0x05 |
| MUIC INT1M / INT2M | 0x08 / 0x09 | 0x06 / 0x07 |
| ADC | 0x61 | 0x49 |
| DEV1 / DEV2 / DEV3 | 0x62 / 0x63 / 0x64 | 0x4a / 0x4b / 0x4c |
| BUTTON1/2, RESET | 0x65/0x66, 0x67 | 0x4d/0x4e, 0x4f |
| CHG_TYPE, DEV_APPLE | 0x68, 0x69 | 0x50, 0x51 |
| BCD_RESCAN | 0x6a (bit0 = start secondary D+/D- detection) | 0x52 |
| MUIC_CTRL1 | 0xc7 | 0xb2 |
| TIMER_SET1/2 | 0xc8 / 0xc9 | 0xb3 / 0xb4 |
| SW_CTRL (D+/D- switch) | 0xca | 0xb5 |
| TIMER_SET3 | 0xcb ([2:0] DCD timeout, vendor sets 4 = 0.6 s) | 0xb6 |
| CTRL2 = RID_CTRL | 0xcc (bit1 = RID ADC off) | 0xb7 |
| CTRL3 | 0xcd | 0xb8 |
| LDOADC VSETL / VSETH | 0xd4 / 0xd5 | 0xbf / 0xc0 |
| chip ID | 0x82 REV_ID (this unit 0x38) | 0x73 |

Bits (same on both): DEV1 b7 OTG, b6 DCP, b5 CDP, b4 T1/T2 charger, b3 UART, b2 SDP;
DEV2 jig/TTY/PPD; DEV3 b7 U200 charger, b1 VBUS R255; DEV_APPLE b3 SDP DCD-out (= unknown
"floating" charger), b0 BC1.2-or-open. SW_CTRL [7:2] D-/D+ switch: 1/1 = USB, 2/2 = UART, 0 = open;
bit1 charger path, bit0 OTG enable. MUIC INT1: b0 attach, b1 detach, b5 RID change;
INT2: b0 VBUS on, b2 ADC change, b7 VBUS off (our sec-irq.c already declares these as
S2MU004_IRQ_MUIC_*; s2m-charger already takes "vbus-on"/"vbus-off").

### extcon-s2m S2MU004 variant: what to add

- MFD cell in sec-common.c: `MFD_CELL_OF("s2mu004-muic", s2mu004_muic_resources, ..,
  "samsung,s2mu004-muic")` with "attach"/"detach" IRQs (S2MU004_IRQ_MUIC_ATTACH/DETACH).
- Binding: add `samsung,s2mu004-muic` to samsung,s2mu005-muic.yaml.
- Driver: a per-variant register table (INT/ADC/DEV1/SWCTRL/CTRL1/CTRL3/LDOADC addresses) instead
  of the hard-coded S2MU005_REG_* macros; same cable map (OTG -> USB_HOST, SDP -> USB+CHG_USB_SDP,
  CDP -> USB+CHG_USB_CDP, DCP -> CHG_USB_DCP, UART -> JIG). v6 of the series dropped "variant
  conditionals" on Lee Jones' request, so expect to argue for a table, or wait until it is merged
  and send the S2MU004 variant as a follow-up.
- A3 difference: on a Type-C phone **OTG/host is decided by the CC lines (the PD chip), not by
  the MUIC ID ADC**. The MUIC only does BC1.2 (SDP/CDP/DCP) and the D+/D- switch. The extcon
  driver must not drive host mode on the A3; DT can describe this by not linking the MUIC extcon
  to the USB controller (role switching comes from the tcpm connector).
- Vendor init for the Type-C build (`s2mu004_muic_reg_init`, s2mu004-muic.c:3013): INT1M = 0xFC,
  INT2M = 0x7A (only attach, detach, VBUS on/off, ADC change unmasked), DCD timeout 0.6 s
  (0xcb[2:0] = 4), RID ADC on (0xcc bit1 = 0, for water detection: we can leave it off),
  0xda bit4 cleared (AFC OTP, unknown), CTRL1 = 0x13 (switch open bit, wait, int-mask bit; manual
  switch bit 2 = 0 -> automatic switching), LDOADC VSETL = 3.0 V, VSETH = 2.7 V.
  Note the CTRL1 bit0 "INT_MASK" is written 1 by vendor while extcon-s2m *clears* bit0 to unmask
  S2MU005 interrupts: check the polarity on the S2MU004 (read CTRL1 on the phone, section 6)
  before copying either.
- BC1.2 flow on VBUS (vendor "first/second attach", s2mu004-muic.c:2540-2620): on VBUS with ADC
  = 0 (Type-C, no ID resistor) set RID_CTRL bit1 (RID ADC off), wait 100 ms, set BCD_RESCAN bit0
  to start D+/D- detection, then DEV1/DEV_APPLE report the type with the next attach IRQ. The
  mainline driver needs the same rescan on VBUS-on, because with the CC-based attach the MUIC sees
  no ID change.

## 2. Charger registers needed (0x3d, from s2mu004_charger.h / .c)

Our s2mu004.h names the CTRLn registers 0x10 + n. Encodings (vendor .c line numbers):

| reg | field | encoding | survey value (cable in, bootloader) |
|---|---|---|---|
| CTRL0 0x10 | [3:0] mode | 0 off, 1 buck, 3 charge, 6 OTG boost | 0x83 (charge) |
| CTRL2 0x12 | [6:0] input current limit | mA = code*25 + 50, code 2..0x62 (100..2500 mA) (:338) | 0x12 = 500 mA |
| CTRL3 0x13 | [6:0] wireless input limit | not used on the A3 (no wireless) | |
| CTRL4 0x14 | [3:2] OTG OCP limit | vendor writes 3 before OTG (:101); mA per code unknown (S2MU005: 1.5 A setting) | |
| CTRL6 0x16 | [5:0] float voltage | mV = 3900 + code*10, max 0x3f (4530 mV) (:304) | 0x2d = 4350 mV |
| CTRL7 0x17 | [6:5] VF_VBYP (OTG output voltage) | vendor: 0 before boost, 2 after 1.5 s (soft start); values in mV unknown | 0xc6 -> 2 |
| CTRL8 0x18 | cool charging current | vendor caps at 0x11 (450 mA) | |
| CTRL9 0x19 | [6:0] fast charge current | mA = (code+1)*25, code 3..0x7d (100..3150 mA) (:418) | 0x11 = 450 mA |
| CTRL11 0x1b | [3:0] 1st top-off, [7:4] 2nd top-off | mA = 100 + code*25 (:466) | 0x06 -> 250 / 100 mA |
| CTRL13 0x1d | bit1 WDT enable, bit0 WDT AP reset | watchdog (done) | |
| CTRL14 0x1e | bit0 WDT clear | done | |
| CTRL17 0x21 | [2:0] WDT time, top-off timer field | 80 s / 30 min | |

Other vendor init writes (s2mu004_chg_init, :528), all undocumented magic, do NOT copy blindly:
0x32 |= 0x03 (poor-charge IRQ mask), 0xb0 bit4 = 0 (self-discharge), 0xb2[7:4] = 0 (QBAT switch
speed), 0xc0[7:6] = 3 (top-off debounce 1 s), CTRL12 buck frequency (1 MHz: stock
`battery,is_1MHz_switching = <1>`), CTRL15 boost oscillator 1 MHz, IVR recovery in CTRL13.
The bootloader already set most of them; read them (section 6) and only write what differs from
what we need.

## 3. Charging policy: stock numbers (decoded from the stock DT)

Per cable type (`battery,input_current_limit` / `fast_charging_current` indexed by the vendor's
power_supply_type enum, include/linux/power_supply.h):

| cable | input limit | charge current | mainline source of the type |
|---|---|---|---|
| USB SDP (PC) | 500 mA | 800 mA | MUIC DEV1 SDP |
| USB CDP | 1500 mA | 1500 mA | MUIC DEV1 CDP |
| USB DCP (dumb wall charger) | 900 mA | 1200 mA | MUIC DEV1 DCP |
| MAINS / UNKNOWN / UARTOFF | 1500 mA | 1775 mA | |
| Type-C Rp 1.5 A / 3.0 A, PD 5 V ("PDIC") | min(Rp/PD current, 1500 mA) | 1500 mA region | tcpm psy CURRENT_MAX |
| AFC 9 V (HV_MAINS) | 1650 mA | 3000 mA (clamped by `max_charging_current` 2100 mA and step charging) | AFC (section 5) |
| OTG (phone is source) | 450 mA | - | |

- Float voltage `battery,chg_float_voltage` = 4350 mV (bootloader value already).
- Full: `full_check_current_1st` 245 mA, `2nd` 122 mA (top-off); full condition SOC 93 % and
  VCELL 4250 mV; recharge at VCELL < 4280 mV or SOC 98 %.
- Safety timer: `charging_total_time` 21600 s (6 h), recharge 5400 s.
- Temperature (0.1 °C units), "normal" mode:
  - stop charging above 50.0 °C, resume below 45.0 °C;
  - stop below 0 °C, resume above 5 °C;
  - "swelling" (reduced charging): above 41 °C block / 39 °C recover -> 825 mA, float 4150 mV;
    below 15 °C (1st step, recover 20 °C) -> 550 mA; below 5 °C (2nd step, recover 10 °C);
  - "highlimit" 80 °C (emergency).
- Battery: SDI cell, `fuelgauge,capacity_full` = 2450 (mAh, 0x992), design 2350 mAh
  per Samsung spec. `capacity_max` 1000 = 100.0 % (no stock scaling actually configured beyond
  the margin 200, so our raw RSOC is close to what stock showed).
- Battery thermistor: SoC ADC channel 1 (`io-channel-names = "adc-temp"`), table ADC -> 0.1 °C:
  ```
  adc:  288  348  403  483  549  645  734  882 1036 1200 1378 1645 1776 1907 1998 2030 2259 2500 2715 2928 3113 3278 3381 3519 3616
  temp: 900  850  800  750  700  650  600  550  500  450  400  350  320  290  270  250  200  150  100   50    0  -50 -100 -150 -200
  ```
  ADC channel 3 = "adc-in-bat" (battery voltage through a divider; table in the stock DT, not
  needed: the fuel gauge measures voltage).

### Mainline shape (proposal)

- Battery temperature: the exynos7870 ADC variant (audio.md section 6) + an
  `generic-adc-thermal` node (`compatible = "generic-adc-thermal"`, io-channels = <&adc 1>,
  `temperature-lookup-table` from the table above in m°C) giving a thermal zone `battery-thermal`.
  The fuel-gauge driver (or the charger) reads it through `monitored-battery` + the thermal zone,
  or the charger reads the IIO channel directly. Simplest upstream-acceptable route: thermal zone
  with trip points at 45/50 °C and 0/5 °C and the charger as a cooling device? There is no generic
  "charger cooling device" for JEITA; look at how other mainline chargers do JEITA (bq25890 has
  hardware JEITA, rt9467/mt6370 use `charge-term`/`temp_*` from simple-battery). Recommendation:
  `simple-battery` node with `operating-range-celsius = <0 50>`, `alert-*`, and a small JEITA
  check in s2m-charger's existing 30 s watchdog work (it already runs): if the battery temperature
  is outside [0, 50] °C, set charge mode off (buck only); resume with the 5 °C hysteresis. The
  reduced-current "swelling" bands are optional.
- Battery description: `monitored-battery = <&battery>` on the fuel gauge and the charger:
  ```dts
  battery: battery {
  	compatible = "simple-battery";
  	device-chemistry = "lithium-ion";
  	charge-full-design-microamp-hours = <2350000>;
  	voltage-max-design-microvolt = <4350000>;
  	constant-charge-voltage-max-microvolt = <4350000>;
  	constant-charge-current-max-microamp = <1500000>;   /* raise to 1775000/2100000 later */
  	charge-term-current-microamp = <245000>;
  	operating-range-celsius = <0 50>;
  };
  ```
  The stock OCV/SOC tables (`battery_params`) are fuel-gauge internal calibration (written into
  the gauge's battery table registers by the vendor gauge driver). Our gauge uses the bootloader's
  values; do not write them.
- s2m-charger S2MU004 additions (each a charger write, needs the user's yes):
  1. POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT (CTRL2) and CONSTANT_CHARGE_CURRENT(_MAX) (CTRL9),
     CONSTANT_CHARGE_VOLTAGE (CTRL6, read-only at first), CHARGE_TERM_CURRENT (CTRL11).
  2. USB_TYPE from the MUIC extcon (same as the S2MU005 path in s2m-charger, which already reads
     EXTCON_CHG_USB_SDP/DCP/CDP) and the tcpm power supply.
  3. `external_power_changed`: input limit = max(BC1.2 table above, Type-C/PD CURRENT_MAX),
     capped at 1500 mA (stock `max_input_current`); charge current from the battery node.
  4. OTG regulator (section 4).
- Policy beyond that (step charging, "SIOP" limits during calls/screen on, CISD statistics,
  6 h safety timer) stays out. The charger chip has its own top-off timer (30 min) and done
  detection.

## 4. OTG boost (VBUS out, for USB-C host)

Vendor `s2mu004_charger_otg_control` (s2mu004_charger.c:85):
- on: CTRL7 VF_VBYP = 0, CTRL4 (0x14) bits[3:2] = 3 (OTG OCP), wait 30 ms, CTRL0 mode = 6
  (OTG boost); after 1.5 s CTRL7 VF_VBYP = 2 (raise output voltage after soft start, :1226);
- off: CTRL0 mode = 3 (back to charge mode, not "off": with nothing on VBUS that is buck idle).
Mainline: a regulator in s2m-charger (`regulator_desc` with enable/disable ops doing the above,
fixed 5 V, `regulator-name = "otg-vbus"`, a `regulators { otg-vbus {}; }` child or an of_node on
the charger cell), used by the usb-c-connector `vbus-supply`. The S2MU005 path in s2m-charger
switches OTG from extcon USB_HOST; the S2MU004 must not do that (CC decides). Report OTG faults
(IRQ S2MU004_IRQ_CHGR_OTG_FAULT, INT2 bit) through `regulator_notifier_call_chain`
(REGULATOR_EVENT_OVER_CURRENT) and turn the boost off.
Open: mA per OCP code and mV per VF_VBYP code (no table in the vendor header). Read CTRL4/CTRL7
in section 6; the S2MU005 datasheet-derived values in mainline (OCP 1.5 A, OTG 5.1 V) suggest the
same encodings, but don't assume.

## 5. Samsung AFC (9 V over D+/D-), optional, later

Protocol engine in the MUIC block, registers 0x48-0x5f (s2mu004-private.h), IRQs AFC_INT 0x04
(already in our irq.h: VBADC, VDNMON, DNRES, MPNACK, MRXTRF, MRXPERR, MRXRDY):
- AFC_STATUS 0x48: [3:0] VBUS ADC, b4 VDNMON, b5 DNRES, b6 MPNACK.
- AFC_CTRL1 0x49 (HVCONTROL1): b0 DPDNVDEN, [2:1] DN voltage, [4:3] DP voltage, b5 VBUS ADC en,
  b6 CTRLIDMON, b7 AFC enable.
- AFC_CTRL2 0x4a (HVCONTROL2): b1 DP 0.6 V en, b2 DN res en, b3 MPING, b4 MTX en, b7 RST DM 100UI.
- TX_BYTE1 0x4b: [7:4] voltage (0 = 5 V ... 4 = 9 V ... 7 = 12 V), [3:0] current code;
  RX_BYTE1-16 0x4c-0x5b.
- Prepare (s2mu004-muic-afc.c:1339): after DCP detection, 200 ms, CTRL1 = 0xa0, TX_BYTE1 = 0x46
  (9 V, 1.65 A), CTRL1 = 0xa1, CTRL2 = 0x06 (DP 0.6 V + DN res), then ping (MPING) and wait for
  the charger's reply; on success VBUS rises to 9 V and the charger input limit becomes 1650 mA.
- QC 2.0 fallback: CTRL1 = 0xbd (D+ 0.6 V / D- 3.3 V) selects 9 V on QC chargers, 0xa1 back to 5 V.
Mainline has no AFC framework. If wanted: an optional part of the extcon/MUIC driver exposing a
"fast charge" knob, or a power_supply property; out of scope until basic charging is done. Raising
VBUS to 9 V is a charger-side change: needs the user's explicit approval and a thermal story.

## 6. Read-only checks (when the phone is free)

With the drivers bound reads need `i2cget -f`; on pmOS `sudo`. i2c-1 = 13870000 (&i2c4, 0x3d);
check bus numbers via /sys/bus/i2c/devices/*/name first. Do NOT read clear-on-read registers:
0x00, 0x01 (charger INT), 0x04 (AFC INT), 0x06, 0x07 (MUIC INT); the regmap-irq driver owns them.

Read in states (a) no cable, (b) PC USB, (c) dumb wall charger, (d) Samsung AFC charger if the
user has one (it stays at 5 V without the AFC handshake), (e) OTG adapter:
- MUIC: 0x08 0x09 (masks), 0x61-0x6a, 0xc7-0xcd, 0xd4 0xd5;
- charger: 0x10-0x24 (CTRL0-20; CTRL4 0x14 and CTRL7 0x17 for the OTG encodings), 0x0a-0x0f;
- AFC: 0x48-0x4b (status/ctrl only);
- `cat /sys/class/power_supply/*/uevent`.
Expected: DEV1 = 0x04 (SDP) on the PC, 0x40 (DCP) on a wall charger, SW_CTRL 0xca = 0x24
(D-/D+ = USB) or auto. If DEV1 stays 0 on the charger, BC1.2 did not run (no rescan) and the
MUIC driver must trigger it.

## 7. Order

1. ADC variant + battery thermistor thermal zone (also listed in next-areas.md; no phone writes).
2. MUIC: S2MU004 variant of extcon-s2m (ask on the list where v9 stands first), read-only at
   first (no switch writes: just report types).
3. s2m-charger: USB_TYPE + input-current setting per type (user approval for the writes), then
   JEITA stop/resume in the watchdog work, then the OTG regulator.
4. tcpc driver (usb-c.md), then input current from the tcpm psy.
5. AFC only if the user wants faster charging, with explicit approval.
