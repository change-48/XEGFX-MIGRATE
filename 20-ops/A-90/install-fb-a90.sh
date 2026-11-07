#!/bin/bash
# A-90 A1 install: XeGfxIntelFB with the debug.xegfxfb_notify sysctl.  Follows the project's §3.3 order:
# backup binary + KC + sha, verify the right tree, install, codesign, rebuild aux KC, re-check the six ids.
set -u
L=$HOME/xegfx-log/A-90/backup
mkdir -p "$L"
KE=/Library/Extensions/XeGfxIntelFB.kext
BIN=$KE/Contents/MacOS/XeGfxIntelFB
NEW=$HOME/src/NvGfx-build/kexts/XeGfxIntelFB/XeGfxIntelFB.kext/Contents/MacOS/XeGfxIntelFB
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc

echo "== tree check (must be the XeGfxIntelFB tree)"
echo "new      : $(shasum -a 256 "$NEW" | cut -c1-12)  $(stat -f %z "$NEW") B"
echo "installed: $(shasum -a 256 "$BIN" | cut -c1-12)  $(stat -f %z "$BIN") B"
n=$(strings -a "$NEW" | grep -c "A-90 notify"); echo "A-90 notify in new binary: $n"
[ "$n" -ge 1 ] || { echo "STOP: new binary does not carry the A-90 sysctl"; exit 1; }

echo "== backup"
cp -p "$BIN" "$L/XeGfxIntelFB.before-a90" 2>/dev/null || sudo cp -p "$BIN" "$L/XeGfxIntelFB.before-a90"
[ -f "$L/aux-before-A90.kc" ] || sudo cp -p "$KC" "$L/aux-before-A90.kc"
shasum -a 256 "$BIN" "$KC" | tee "$L/sha.before-a90"

echo "== install"
sudo cp -f "$NEW" "$BIN"
sudo chown root:wheel "$BIN"
sudo codesign -f -s - "$KE" >/dev/null 2>&1
codesign -dv "$KE" 2>&1 | grep Signature

echo "== aux KC"
sudo /usr/bin/kmutil create -n aux --volume-root / --allow-missing-kdk \
     -B /System/Library/KernelCollections/BootKernelExtensions.kc \
     -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
     --repository /Library/Extensions -A /tmp/aux-a90.kc -z || { echo "STOP: kmutil create failed"; exit 2; }
sudo cp -f /tmp/aux-a90.kc "$KC"

cnt=$(sudo /usr/bin/kmutil inspect -A "$KC" -M --no-header | awk 'NF{print $1}' | sort | grep -c watchthelight)
echo "watchthelight ids in KC: $cnt (need >= 6)"
echo "IGPUConsoleRange present: $(sudo /usr/bin/kmutil inspect -A "$KC" -M --no-header | grep -c IGPUConsoleRange)"
[ "$cnt" -ge 6 ] || { echo "STOP: KC looks wrong, restoring"; sudo cp -f "$L/aux-before-A90.kc" "$KC"; exit 3; }
shasum -a 256 "$BIN" "$KC" | tee "$L/sha.after-a90"
echo "OK: installed, reboot pending"
