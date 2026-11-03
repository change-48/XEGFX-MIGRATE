#!/bin/bash
# A-89 P1: install the accel that records sub + vcnt per frame, rebuild the AuxKC, leave the reboot to the operator.
set -uo pipefail
L=$HOME/xegfx-log/A-89
NEW=$L/build-accel/NVAccel.kext/Contents/MacOS/NVAccel
DST=/Library/Extensions/XeGfxAccel.kext/Contents/MacOS/XeGfxAccel
mkdir -p "$L/backup"

echo "== 1. before =="
[ -f "$NEW" ] || { echo "no build"; exit 1; }
echo "new:       $(stat -f %z "$NEW") B  $(md5 -q "$NEW")"
echo "installed: $(stat -f %z "$DST") B  $(md5 -q "$DST")"
strings -a "$NEW" > "$L/build-accel/strings.txt"
for s in xegfxaccel_iop_tl_raw xegfxaccel_iop_tl xegfxaccel_iop_xe_flip; do
  grep -q -- "$s" "$L/build-accel/strings.txt" || { echo "MISSING $s -- not installing"; exit 1; }
done
echo "new sysctl strings: ok"

if [ ! -f "$L/backup/XeGfxAccel.a88.before" ]; then
  sudo cp -p "$DST" "$L/backup/XeGfxAccel.a88.before"
  sudo cp -p /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$L/backup/aux-before-A89.kc"
  echo "backup: $(md5 -q "$L/backup/XeGfxAccel.a88.before")  $(ls -l "$L/backup/aux-before-A89.kc" | awk '{print $5}') B"
fi

echo "== 2. install =="
sudo cp -f "$NEW" "$DST"
sudo chown root:wheel "$DST"; sudo chmod 755 "$DST"
sudo codesign -f -s - /Library/Extensions/XeGfxAccel.kext
codesign -dv /Library/Extensions/XeGfxAccel.kext 2>&1 | grep -E "Signature|flags"
echo "installed now: $(md5 -q "$DST")"

echo "== 3. AuxKC =="
sudo /usr/bin/kmutil create -n aux --volume-root / --allow-missing-kdk \
     -B /System/Library/KernelCollections/BootKernelExtensions.kc \
     -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
     --repository /Library/Extensions -A /tmp/aux-a89.kc -z || { echo "kmutil create FAILED"; exit 1; }
sudo cp -f /tmp/aux-a89.kc /Library/KernelCollections/AuxiliaryKernelExtensions.kc
echo -n "watchthelight ids in AuxKC: "
sudo /usr/bin/kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc -M --no-header | awk 'NF{print $1}' | sort | grep -c watchthelight
echo -n "IGPUConsoleRange still there (must be >=1): "
sudo /usr/bin/kmutil inspect -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc -M --no-header | grep -c IGPUConsoleRange
echo "REBOOT to load it; observation only, A-88 switches untouched"
