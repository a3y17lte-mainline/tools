#!/bin/sh
# ssp-cap.sh: 60 s on-phone capture of the sensor hub: proximity events (iio-events in
# /usr/local/bin) and the magnetometer + accelerometer buffers into /tmp/cap/. Copy to the phone
# (ssh a3 "cat > /tmp/cap.sh" < ssp-cap.sh) and run it with phone-sh "sh /tmp/cap.sh" 80: the
# default phone-sh timeout (20 s) would cut it short. Samples are 3 x s16, 2 pad, s64 time.
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
sleep 60
kill $(jobs -p) 2>/dev/null
echo 0 > $P/events/in_proximity_thresh_either_en
for D in $M $A; do echo 0 > $D/buffer/enable; done
wc -c /tmp/cap/*
