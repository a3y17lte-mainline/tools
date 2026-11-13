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
- Reset reason: `/usr/local/bin/peek 0x10480404` (PMU RST_STAT): 0x20000000 after `reboot`,
  0x00800000 (bit 23) after a watchdog reset. peek is not on the ssh PATH.
- Wake IRQs: 54 = gpa0-2 = S2MPU05 (RTC alarm), 78 = gpio-keys KEY_POWER. `SSP_Int` has wake
  enabled from probe. Watchdog state: WTCON/WTDAT/WTCNT = 0x10170000/04/08.

## Open

- Wakeup sources other than RTC and power: sensor hub (`SSP_Int` is wake-enabled: check it does
  not wake the phone constantly once sensors are enabled), USB plug, home key.
- Check after resume: sensor hub data, touch keys, display unblank under Phosh, BT.
- Long sleeps while charging: the S2MU004 charger watchdog (80 s) is kicked by a frozen work item;
  charging stops during sleep and the driver recovers it on resume. Changing that is a charger
  setting (needs the user's yes).
