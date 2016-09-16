#!/bin/sh

varname="xmit_ok"
if [ -n "$1" ]; then
	varname="$1"
fi

last="0"
while true; do
	cur=$(sysctl net.pspat.${varname} | cut -d ' ' -f 3)
	delta=$(($cur - $last))
	echo "$delta per second"
	last=$cur
	sleep 1
done
