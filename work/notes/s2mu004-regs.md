# S2MU004 register survey (read-only, i2cget), 2026-11-08

Kernel 7.4.0-rc1-a3y17lte+ #42, RAM boot, rootfs on SYSTEM. No S2MU004 drivers bound.
Adapters: i2c-0 = 13830000 (&i2c0: FG 0x3b, PD 0x3c), i2c-1 = 13870000 (&i2c4: IF-PMIC 0x3d).
Decoded against src/vendor-kernel/drivers/battery_v2/include/{charger,fuelgauge}/s2mu004_*.h.
Clear-on-read INT registers (charger INT, FG 0x02, PD 0xE0-E4) were NOT read.

## State A: USB cable in (VMware host USB 2.0 port)

### Fuel gauge 0x3b
| reg | raw | decoded |
|---|---|---|
| 0x00 STATUS | 0x0310 | |
| 0x04 RVBAT | 0x891c | 4284 mV (x*1000>>13) |
| 0x06 RCUR_CC | 0xfa15 | +370 mA, charging (mainline sign: -(s16)x*1e6>>12) |
| 0x08 RSOC | 0x3905 | 89.1 % raw (x*100>>14) |
| 0x25 CTRL0 | 0x10 | |
| 0x48 FG_ID | 0x2b | |

### IF-PMIC / charger 0x3d
REV_ID 0x82 = 0x38 (rev 3, ver 8).
| reg | raw | decoded |
|---|---|---|
| 0x0A STATUS0 | 0xa1 | CHGIN[7:5]=5 -> stock treats 0xA0 as "charging" (VBUS valid) |
| 0x0B STATUS1 | 0x00 | fault[6:3]=0 (none), not done/top-off, bit7 (discharging)=0 |
| 0x0C-0x0F STATUS2-5 | 03 0b 12 25 | |
| 0x10 CTRL0 | 0x83 | REG_MODE[3:0]=3 = CHG_MODE (buck + charger on) |
| 0x12 CTRL2 | 0x12 | input current limit 18*25+50 = 500 mA |
| 0x16 CTRL6 | 0x6d | float voltage VF_VBAT=0x2d -> 3900+450 = 4350 mV |
| 0x19 CTRL9 | 0x11 | fast-charge current (x+1)*25 = 450 mA |
| 0x1D CTRL13 | 0x02 | **SET_EN_WDT=1 (charger watchdog ON)**, WDT_AP_RESET=0 |
| 0x21 CTRL17 | 0x1d | WDT_TIME[2:0]=5 -> 80 s |
| 0x72 PWRSEL_CTRL0 | 0x80 | |
Full CTRL0-20 (0x10-0x24): 83 05 12 25 10 dd 6d c6 91 11 0a 06 12 02 00 ec 1b 1d 55 55 05

### USB-C/PD 0x3c (for the later PD phase)
01=03 02=87 18=c1 19=01 1e=09 1f=2a 26=00 28=00 2e=20 2f=22 3e..42=00 b2=00 b3=e2 b4=26

## Watchdog finding (cable in)
First full read at ~30 s uptime: STATUS1=0x00, FG current +370 mA (charging).
Samples after that:
  up=48.55  STATUS0=0xa1 STATUS1=0x18 CTRL0=0x83 RCUR=0x0004 (~0 mA)
  up=103.71 STATUS0=0xa1 STATUS1=0x18 CTRL0=0x83 RCUR=0xffec (~+5 mA)
STATUS1 fault[6:3] = 3 = CHG_STATUS_WD_SUSPEND. The charger watchdog (CTRL13 SET_EN_WDT=1, 80 s)
was last cleared by the bootloader; nothing under mainline kicks it, so **charging stops ~80 s
after boot**. VBUS still powers the system (battery current ~0), so on the cable the battery
holds steady but does not charge; off the cable it discharges normally.
Stock recovery (s2mu004_wdt_clear): set CTRL14 bit0 (WDT_CLR) every poll; if the fault reads
WD_SUSPEND/WD_RST and charging is wanted, toggle the charger switch off/on.

## Result with drivers (kernel #44, commits 814e3fe0081f..63d3153fec4d)
s2m-charger S2MU004 variant clears the watchdog (CTRL14 bit0) every 30 s: charging holds past
80 s (+335 mA at 157 s uptime, health Good). Unplug/replug test via /root/battlog.txt (20 s samples):
cable out -> charger online=0 Discharging, FG ~-150 mA (Wi-Fi idle), 4.31 V -> 4.05 V over 5.5 min,
capacity 90 -> 89 %; cable back in -> online=1 Charging ~+350 mA immediately, no WD restart needed.

## Survey 2 (2026-11-12, Alpine, kernel with ADC only, PC cable, before the MUIC driver)
i2c bus numbers: Alpine i2c-1 = 13830000 (FG 0x3b, PD 0x3c), i2c-2 = 13870000 (0x3d); pmOS the same.
Logger: tools/probes/s2mu004-survey.sh (log copy in out/survey/).
- PD 0x3c: 01=01 (LPM off, unlike survey 1's 0x03), 18=e3 (DRP, Rp 180 uA), 1f=34 (SET_RP 2057 mV),
  27=10, b3=d4 (CC1 4, CC2 2: the connected CC reads 2; the cable was flipped vs survey 1's e2),
  b4=26 (attached as sink). So the bootloader state of the PD chip varies between boots.
- PD IRQ gpa0-5 (gpa0 DAT 0x139f0044 = 0xcd, bit 5 = 0): LOW at idle with no driver = events pending
  and not cleared -> the line is level-low; use IRQ_TYPE_LEVEL_LOW + IRQF_ONESHOT in the tcpc driver.
- MUIC with the ID (RID) ADC on (0xcc = 0x1d): ADC 0x61 = 0x00, DEV1 = 0x80 (OTG) on a PC cable.
  The ID pin is not connected on the Type-C port, so DEV1 says OTG for everything. With the RID ADC
  off (0xcc bit1, our extcon driver) ADC reads 0x1f and a BCD rescan gives DEV1 = 0x20 = CDP:
  the VMware host port is a charging downstream port.
- Charger: CTRL2 = 0x3a (1500 mA input), CTRL9 = 0x46 (1775 mA) this time, vs 500/450 mA in
  survey 1: the bootloader picks different limits per boot.
- MUIC CTRL1 0xc7 = 0x13 (bit0 set) while the VBUS IRQs arrive: on the S2MU004 bit0 does not gate
  the MUIC interrupts. No write needed.

## MUIC + charger drivers (2026-11-12, user-approved writes)
- extcon-s2m S2MU004: RID ADC off at probe, BCD rescan once when VBUS has no type, result re-read
  after 1.5 s. The attach/detach IRQs for the probe-time rescan arrive ~12 s late (or not at all on
  some boots; the PMIC parent IRQ fired with no child handled): cause not found, the re-read covers it.
- s2m-charger S2MU004: input/charge current per BC1.2 type (CDP 1500/1500 verified), JEITA from the
  battery-thermal zone (emul_temp 42/51/44/30 C verified: 825 mA, buck "Not charging", 825, 1500).

## Cable session + OTG (2026-11-12 evening; logs in out/survey/)
- MUIC CTRL1 bit0 = interrupt mask (vendor clears it at the end of probe, like S2MU005). With it
  set, MUIC attach/detach only got noticed when another S2MU004 source fired the parent IRQ: the
  "late or lost" attach IRQs. Cleared by our driver now: unplug/replug reported within 1 s.
- BCD_RESCAN (0x6a bit0) is NOT trustworthy: after turning the RID ADC off it reported the PC port
  as CDP (DEV1 0x20), a fresh plug-in reports the same port as SDP (0x04). Dropped: a cable present
  at probe stays "Unknown" (500/500 mA) until replugged.
- OTG boost (CTRL0 mode 6) on for 30 s with the hub: STATUS2[7:5] = 4 while boosting (0 otherwise),
  PD chip saw the hub (b4 = 0x6b, source attached, b3 = d1). Battery current only ~20 mA higher than
  idle; hub LED not confirmed yet. Off -> CTRL0 0x83 cleanly.
- PD charger and laptop without a tcpc driver: VBUS comes and goes (the PD chip keeps DRP-toggling,
  b4 0x26 <-> 0x41/0x00), MUIC gets no BC1.2 type. Needs the TCPM driver (usb-c.md).
- PLUG_MON2 values seen: 0x26 attached as sink (PC, PD charger, laptop), 0x6b attached as source
  (hub), 0x41 / 0x00 toggling/unattached. PLUG_MON1: d4/c9/cc/e4/d1 across states.
