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
