# RTC and torch (2026-11-13)

## RTC (S2MPU05)

- The RTC is in the S2MPU05 PMIC: its own I2C client at 0x06 (0x0C >> 1) on hsi2c0, next to the
  PMIC at 0x66. Mainline already has it: sec-common gives S2MPU05 an `s2mps15-rtc` cell with the
  RTCA0 alarm IRQ, and rtc-s5m uses the S2MPS15 register map. The vendor rtc-sec.c agrees (S2MPU05
  uses the "REV" UDR bits = S2MPS15's WUDR bit 1 / AUDR bit 4, year base 2000, and the same
  probe-time CTRL write: binary, 24 h). No DT change is needed.
- It was missing only because CONFIG_RTC_DRV_S5M was off: the pmOS config has it =y, but the first
  olddefconfig in mkconfig-a3y17lte dropped it (MFD_SEC_CORE off at that point). mkconfig now sets
  it (=y, so RTC_HCTOSYS works at boot). RTC_SYSTOHC (11-minute mode) writes it once ntpd syncs.
- Probe writes RTC_CTRL + UDR, the same as the vendor kernel on every boot.

## Torch (Richtek RT8547)

- Stock DT `leds_rt8547`: flash_control = gpd3-3 = FLSET (one-wire), flash_en = gpd3-2 = FLEN (LED
  on; also the fimc-is flash GPIO). Stock currents: torch 100 mA, factory 350 mA, pre-flash 150 mA,
  flash 1200 mA; the stock torch UI steps are 25/50/75/100/125 mA.
- Datasheet: Richtek DS8547-02 (Feb 2018), richtek.com. One-wire: FLSET high >= 10 us before the
  first falling edge; bit 1 = low tL (4..180 us) then high > 2 tL; bit 0 = low > 2 tH, high tH
  (>= 4 us). FLSET low >= 1.5 ms = shutdown (registers reset). Frame: 0x99 (8 bits), register
  (3 bits), value (8 bits), end (low, then high). VIN 2.8-5.5 V.
- Registers: 1 LVP [3:0] 3.0 V + 0.1 V steps (reset 3.6 V); 2 TimeI [7:5] 100..400 mA (50 mA steps),
  strobe current [4:0] 100 mA + 50 mA steps (max 1.6 A); 3 Sreset [5], Selstr [4] (1 = torch),
  torch current [3:0] 25 mA + 25 mA steps; 4 strobe timeout [5:0] 64 ms + 32 ms steps (max 1216 ms).
- Driver: drivers/leds/flash/leds-rt8547.c (LED flash class, module), ExpressWire with the new
  expresswire_write_bits(). Bits 10/40 us. LVP 3.0 V and TimeI 400 mA as the vendor kernel.
  DT: torch max 125 mA (5 steps), flash 1.2 A / 1216 ms. Tracker groups AG (driver) and AH (DT).
- Test: `/sys/class/leds/white:flash/brightness` 0..5; strobe with flash_brightness (uA),
  flash_timeout (us), flash_strobe.

## Status (verified on Alpine 2026-11-13)

- RTC: rtc0 registers at boot and sets the system clock (the RTC already held correct UTC time);
  the alarm IRQ fires (`echo +5 > /sys/class/rtc/rtc0/wakealarm` -> rtc-alarm0 count +1).
  openrc's hwclock service no longer complains at boot (user, 2026-11-13).
  Keeps time across a full power-off (user-confirmed 2026-11-13).
- Torch: leds_rt8547 autoloads, `white:flash` appears; torch steps 1-5 and a 1 A / 512 ms strobe
  light the LED (user-confirmed); the strobe ends by itself (flash_strobe reads 0 afterwards).
