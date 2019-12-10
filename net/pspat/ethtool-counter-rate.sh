#!/bin/sh

ifname="enp1s0f1"
if [ -n "$1" ]; then
	ifname="$1"
fi

last="0"
while true; do
	cur=$(ethtool -S ${ifname} | grep " tx_packets" | cut -d ':' -f 2)
	delta=$(($cur - $last))
	echo "$delta per second"
	last=$cur
	sleep 1
done
