# Suspend (s2idle) on the A3: state 2026-11-14

s2idle (`echo freeze > /sys/power/state`) works: RTC alarm and power key wake, Wi-Fi loaded,
11 of 11 cycles without the old CPU0 hold, with every tick frozen (2026-11-14). Deep sleep (`mem`) needs the vendor PMU sleep sequences and is
not started.

## The two bugs

1. imagis touchscreen: probe never called i2c_set_clientdata(), so imagis_suspend() dereferenced
   NULL on the first system suspend (the original "hangs at Suspending console(s)"). Kernel commit
   "Input: imagis - set the I2C client data" (Fixes: a23ba3c043db), tracker group AI.
2. CPUCL0 watchdog reset ~1 s after resume (looked like "dies 250 ms after suspend exit, no
   message"; also the apparent ath10k_sdio wiphy_resume hang, which was this bug). Root cause
   found 2026-11-14:
   - Mainline s3c2410wdt_suspend sets PMU AUTOMATIC_DISABLE_WDT (0x10480408) bit 23 as well as
     MASK_WDT_RESET (0x1048040c) bit 23 (QUIRK_HAS_PMU_AUTO_DISABLE, exynos7 data). While that bit
     is set, the watchdog is held at its reset values: WTCON 0x8021 (enabled, reset on),
     WTDAT/WTCNT 0x8000, ~1.0 s at the ~67 MHz input clock. Runtime proof: poking bit 23 (reset
     masked first) gave WTCON 0x8021 / WTCNT 0x8000 at once, and after 60 s WTCNT was still
     ~0x7fe8 while cluster 0's NONCPU_STATUS stayed on (0x0008000f).
   - Mainline resume restored WTDAT/WTCNT/WTCON first and cleared 0x408/0x40c afterwards. The
     restore could be lost, and once the mask was cleared the watchdog ran from defaults and
     reset the board (PMU RST_STAT 0x10480404 bit 23). It was intermittent, depending on timing.
   - The vendor driver never sets 0x408 on suspend (it only masks) and on resume clears 0x408
     first, then stop + intclear, restore, unmask.
   - Fix: "watchdog: s3c2410_wdt: Clear the PMU automatic disable before restoring" (Fixes:
     4f1f653a68d6, generic). Verified 2026-11-14: 11/11 s2idle cycles (RTC + charger alarm),
     WTCON restored 0x7739, no reset.
   - It was NOT a cluster 0 power-down: the earlier theory and its workaround (CPU0 kept out of
     power-down during s2idle, group G) are gone. Runtime cluster 0 idle (120 s) and hotplug
     (cpu0-3 off 60 s) never changed NONCPU_STATUS. Don't offline CPU0: re-onlining it ended in
     a watchdog reset.
   - BL31 (RE, el3 dump): the only watchdog code is 0xba74 on the boot entries. It re-arms the
     watchdog only if AUTOMATIC_DISABLE_WDT bit 23 is set AND INFORM1 (0x10480804) == 0xbad
     (INFORM1 is 0 here). Core idle (power_state 0x10000) is mode 0 in 0xadb8 and writes no PMU
     sequencer, CENTRAL_SEQ or CL0 NONCPU register.
   - With the hold gone, s2idle now freezes every tick: "Timekeeping suspended for N s" on resume,
     and no more false "hard LOCKUP" reports. That exposed a ktime_get() in the cluster-1
     last-man check while timekeeping is suspended (WARN in timekeeping.c), fixed in the cluster
     power-down commit (clock only read when another CPU has a next_hrtimer).
   - Still true: the vendor L2 option STANDBYWFIL2 (0x10482608 bit 16) on its own makes cluster
     0's domain go down at runtime. Don't port it alone.

## Charging across suspend

The S2MU004 charger watchdog (80 s, left on by the bootloader) is cleared every 30 s. While a
charger is attached, a boottime wakeup alarm drives that (alarmtimer programs the RTC on suspend)
and a wakeup source holds the system until the clearing work has run, as the stock sec_battery does
(polling_time 30 s while charging). On battery it is a plain delayed work: no wakeups. The work
runs on the freezable workqueue, so it never touches I2C before resume. Folded into "power: supply:
s2m-charger: Add S2MU004 support" (group K). Verified 2026-11-13 with tools/probes/s2idle-charge.sh:
4 s2idle cycles in a row on a charger, each woken by the alarm after ~30 s (wake IRQ 54), status
Charging/Good throughout, "watchdog suspended charging" never logged.

## Tools

- Config (tools/mkconfig-a3y17lte): PM_DEBUG + PM_ADVANCED_DEBUG (pm_test, pm_print_times,
  pm_debug_messages), DPM_WATCHDOG 20 s / warn 5 s (needs EXPERT), soft/hard(buddy)-lockup and
  hung-task detectors. The lockup detectors' per-CPU hrtimers changed the timing enough to hide
  bug 2: turn them off for suspend tests with `echo 0 > /proc/sys/kernel/watchdog`.
- tools/probes/cl0-watch.sh SECONDS [domain]: run on cpu4 (taskset), logs every change of
  cluster 0's PMU state and the watchdog registers to stdout and /dev/pmsg0.
- tools/probes/s2idle.sh LEVEL [wfi] [quiet] [nowd] [probe], copied to /root on the phone. One
  command per run, wakes on the RTC alarm after 20 s (or a wake key), runs detached (USB drops),
  writes /root/s2idle-LEVEL.log. After a reset: /sys/fs/pstore/console-ramoops-0 (16 KiB; the
  verbose per-device log fills it with the resume alone, `quiet` keeps entry + wake) and
  pmsg-ramoops-0 (`probe`: watchdog registers every 0.2 s after resume).
- tools/probes/s2idle-charge.sh [CYCLES]: back-to-back s2idle cycles on a charger, logs wake IRQ,
  charger status/health and battery current per cycle to /root/s2idle-charge.log.
- Reset reason: `/usr/local/bin/peek 0x10480404` (PMU RST_STAT): 0x20000000 after `reboot`,
  0x00800000 (bit 23) after a watchdog reset. peek is not on the ssh PATH.
- Wake IRQs: 54 = gpa0-2 = S2MPU05 (RTC alarm, also the charger watchdog alarm), 68 = gpa2-7 =
  S2MU004 (cable plug), 78 = gpio-keys KEY_POWER, 86 = `SSP_Int` (wake enabled from probe). Watchdog state: WTCON/WTDAT/WTCNT = 0x10170000/04/08.

## Wake sources and resume checks (2026-11-14)

- USB/charger plug: the S2MU004 PMIC (pmic@3d, IRQ gpa2-7 = 68) is a wakeup source
  (`wakeup-source` in DT; the i2c core arms the IRQ). Plug-in during s2idle woke the phone at once
  (wake IRQ 68), and charging restarted. Without it, a charger plugged in during suspend charged
  for only 80 s (charger watchdog, alarm not armed).
- The charger "done" source toggles every ~7 s while a full battery sits on a weak port. The
  charger driver masks it across system sleep, so it can't wake the phone: on the PC port at 100%,
  3/3 s2idle cycles slept until the 30 s charger alarm (IRQ 54).
- Sensor hub: with accel + mag buffers and prox events on, s2idle slept the full 20 s (RTC wake).
  `SSP_Int` doesn't wake the phone: the hub stops streaming after AP_STATUS_SUSPEND. Data resumed
  at the same ~5.5 Hz right after wake (tools/probes/ssp-s2idle.sh; the plug test is tools/probes/plug-wake.sh).
- After resume: touch keys work (IRQ 84), BT works (hci0 powered, scan finds devices), Wi-Fi
  reconnects. Not checked: display unblank under Phosh (pmOS).
