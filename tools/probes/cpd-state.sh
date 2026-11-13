#!/bin/sh
# cpd-state.sh [N]: sample cluster 1 power state N times (default 500) with peek (read-only)
# and print how often each value was seen, plus the cpuidle cluster-state counters.
# Run on the phone: CL1 NONCPU_STATUS 0x10482424 (0x...f = on), L2_STATUS 0x10482624
# (7 = on), CPUSEQUENCER_OPTION 0x104824a8 (bit 0 = power the cluster down after its
# last core). See work/notes/cpuidle.md.
P=/usr/local/bin/peek
n=${1:-500}
i=0
while [ $i -lt $n ]; do
	$P 0x10482424; $P 0x10482624; $P 0x104824a8
	i=$((i + 1))
done | sort | uniq -c
for c in 4 5 6 7; do
	d=/sys/devices/system/cpu/cpu$c/cpuidle
	[ -d $d/state2 ] || { echo "cpu$c: no state2"; continue; }
	echo "cpu$c state1 usage=$(cat $d/state1/usage) rej=$(cat $d/state1/rejected)" \
	     "state2 $(cat $d/state2/name) usage=$(cat $d/state2/usage)" \
	     "rej=$(cat $d/state2/rejected) time=$(cat $d/state2/time)"
done
