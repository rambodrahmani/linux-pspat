#!/bin/sh

sudo ethtool -G enp1s0f1 tx 256

sudo sysctl net.pspat.debug_xmit=0
sudo sysctl net.pspat.xmit_mode=0
sudo sysctl net.pspat.tc_bypass=0
sudo sysctl net.pspat.single_txq=0
sudo sysctl net.pspat.mailbox_entries=512
sudo sysctl net.pspat.mailbox_line_size=128
sudo sysctl net.pspat.arb_qdisc_batch=1000
sudo sysctl net.pspat.dispatch_batch=1000
sudo sysctl net.pspat.enable=1

sudo taskset -pc 7 $(pgrep pspat-arb)
sudo taskset -pc 3 $(pgrep pspat-snd)
