# Suspend (s2idle) on the A3: state 2026-11-13

Only s2idle (`echo freeze > /sys/power/state`). Deep sleep (`mem`) needs the vendor PMU sleep
sequences and is not started.

## Tools

- Config (tools/mkconfig-a3y17lte): PM_DEBUG + PM_ADVANCED_DEBUG (pm_test, pm_print_times,
  pm_debug_messages), DPM_WATCHDOG 20 s / warn 5 s (needs EXPERT), soft/hard(buddy)-lockup and
  hung-task detectors. Note: the lockup detectors' per-CPU hrtimers HIDE the open bug below;
  turn them off for tests with `echo 0 > /proc/sys/kernel/watchdog`.
- tools/probes/s2idle.sh LEVEL [wfi] [quiet] [nowd] [probe], copied to /root on the phone. One
  command per run, wakes on the RTC alarm after 20 s, runs detached (USB drops), writes
  /root/s2idle-LEVEL.log. After a reset: /sys/fs/pstore/console-ramoops-0 (16 KiB, the verbose
  per-device log fills it with the resume only; `quiet` keeps entry + wake) and
  pmsg-ramoops-0 (`probe`: watchdog registers every 0.2 s after resume).
- Reset reason: `/usr/local/bin/peek 0x10480404` (PMU RST_STAT): 0x20000000 after `reboot`,
  0x00800000 (bit 23) after a watchdog reset. peek is not on the ssh PATH.
- Wake IRQ 54 = gpa0-2 = S2MPU05 (RTC alarm). `SSP_Int` has wake enabled from probe.

## Results

| pm_test | Result |
|---|---|
| freezer | ok |
| devices | NULL deref in imagis_suspend (fixed, below); then hard hang in ath10k_sdio's wiphy_resume |
| devices, ath10k_sdio unloaded | ok |
| platform (late + noirq), no ath10k | ok |
| none, cpuidle state1 disabled (WFI only) | ok, wakes on RTC, stays up |
| none, core power-down allowed | intermittent: wakes, resumes completely, then the console stops ~250 ms after "PM: suspend exit" (right after the mmc0/mmc2 re-init lines) and the watchdog resets the board ~13 s later (RST_STAT bit 23) |

Fixed:
- imagis never called i2c_set_clientdata(), so imagis_suspend() dereferenced NULL on the first
  system suspend. Kernel commit "Input: imagis - set the I2C client data" (Fixes: a23ba3c043db).

Open 1, ath10k_sdio: `wiphy_resume` (mac80211 restarting the firmware over SDIO) never returns
and the whole system stops ~100 ms later. Workaround for tests: `rmmod ath10k_sdio`.

Open 2, post-resume stop with core power-down:
- Timers: local clockevent = arch_sys_timer (stops in core power-down, `local-timer-stop`),
  the per-CPU MCT timers (mct_tick0-7) are the tick-broadcast "oneshot wakeup" devices
  (thousands of mct_tick IRQs, ~0 broadcast IPIs), mct-comp is the unused global broadcast.
- Tried (uncommitted, src/linux working tree): stop the wakeup device in tick_suspend_local()
  (kernel/time/tick-*.c), since tick_freeze() only shuts the tick device and an armed MCT event
  could run the tick handler with timekeeping suspended. Failures after it: 3 of 11 runs (before:
  1 of 1). Not proven to help.
- Pass/fail is timing dependent: with the lockup detectors on, 0 of 5 failed.
- In a `nowd` run the board also reset by itself with RST_STAT still 0x00800000, but the boot
  before was a watchdog reset too, so that value may be stale. The pmsg probe of passing runs
  shows WTCON restored correctly (0x7739 running / 0x7718 stopped).
- Next: catch one failure with `probe` (detectors off, watchdog on) to see whether userspace
  still runs after the console stops and what the watchdog registers say; then look at what
  cpuidle state1 entry needs after a long all-core power-down (vendor exynos-powermode.c:
  cluster/PMU state the firmware may change when every core of a cluster is down for seconds).
