#!/bin/sh
# otg-test.sh: one supervised OTG boost test on the phone (needs a kernel with the
# test-only "regulator-output" node otg-test). Logs to /root/otg-test.log.
# 1. waits until the charger input goes offline (PC cable unplugged),
# 2. waits 20 s for the USB-C hub to be plugged in,
# 3. if no external VBUS appeared, turns the boost on for 30 s, logging every 0.5 s,
#    and turns it off at once if the PD chip reports a power source (sink attach).
LOG=/root/otg-test.log
STATE=/sys/devices/platform/otg-test/state
CHG=/sys/class/power_supply/s2mu004-charger
FG=/sys/class/power_supply/s2mu005-fuel-gauge
log() { echo "$(cut -d' ' -f1 /proc/uptime) $*" >> $LOG; }
r() { i2cget -f -y "$1" "$2" "$3" 2>/dev/null; }
snap() {
	log "CTRL0=$(r 2 0x3d 0x10) STS0=$(r 2 0x3d 0x0a) STS1=$(r 2 0x3d 0x0b)" \
	    "pd18=$(r 1 0x3c 0x18) pdb3=$(r 1 0x3c 0xb3) pdb4=$(r 1 0x3c 0xb4)" \
	    "fg_uA=$(cat $FG/current_now) vbat=$(cat $FG/voltage_now) online=$(cat $CHG/online)"
}
off() { echo disabled > $STATE; log "OTG off ($1)"; snap; }

log "start, waiting for the input to go offline"
while [ "$(cat $CHG/online)" = 1 ]; do sleep 1; done
log "input offline, waiting 20 s for the hub"
sleep 20
snap
if [ "$(cat $CHG/online)" = 1 ]; then log "external VBUS present: OTG test skipped"; exit 0; fi

log "OTG on"
echo enabled > $STATE
n=0
while [ $n -lt 60 ]; do
	sleep 0.5
	snap
	b4=$(r 1 0x3c 0xb4)
	# PLUG_MON2 bit2 = attached as sink: a power source is connected
	if [ -n "$b4" ] && [ $((b4 & 0x04)) -ne 0 ]; then off "source detected"; exit 0; fi
	n=$((n + 1))
done
off "done"
