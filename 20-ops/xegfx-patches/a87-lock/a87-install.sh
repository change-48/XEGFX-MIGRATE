#!/bin/bash
# A-87: install the lock-experiment build into the WindowServer's bundle, atomically.
# The running WindowServer keeps the old inode (rename, not overwrite), so nothing breaks before the reboot.
set -eu
SRC=${1:?usage: a87-install.sh <NVMTLDriver>}
DST=/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
L=$HOME/xegfx-log/A-87
mkdir -p "$L/backup"

if [ ! -f "$L/backup/NVMTLDriver.installed" ]; then
  sudo cp -p "$DST" "$L/backup/NVMTLDriver.installed"
  echo "backup: $(shasum -a256 "$L/backup/NVMTLDriver.installed" | cut -c1-16)"
fi
echo "running now: $(shasum -a256 "$DST" | cut -c1-16)  $(stat -f %z "$DST") B"
echo "new build:   $(shasum -a256 "$SRC" | cut -c1-16)  $(stat -f %z "$SRC") B"

# A build that lost its own features must never be installed.
for s in "page-off" "nvmtl-ws-prof" "library key memo" "G-SUB" "A87 out-of-lock" "A87 switch"; do
  if ! strings -a "$SRC" | grep -q -- "$s"; then echo "MISSING $s -- not installing"; exit 1; fi
done
echo "feature strings: ok"

T=/tmp/a87-NVMTLDriver.$$
cp -f "$SRC" "$T"
codesign -f -s - "$T"
sudo /usr/bin/python3 /usr/local/bin/patch-csflags.py "$T"
codesign -dvv "$T" 2>&1 | grep -E "^flags|Signature=adhoc|Identifier" | head -3
sudo install -o root -g wheel -m 755 "$T" "$DST.new"
sudo mv -f "$DST.new" "$DST"
rm -f "$T"
echo "installed:   $(shasum -a256 "$DST" | cut -c1-16)  $(stat -f %z "$DST") B"
echo "REBOOT to load it; the switch file /private/tmp/nvmtl-a87 defaults to absent = A-80 behaviour"
