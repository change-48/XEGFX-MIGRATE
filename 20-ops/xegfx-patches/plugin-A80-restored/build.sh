#!/bin/bash
# Build NVMTLDriver from this tree. This is the command A-80 actually used (A-80 §5.4):
# build_plugin.sh points at ~/nvmtl-build/LIVE (gone), and ~/Vulkan-Headers does not exist;
# the working include path is mesa's. -framework IOKit is required (without it the link fails).
set -eu
cd "$(dirname "$0")"
OUT=${1:-./NVMTLDriver}
xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 \
  -I"$HOME/src/xe-userland/mesa/include" NVMTLDevice.m \
  -framework Foundation -framework Metal -framework IOSurface -framework IOKit \
  -o "$OUT" 2> "$OUT.err"
echo "built $OUT  $(stat -f %z "$OUT") B  $(shasum -a256 "$OUT" | cut -c1-16)  uuid $(otool -l "$OUT" | awk '/uuid/{print $2; exit}')"
echo "warnings $(grep -c ': warning:' "$OUT.err")  errors $(grep -c ': error:' "$OUT.err")"
