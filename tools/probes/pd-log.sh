#!/bin/sh
# pd-log.sh: log charger, fuel gauge, Type-C/TCPM, PD chip and battery temperature every 2 s
# to /root/pd-test.log (for cable swaps without the USB link). Reads only.
LOG=/root/pd-test.log
C=/sys/class/power_supply/s2mu004-charger
F=/sys/class/power_supply/s2mu005-fuel-gauge
T=/sys/class/power_supply/tcpm-source-psy-1-003c
while :; do
	echo "$(cut -d' ' -f1 /proc/uptime) on=$(cat $C/online) type=$(cat $C/usb_type | grep -o '\[[A-Za-z_]*\]')" \
	     "in=$(cat $C/input_current_limit) chg=$(cat $C/constant_charge_current) fg=$(cat $F/current_now)" \
	     "tcpm_on=$(cat $T/online) tcpm_max=$(cat $T/current_max) tcpm_v=$(cat $T/voltage_now)" \
	     "partner=$(ls /sys/class/typec | tr '\n' ,) pr=$(cat /sys/class/typec/port0/power_role) dr=$(cat /sys/class/typec/port0/data_role) usb=$(ls /sys/bus/usb/devices 2>/dev/null | tr "\n" ,) otg=$(i2cget -f -y 2 0x3d 0x10) pd=$(for r in 0x01 0x18 0x27 0x28 0xb3 0xb4; do i2cget -f -y 1 0x3c $r; done | tr "\n" ,)" "bt=$(cat /sys/class/thermal/thermal_zone1/temp)" >> $LOG
	sleep 2
done
