#!/bin/sh

# Utility script to print the number of transmitted packets per second using
# ethtool.

# default network interface
ifname="enp1s0f1"

# check if a different net interface was provided
if [ -n "$1" ]; then
	ifname="$1"
fi

# calculate e print the number of packets transmitted per second
last="0"
while true; do
	cur=$(ethtool -S ${ifname} | grep " tx_packets" | cut -d ':' -f 2)
	delta=$(($cur - $last))
	echo "$delta per second"
	last=$cur
	sleep 1
done

