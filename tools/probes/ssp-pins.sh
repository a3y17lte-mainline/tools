#!/bin/sh
# ssp-pins.sh: read-only state of the sensor hub pins on the Galaxy A3 2017 (runs on the phone,
# needs peek in PATH). Reads the GPIO bank registers (CON 4 bits/pin: 0 input, 1 output, 2-e
# function, f EINT; DAT; PUD 4 bits/pin: 0 none, 1 down, 3 up). Writes nothing.
PEEK=${PEEK:-peek}

reg() { $PEEK "$1" | sed -n 's/.*: 0x\([0-9a-f]*\)$/\1/p'; }

pin() {	# name bank-base pin note
	con=$(reg $(printf 0x%x $(($2 + 0))))
	dat=$(reg $(printf 0x%x $(($2 + 4))))
	pud=$(reg $(printf 0x%x $(($2 + 8))))
	f=$(( (0x$con >> ($3 * 4)) & 0xf ))
	v=$(( (0x$dat >> $3) & 1 ))
	p=$(( (0x$pud >> ($3 * 4)) & 0xf ))
	case $f in 0) fn=input ;; 1) fn=output ;; 15) fn=eint ;; *) fn="func$f" ;; esac
	case $p in 0) pl=none ;; 1) pl=down ;; 3) pl=up ;; *) pl="pud$p" ;; esac
	printf '%-9s %-7s %-7s level %d  pull %-5s %s\n' "$1" "$4" "$fn" "$v" "$pl" "$5"
}

TOP=0x139b0000
ALIVE=0x139f0000
pin MCU_INT1 $((ALIVE + 0x40)) 7 gpa0-7 "hub -> AP data ready (idle high)"
pin MCU_INT2 $((TOP + 0x140)) 7 gpd3-7 "hub -> AP handshake ack"
pin AP_INT   $((TOP + 0x160)) 2 gpd4-2 "AP -> hub handshake (idle high)"
pin BOOT0    $((TOP + 0x160)) 3 gpd4-3 "must be low"
pin nRST     $((TOP + 0x120)) 3 gpd2-3 "low = MCU held in reset"
pin SPI_SCK  $((TOP + 0x060)) 2 gpc4-2 "func3 = SPI4"
pin SPI_CS   $((TOP + 0x060)) 3 gpc4-3 "func3 = SPI4 CS, output = GPIO CS"
pin SPI_MISO $((TOP + 0x080)) 0 gpc5-0 "func3 = SPI4"
pin SPI_MOSI $((TOP + 0x080)) 1 gpc5-1 "func3 = SPI4"
