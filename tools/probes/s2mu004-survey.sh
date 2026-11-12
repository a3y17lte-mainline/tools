#!/bin/sh
# s2mu004-survey.sh: read-only S2MU004 register survey (usb-c.md section 7, charging.md section 6).
# Snapshots the PD chip (0x3c), MUIC + charger (0x3d) and the PD IRQ pin every 10 s and appends
# to /root/survey.log only when something changed, so cable swaps that drop the USB link are
# still recorded. Reads only; never touches the clear-on-read INT registers
# (0x3c 0xE0-0xE6, 0x3d 0x00 0x01 0x04 0x06 0x07).
#   s2mu004-survey.sh [PD_BUS] [PMIC_BUS]     (Alpine: 1 and 2)
PD=${1:-1}
PM=${2:-2}
LOG=/root/survey.log

rd() { # bus addr regs...
	b=$1; a=$2; shift 2
	for r in "$@"; do
		printf ' %02x=%s' $((r)) "$(i2cget -f -y "$b" "$a" "$r" 2>/dev/null | sed 's/^0x//')"
	done
}
seqhex() { i=$(($1)); while [ $i -le $(($2)) ]; do echo $i; i=$((i + 1)); done; }

snap() {
	echo "pd:$(rd $PD 0x3c 0x01 0x02 0x18 0x19 0x1e 0x1f 0x26 0x27 0x28 0x2e 0x2f \
		$(seqhex 0x3e 0x44) 0xb2 0xb3 0xb4 0xf7 0xf8)"
	echo "muic:$(rd $PM 0x3d 0x08 0x09 $(seqhex 0x61 0x6a) $(seqhex 0xc7 0xcd) 0xd4 0xd5)"
	echo "chg:$(rd $PM 0x3d $(seqhex 0x0a 0x0f) $(seqhex 0x10 0x24))"
	echo "gpa0dat: $(/usr/local/bin/peek 0x139f0044)"
	grep -h "ONLINE\|STATUS\|USB_TYPE" /sys/class/power_supply/*/uevent | tr '\n' ' '
	echo
}

prev=
while :; do
	cur=$(snap)
	if [ "$cur" != "$prev" ]; then
		{ echo "== $(date +%T) up $(cut -d' ' -f1 /proc/uptime)"; echo "$cur"; } >> $LOG
		prev=$cur
	fi
	sleep 10
done
