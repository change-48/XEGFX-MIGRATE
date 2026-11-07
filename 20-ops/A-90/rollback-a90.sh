#!/bin/bash
# A-90 rollback: put the pre-A-90 framebuffer binary, kernel collection and bring-up script back.
set -u
L=$HOME/xegfx-log/A-90/backup
KE=/Library/Extensions/XeGfxIntelFB.kext
BIN=$KE/Contents/MacOS/XeGfxIntelFB
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc

echo "== restore framebuffer binary"
sudo cp -f "$L/XeGfxIntelFB.before-a90" "$BIN"
sudo chown root:wheel "$BIN"
sudo codesign -f -s - "$KE" >/dev/null 2>&1
codesign -dv "$KE" 2>&1 | grep Signature
shasum -a 256 "$BIN"
echo "expected (pre-A-90): d4c74c03ac475e94df76ff3715abad6608446682150f4e5b879c46db575c8a62"

echo "== restore kernel collection (byte-identical to the pre-A-90 one)"
sudo cp -f "$L/aux-before-A90.kc" "$KC"
shasum -a 256 "$KC"
cnt=$(sudo /usr/bin/kmutil inspect -A "$KC" -M --no-header | awk 'NF{print $1}' | sort | grep -c watchthelight)
echo "watchthelight ids: $cnt"
echo "IGPUConsoleRange: $(sudo /usr/bin/kmutil inspect -A "$KC" -M --no-header | grep -c IGPUConsoleRange)"

echo "== restore bring-up script"
sudo cp -f "$L/xegfx-bringup.sh.before" /usr/local/bin/xegfx-bringup.sh
sudo chown root:wheel /usr/local/bin/xegfx-bringup.sh
sudo chmod 755 /usr/local/bin/xegfx-bringup.sh
shasum -a 256 /usr/local/bin/xegfx-bringup.sh
echo "expected (pre-A-90): 32f9db924b8062868bde8154c393ca6ecb0691ad3d9b615f1f947c5d6e6572fa"
grep -c "A-90" /usr/local/bin/xegfx-bringup.sh || true
echo "== source tree: keep the patched intelfb.cpp, the pre-A-90 copy is in $L/intelfb.cpp.before-a90"
