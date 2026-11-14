#!/bin/sh
# One display test after a DISPAUD power cycle without system sleep (debug kernel with the
# exynos_pmu dbg_abort/dbg_pd_enable and samsung_dsim dbg_* switches; deep-sleep.md section 8).
# Without arguments on a kernel without the switches: plain first-enable test.
# Usage: ds-reduce.sh A B C D [E]  (E = HS clock request at the first transfer; 1/0: init before panel power, timings at init,
#                                standby at first transfer, lanes before clocks)
# Prints PASS/FAIL from the registers: frame counter running, TE present, DSIM INTSRC.
P=/usr/local/bin/peek
M=/sys/module/samsung_dsim/parameters
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 0 > /sys/class/vtconsole/vtcon1/bind
echo 1 > /sys/class/graphics/fb0/blank
[ -n "$TRACE" ] && echo $TRACE > $M/dbg_trace
[ -n "$DELAY" ] && echo $DELAY > $M/dbg_delay_us
if [ -f $M/dbg_hs_late ]; then	# only the reduction build (#146/#147) has these
	echo $1 > $M/dbg_init_pre_enable; echo $2 > $M/dbg_mode_at_init
	echo $3 > $M/dbg_standby_xfer; echo $4 > $M/dbg_lanes_first
	echo ${5:-1} > $M/dbg_hs_late
fi
echo 1 > /sys/module/exynos_pmu/parameters/dbg_abort
echo 1 > /sys/module/exynos_pmu/parameters/dbg_pd_enable
dmesg -c > /dev/null
sh /root/s2idle.sh none mem quiet > /dev/null
sleep 11
if ! dmesg | grep -q "PM: sleep aborted" || [ "$($P 0x14800010 | cut -d' ' -f2)" != 0x000000ff ]; then
	echo "INVALID: DISPAUD was not power cycled"; dmesg | grep "PM:" | tail -3; exit 2
fi
dmesg -c > /dev/null
(echo 0 > /sys/class/graphics/fb0/blank) > /dev/null 2>&1 &
sleep 6
a=$(($($P 0x148307fc | cut -d' ' -f2))); sleep 2; b=$(($($P 0x148307fc | cut -d' ' -f2)))
n=0; hi=0; while [ $n -lt 300 ]; do case $($P 0x139b01a4) in *4|*5|*6|*7|*c|*d|*e|*f) hi=$((hi+1));; esac; n=$((n+1)); done
int=$($P 0x14800034 | cut -d' ' -f2)
err=$(dmesg | grep -c -E "timed out|failed")
r=FAIL; [ $((b-a)) -gt 100 ] && [ $hi -gt 0 ] && [ $err -eq 0 ] && r=PASS
echo "$r A=$1 B=$2 C=$3 D=$4 E=${5:-1} trace=$(cat $M/dbg_trace 2>/dev/null) delay=$(cat $M/dbg_delay_us 2>/dev/null): frames +$((b-a))/2s TE $hi/300 INTSRC $int errors $err"
dmesg | grep -E "DBG (lanes on|hs request)" | sed 's/.*DBG /  /' | cut -c1-70
echo 1 > /sys/class/graphics/fb0/blank
