#!/bin/bash
# A-13 canonical procedure. Order matters: geometry BEFORE surf, then latch.
D=$HOME/xegfx-log/A-13
C=/usr/local/bin/xenative-ctl
mkdir -p $D
sudo $C status > $D/f0-before.txt
sudo $C mapfast 0x1000 6338 0x01000000 | tee $D/f1-mapfast.txt
sudo $C apfill  0x1000 1              | tee $D/f2-apfill.txt
sudo $C regset  3 2 195 1 0x081F0C2F 3 0x0C2F081F | tee $D/f3-regset.txt
sudo $C reg surf 0x01000000          | tee $D/f4-surf.txt
sudo $C latch                        | tee $D/f5-latch.txt
sleep 1
{ sudo $C status; sudo $C dump 0x68184 1; sudo $C dump 0x68190 1; } | tee $D/f6-status.txt
