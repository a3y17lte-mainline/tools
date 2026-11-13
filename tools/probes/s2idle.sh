#!/bin/sh
# s2idle test run on the phone (Alpine, root). Usage: s2idle.sh LEVEL [FLAG...]
#   LEVEL = freezer | devices | platform | none   (/sys/power/pm_test)
#   wfi   = disable cpuidle state1 (core power-down) on all CPUs first
#   quiet = no pm_print_times (the 16 KiB pstore console then keeps entry + wake)
#   nowd  = stop the hardware watchdog, so a hang stays up long enough for the
#           RCU stall / lockup detectors to print (needs a forced reset after)
#   probe = after resume, log the watchdog registers 30x every 0.2 s to
#           /dev/pmsg0 (pstore pmsg survives a reset)
# Wake source: RTC alarm in 20 s (or a wakeup key). Returns at once; the
# suspend runs detached because USB/ssh drop. Result: /root/s2idle-LEVEL.log,
# or after a hang /sys/fs/pstore/console-ramoops-0 on the next boot.
set -u
LEVEL=${1:?usage: s2idle.sh freezer|devices|platform|none [wfi] [quiet] [nowd] [probe]}
shift
FLAGS=" $* "
LOG=/root/s2idle-$LEVEL.log

rc-service acpid stop >/dev/null 2>&1   # power key would run poweroff after resume
echo N > /sys/module/printk/parameters/console_suspend
case $FLAGS in *" quiet "*) echo 0 ;; *) echo 1 ;; esac > /sys/power/pm_print_times
echo 1 > /sys/power/pm_debug_messages
echo "$LEVEL" > /sys/power/pm_test || exit 1
case $FLAGS in *" nowd "*) echo V > /dev/watchdog && echo "hardware watchdog stopped" ;; esac
case $FLAGS in *" wfi "*)
	for d in /sys/devices/system/cpu/cpu*/cpuidle/state1/disable; do echo 1 > "$d"; done ;;
esac
mount -t debugfs none /sys/kernel/debug 2>/dev/null
grep -l enabled /sys/kernel/irq/*/wakeup 2>/dev/null | while read -r f; do
	d=${f%/wakeup}; echo "wake irq ${d##*/}: $(cat "$d/actions" 2>/dev/null)"
done > "$LOG"
PROBE=:
case $FLAGS in *" probe "*)
	PROBE='wdt() { P=/usr/local/bin/peek; echo "$1 $(cut -d" " -f1 /proc/uptime) $($P 0x10170000) $($P 0x10170004) $($P 0x10170008)" > /dev/pmsg0; }
	i=0; while [ $i -lt 30 ]; do wdt after$i; i=$((i+1)); sleep 0.2; done'
	P=/usr/local/bin/peek
	echo "before $(cut -d" " -f1 /proc/uptime) $($P 0x10170000) $($P 0x10170004) $($P 0x10170008)" > /dev/pmsg0 ;;
esac
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +20 > /sys/class/rtc/rtc0/wakealarm

setsid sh -c "
	sleep 2
	echo freeze > /sys/power/state; echo \"state write: \$?\" >> $LOG
	$PROBE
	{ echo; echo wakeup_irq: \$(cat /sys/power/pm_wakeup_irq 2>/dev/null)
	  cat /sys/power/suspend_stats/* 2>/dev/null | head -20
	  echo; cat /sys/kernel/debug/wakeup_sources; echo; dmesg; } >> $LOG 2>&1
	echo none > /sys/power/pm_test
	for d in /sys/devices/system/cpu/cpu*/cpuidle/state1/disable; do echo 0 > \"\$d\"; done
	rc-service acpid start >/dev/null 2>&1
	sync
" </dev/null >/dev/null 2>&1 &
echo "suspending in 2 s (pm_test=$LEVEL, flags:$FLAGS), log: $LOG"
