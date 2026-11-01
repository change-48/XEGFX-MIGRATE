#!/bin/bash
echo "=== fb node tree ==="
ioreg -w0 -l -c XeGfxIntelKMSFramebuffer -d 2 2>/dev/null | head -20
echo "=== fb keys ==="
ioreg -w0 -l -c XeGfxIntelKMSFramebuffer 2>/dev/null | grep -oE '"[A-Za-z0-9_.-]{3,40}" =' | sort -u | head -50
echo "=== IOFramebuffer instances ==="
ioreg -w0 -c IOFramebuffer 2>/dev/null | grep -E '^ *\| *\+-o ' | head -10
echo "=== IODisplay instances ==="
ioreg -w0 -c IODisplay 2>/dev/null | grep -cE 'class IODisplay'
echo "=== WS sees displays ==="
sudo launchctl asuser 501 sudo -u change48 /usr/bin/python3 -c "
import Quartz
ids, n = Quartz.CGGetActiveDisplayList(16, None, None)
print('active displays', ids, n)
for i in ids:
    print(i, Quartz.CGDisplayPixelsWide(i), Quartz.CGDisplayPixelsHigh(i))
" 2>&1 | tail -8
