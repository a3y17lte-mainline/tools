#!/bin/sh
# Charging across s2idle (phone on a charger, Alpine, root). Usage: s2idle-charge.sh [CYCLES]
# Suspends CYCLES times (default 4) in a row. The S2MU004 charger driver's watchdog alarm
# should wake the system every ~30 s while a charger is attached; the RTC alarm at +300 s is
# only a fallback. Logs wake IRQ, charger status/health and battery current per cycle to
# /root/s2idle-charge.log. Runs detached (USB drops during suspend).
N=${1:-4}
LOG=/root/s2idle-charge.log
rc-service acpid stop >/dev/null 2>&1
PS=/sys/class/power_supply
st() {
	echo "$1 up=$(cut -d' ' -f1 /proc/uptime) wake=$(cat /sys/power/pm_wakeup_irq 2>/dev/null)" \
	     "chg=$(cat $PS/s2mu004-charger/status)/$(cat $PS/s2mu004-charger/health)" \
	     "bat_ua=$(cat $PS/s2mu005-fuel-gauge/current_now 2>/dev/null)"
}
st start > $LOG
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +300 > /sys/class/rtc/rtc0/wakealarm
setsid sh -c "
	sleep 2
	i=1; while [ \$i -le $N ]; do
		echo freeze > /sys/power/state
		echo \"cycle\$i up=\$(cut -d' ' -f1 /proc/uptime) wake=\$(cat /sys/power/pm_wakeup_irq 2>/dev/null) chg=\$(cat $PS/s2mu004-charger/status)/\$(cat $PS/s2mu004-charger/health) bat_ua=\$(cat $PS/s2mu005-fuel-gauge/current_now 2>/dev/null)\" >> $LOG
		sleep 3
		i=\$((i+1))
	done
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	dmesg | grep -iE 's2mu004|watchdog suspended|charger' | tail -5 >> $LOG
	rc-service acpid start >/dev/null 2>&1
" </dev/null >/dev/null 2>&1 &
echo "running $N cycles, log: $LOG"
