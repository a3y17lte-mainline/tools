#!/bin/sh
# DSIM bring-up by hand, one named step per argument (display blanked, driver idle).
# Steps: phyon swrst pll phy fifo cfg(config without lanes) cfg0(lanes only, no config bits)
#        lanes(config + lanes) clk(esc/byte clocks, no HS request) hs(HS request) off
P=/usr/local/bin/peek; W=/usr/local/bin/poke; D=0x148000
st() { echo "$1: LINK $($P ${D}04 | cut -d' ' -f2) DPHY $($P ${D}08 | cut -d' ' -f2) INT $($P ${D}34 | cut -d' ' -f2) CLK $($P ${D}10 | cut -d' ' -f2) CFG $($P ${D}30 | cut -d' ' -f2)"; }
w() { $W $1 $2 >/dev/null; }
for s in "$@"; do
	case $s in
	phyon) w 0x148f100c 1; w 0x1048070c 1 ;;
	swrst) w ${D}0c 1 ;;
	pll) w ${D}a0 0x13880; w ${D}98 0x40000000; w ${D}94 0x00806389; usleep 5000 ;;
	phy) w ${D}a4 0x177; w ${D}b4 0x306; w ${D}b8 0x02100a04; w ${D}bc 0x00030706 ;;
	fifo) w ${D}4c 0x1540; usleep 10000; w ${D}4c 0x155f; usleep 10000 ;;
	cfg) w ${D}30 0xc0007000 ;;
	cfg0) w ${D}30 0x0000007f ;;
	lanes) w ${D}30 0xc000707f ;;
	clk) w ${D}10 0x00031f04 ;;
	hs) w ${D}10 0x00131f04 ;;
	wait) usleep 1000 ;;
	off) w ${D}30 0; w ${D}10 0xff; w ${D}94 0; w ${D}0c 1; w 0x1048070c 0; w 0x148f100c 0; continue ;;
	esac
	st $s
done
