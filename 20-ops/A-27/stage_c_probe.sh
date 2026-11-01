#!/bin/bash
L=~/xegfx-log/A-27
mkdir -p "$L"
PID=$(pgrep -x WindowServer)
echo "WS pid $PID"
sudo sample "$PID" 5 -file "$L/ws-s3.txt" >/dev/null 2>&1
echo "SW=$(grep -cE 'CA::OGL::SW|scanline|draw_polygon' "$L/ws-s3.txt")"
echo "MTL=$(grep -cE 'CA::MTL|MTLCommandBuffer|IOGPU|NVMTL|MetalContext' "$L/ws-s3.txt")"
echo "--- accel stats ---"
ioreg -r -c IOAccelerator -l -w0 2>/dev/null | grep -oE '"(context2DCount|bufferSwapCount|bufferFlipCount|gartUsedBytes)" = [0-9]+' | head -8
echo "--- fb props ---"
ioreg -l -w0 -c XeGfxIntelKMSFramebuffer 2>/dev/null | grep -oE '"XeGfxFb[A-Za-z]*" = [A-Za-z0-9]+' | sort -u
echo "--- vblank/count ---"
sudo log show --last 2m --predicate 'process == "kernel" AND eventMessage CONTAINS "XEGFX_DISPLAY"' 2>/dev/null | grep -oE "count=[0-9]+|vbl src=[0-9]+|presented=[0-9]+" | tail -6
echo "--- displays ---"
system_profiler SPDisplaysDataType 2>/dev/null | grep -E "Resolution|Online|Main"
