#!/bin/bash
D=$HOME/xegfx-log/A-13
C=/usr/local/bin/xenative-ctl
mkdir -p $D
dumpk() {
  echo "--$1--"
  sudo $C dump 0x60000 8
  sudo $C dump 0x6001c 1
  sudo $C dump 0x60400 1
  sudo $C dump 0x68170 2
  sudo $C dump 0x68180 2
  sudo $C dump 0x68190 1
  sudo $C dump 0x70008 1
  sudo $C dump 0x70024 1
  sudo $C dump 0x70240 4
  sudo $C dump 0x70268 1
  sudo $C dump 0x7027c 1
  sudo $C dump 0x70180 8
}
{ date "+%H:%M:%S"; echo "== baseline =="; dumpk baseline; } | tee $D/b1-baseline.txt
echo "== mapfast =="; sudo $C mapfast 0x1000 6338 0x01000000 | tee $D/b2-mapfast.txt
echo "== solid white 3120 =="; sudo $C apcolor 0x1000 0xFFFFFFFF 1 | tee $D/b3-white.txt
echo "== surf only (1080 geometry) =="; sudo $C reg surf 0x01000000 | tee $D/b4-surf1080.txt
sleep 2
{ date "+%H:%M:%S"; echo "== white @1080 geometry =="; dumpk white1080; } | tee $D/b5-white1080.txt
echo "== regset 3120 =="; date "+%H:%M:%S.%N"
sudo $C regset 3 2 195 1 0x081F0C2F 3 0x0C2F081F | tee $D/b6-regset.txt
date "+%H:%M:%S.%N"
{ date "+%H:%M:%S"; echo "== white @3120 geometry (t+0) =="; dumpk white3120_t0; } | tee $D/b7-white3120-t0.txt
sleep 5
{ date "+%H:%M:%S"; echo "== white @3120 geometry (t+5s) =="; dumpk white3120_t5; } | tee $D/b8-white3120-t5.txt
sudo $C status | tee $D/b9-status.txt
