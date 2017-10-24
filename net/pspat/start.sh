#!/bin/sh

sudo sysctl net.pspat.debug_xmit=0
sudo sysctl net.pspat.xmit_mode=0
sudo sysctl net.pspat.tc_bypass=0
sudo sysctl net.pspat.single_txq=1
sudo sysctl net.pspat.enable=1
