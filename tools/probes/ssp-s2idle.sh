#!/bin/sh
# Sensor hub across s2idle: accel + mag buffers and prox events on, one s2idle (RTC +20 s),
# then 10 s more capture. Log: /root/ssp-s2idle.log, data sizes before/after.
LOG=/root/ssp-s2idle.log
dev() { for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name)" = "$1" ] && echo $d; done; }
P=$(dev ssp-proximity); M=$(dev ssp-magnetometer); A=$(dev ssp-accelerometer)
rm -rf /tmp/cap; mkdir -p /tmp/cap
for D in $M $A; do
	for f in $D/scan_elements/*_en; do echo 1 > $f; done
	echo 512 > $D/buffer/length; echo 1 > $D/buffer/enable
	cat /dev/${D##*/} > /tmp/cap/$(cat $D/name).bin &
done
echo 1 > $P/events/in_proximity_thresh_either_en
/usr/local/bin/iio-events /dev/${P##*/} > /tmp/cap/prox-events.txt &
ssp() { grep SSP_Int /proc/interrupts | awk '{s=0; for(i=2;i<=9;i++) s+=$i; print s}'; }
sleep 5
{ echo "pre: ssp_int=$(ssp) $(wc -c /tmp/cap/*.bin | tr '\n' ' ')"; } > $LOG
rc-service acpid stop >/dev/null 2>&1
echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +20 > /sys/class/rtc/rtc0/wakealarm
s0=$(ssp); t0=$(cut -d' ' -f1 /proc/uptime)
echo freeze > /sys/power/state
s1=$(ssp); t1=$(cut -d' ' -f1 /proc/uptime)
echo "suspend: wake_irq=$(cat /sys/power/pm_wakeup_irq) ssp_int $s0 -> $s1, up $t0 -> $t1" >> $LOG
echo "post0: $(wc -c /tmp/cap/*.bin | tr '\n' ' ')" >> $LOG
sleep 10
echo "post10: ssp_int=$(ssp) $(wc -c /tmp/cap/*.bin | tr '\n' ' ')" >> $LOG
kill $(jobs -p) 2>/dev/null
echo 0 > $P/events/in_proximity_thresh_either_en
for D in $M $A; do echo 0 > $D/buffer/enable; done
dmesg | grep -iE "ssp|sensorhub" | tail -8 >> $LOG
rc-service acpid start >/dev/null 2>&1
echo done >> $LOG
