#!/bin/sh
# Print a system report on the phone's screen (/dev/tty0). BusyBox-only.
{
clear
cat <<'BANNER'
     _    _____    ____   ___  _ _____
    / \  |___ /   |___ \ / _ \/ |___  |   Galaxy A3 (2017)
   / _ \   |_ \     __) | | | | |  / /    Exynos 7870
  / ___ \ ___) |   / __/| |_| | | / /     mainline Linux
 /_/   \_\____/   |_____|\___/|_|/_/
BANNER
echo
echo "kernel   $(uname -r) ($(uname -m))"
echo "built    $(uname -v | cut -c1-60)"
echo "model    $(cat /proc/device-tree/model)"
echo "compat   $(tr '\0' ' ' < /proc/device-tree/compatible)"
echo "cpus     $(grep -c ^processor /proc/cpuinfo) x Cortex-A53 (part $(grep -m1 'CPU part' /proc/cpuinfo | awk '{print $4}'))"
echo "memory   $(awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} END{printf "%d MiB total, %d MiB free", t/1024, a/1024}' /proc/meminfo)"
echo "uptime   $(cut -d' ' -f1 /proc/uptime) s, load $(cut -d' ' -f1-3 /proc/loadavg)"
echo "drivers  $(find /sys/bus/platform/drivers -mindepth 2 -maxdepth 2 -type l -name "*.*" | wc -l) platform devices bound to $(ls /sys/bus/platform/drivers | wc -l) drivers"
echo "display  $(cat /sys/class/graphics/fb0/name 2>/dev/null) $(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
echo "gpu      $(ls -d /sys/bus/platform/drivers/panfrost/*.gpu 2>/dev/null | sed 's#.*/##') (panfrost, Mali-T830)"
echo "usb      gadget on $(cat /sys/kernel/config/usb_gadget/g1/UDC), net $(ip -4 -o addr show usb0 | awk '{print $4}')"
echo
echo "input devices:"
grep '^N: Name' /proc/bus/input/devices | sed 's/N: Name="/  /; s/"$//'
echo
mb=$(( $(cat /sys/block/mmcblk0/size) / 2048 ))
echo "eMMC: $(cat /sys/block/mmcblk0/device/name) $mb MiB, $(ls -d /sys/block/mmcblk0/mmcblk0p* | wc -l) partitions"
i=0
for p in $(ls -d /sys/block/mmcblk0/mmcblk0p* | sort -t p -k2 -n); do
	n=$(grep PARTNAME "$p/uevent" | cut -d= -f2)
	s=$(( $(cat "$p/size") / 2048 ))
	printf "  %-4s %-10s %6s MiB" "${p##*mmcblk0}" "$n" "$s"
	i=$((i + 1))
	[ $((i % 2)) -eq 0 ] && echo
done
echo
echo
echo "cmdline  $(cat /proc/cmdline)"
} > /dev/tty0 2>&1
