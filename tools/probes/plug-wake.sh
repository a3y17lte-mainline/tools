#!/bin/sh
# USB plug wake test: 15 s to unplug the cable, then s2idle on battery (RTC fallback +120 s).
# Plug the cable back in ~10 s after the screen goes off: expect wake IRQ 68 (S2MU004).
# Then 20 s to press the touch keys (IRQ 84 count before/after). Log: /root/plug-wake.log
LOG=/root/plug-wake.log
irq() { grep " $1:" /proc/interrupts | awk '{s=0; for(i=2;i<=9;i++) s+=$i; print s}'; }
PS=/sys/class/power_supply
rc-service acpid stop >/dev/null 2>&1
echo "start up=$(cut -d' ' -f1 /proc/uptime) online=$(cat $PS/s2mu004-charger/online)" > $LOG
sleep 15
echo "pre up=$(cut -d' ' -f1 /proc/uptime) online=$(cat $PS/s2mu004-charger/online) status=$(cat $PS/s2mu004-charger/status)" >> $LOG
echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +120 > /sys/class/rtc/rtc0/wakealarm
echo freeze > /sys/power/state
echo "wake up=$(cut -d' ' -f1 /proc/uptime) wake_irq=$(cat /sys/power/pm_wakeup_irq) online=$(cat $PS/s2mu004-charger/online)" >> $LOG
echo 0 > /sys/class/rtc/rtc0/wakealarm
k0=$(irq 84); sleep 20; k1=$(irq 84)
echo "touchkey irq $k0 -> $k1, status=$(cat $PS/s2mu004-charger/status) up=$(cut -d' ' -f1 /proc/uptime)" >> $LOG
rc-service acpid start >/dev/null 2>&1
echo done >> $LOG
