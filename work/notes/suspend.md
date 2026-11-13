# Suspend (s2idle) on the A3: state 2026-11-13

s2idle (`echo freeze > /sys/power/state`) works: RTC alarm and power key wake, Wi-Fi loaded,
5 of 5 cycles on the final series. Deep sleep (`mem`) needs the vendor PMU sleep sequences and is
not started.

## The two bugs

1. imagis touchscreen: probe never called i2c_set_clientdata(), so imagis_suspend() dereferenced
   NULL on the first system suspend (the original "hangs at Suspending console(s)"). Kernel commit
   "Input: imagis - set the I2C client data" (Fixes: a23ba3c043db), tracker group AI.
2. CPUCL0 watchdog reset ~1 s after resume (looked like "dies 250 ms after suspend exit, no
   message"; also the apparent ath10k_sdio wiphy_resume hang, which was this bug):
   - The watchdog (0x10170000) sits in cluster 0's non-CPU power domain. With all four cluster-0
     cores in core power-down for long (s2idle, tick frozen), the PMU powers that domain down too.
     While it's off, the watchdog registers read reset values and ignore writes; when it comes back
     it runs from its defaults: WTCON 0x8021 (enabled, reset on), WTDAT/WTCNT 0x8000, ~1.0 s at the
     ~67 MHz input clock. If the domain comes back after the driver's resume restore and unmask, the
     board resets (PMU RST_STAT 0x10480404 bit 23 = 0x00800000).
   - Never happens at runtime, but it does with the vendor L2 option STANDBYWFIL2 (0x10482608
     bit 16) set: then cluster 0's domain goes down at runtime too. Don't port that vendor setting
     on its own. The vendor CPU OPTION bits (STANDBYWFE off / SMPEN on) made no difference.
   - The vendor never powers the boot cluster down on purpose either (exynos-powermode.c
     is_cpd_available: "no benefit in boot cluster").
   - Fix (folded into "soc: samsung: exynos-pmu: Add Exynos7870 CPU idle support", group G):
     exynos-pmu .prepare/.complete keep CPU0 out of its power-down idle states for an s2idle
     transition (CPUIDLE_STATE_DISABLED_BY_DRIVER, only bits it set itself). It must start before
     the watchdog suspend: doing it at noirq was too late (the domain already went down earlier
     in the transition). Only for PM_SUSPEND_TO_IDLE: deep sleep is not affected (secondaries
     offline, CPU0 runs resume, so the domain is up when the watchdog is restored).
   - Not proven / dropped: stopping the per-CPU MCT "oneshot wakeup" timers in tick_suspend_local().
   - Side effect (found 2026-11-13): with CPU0's power-down states disabled, s2idle is never
     "proper". cpuidle_enter_s2idle() only considers states >= 1 that have ->enter_s2idle, so CPU0
     gets index 0. It then falls back to tick_nohz_idle_stop_tick() + WFI with its hrtimers live,
     and never calls tick_freeze(). Timekeeping is never suspended (no "Timekeeping suspended for"
     on resume, even with pm_debug_messages; dmesg time runs on), and CPU0 keeps taking timer
     interrupts all through s2idle. The other CPUs do freeze their ticks.
   - Visible symptom: with the debug config's buddy hard-lockup detector, CPU0's watchdog hrtimer
     sees CPU1's hrtimer count stand still and logs "watchdog: CPU0: Watchdog detected hard LOCKUP
     on cpu 1" ~10-16 s into every s2idle longer than that (3/3 runs; CPU1's NMI backtrace sits in
     cpuidle_enter_s2idle). False positive: not related to cluster 1 power-down
     (cpuidle.md). A fix belongs in group G: CPU0 needs a way to stay up without losing tick
     freezing, or the cluster 0 domain must be kept on another way.

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
- tools/probes/s2idle.sh LEVEL [wfi] [quiet] [nowd] [probe], copied to /root on the phone. One
  command per run, wakes on the RTC alarm after 20 s (or a wake key), runs detached (USB drops),
  writes /root/s2idle-LEVEL.log. After a reset: /sys/fs/pstore/console-ramoops-0 (16 KiB; the
  verbose per-device log fills it with the resume alone, `quiet` keeps entry + wake) and
  pmsg-ramoops-0 (`probe`: watchdog registers every 0.2 s after resume).
- tools/probes/s2idle-charge.sh [CYCLES]: back-to-back s2idle cycles on a charger, logs wake IRQ,
  charger status/health and battery current per cycle to /root/s2idle-charge.log.
- Reset reason: `/usr/local/bin/peek 0x10480404` (PMU RST_STAT): 0x20000000 after `reboot`,
  0x00800000 (bit 23) after a watchdog reset. peek is not on the ssh PATH.
- Wake IRQs: 54 = gpa0-2 = S2MPU05 (RTC alarm), 78 = gpio-keys KEY_POWER. `SSP_Int` has wake
  enabled from probe. Watchdog state: WTCON/WTDAT/WTCNT = 0x10170000/04/08.

## Open

- Wakeup sources other than RTC and power: sensor hub (`SSP_Int` is wake-enabled: check it does
  not wake the phone constantly once sensors are enabled), USB plug, home key.
- Check after resume: sensor hub data, touch keys, display unblank under Phosh, BT.
