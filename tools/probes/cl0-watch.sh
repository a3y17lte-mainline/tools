#!/bin/sh
# cl0-watch.sh SECONDS [domain]: from cpu4 (cluster 1), log every change of cluster 0's PMU power state
# and the CPUCL0 watchdog registers, with /proc/uptime timestamps, to stdout and /dev/pmsg0
# (survives a reset: /sys/fs/pstore/pmsg-ramoops-0). Read-only (peek). See work/notes/suspend.md.
# "domain": leave CPU0 STATUS out of the change test (it flips on every idle entry).
#   0x10482404 CPUCL0 NONCPU_STATUS   0x10482604 CPUCL0 L2_STATUS   0x10482484 CPUSEQ_STATUS
#   0x10482004 CPU0 STATUS            0x10170000/08 WTCON/WTCNT
P=/usr/local/bin/peek
end=$(( $(cut -d. -f1 /proc/uptime) + ${1:-60} ))
prev=""
while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
	cur="nc=$($P 0x10482404 | cut -d' ' -f2) l2=$($P 0x10482604 | cut -d' ' -f2)"
	cur="$cur seq=$($P 0x10482484 | cut -d' ' -f2)"
	[ "${2:-}" = domain ] || cur="$cur c0=$($P 0x10482004 | cut -d' ' -f2)"
	cur="$cur wtcon=$($P 0x10170000 | cut -d' ' -f2)"
	if [ "$cur" != "$prev" ]; then
		line="$(cut -d' ' -f1 /proc/uptime) $cur wtcnt=$($P 0x10170008 | cut -d' ' -f2)"
		echo "$line"; echo "cl0w $line" > /dev/pmsg0
		prev=$cur
	fi
done
echo done
