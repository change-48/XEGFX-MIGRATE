#!/bin/bash
T=$HOME/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8
echo "=== 含 XeGfxIntelFB 的源文件 ==="
grep -rl 'XeGfxIntelFB' "$T/kexts" 2>/dev/null | grep -E '\.(cpp|h|plist|mk|make)$' | head -12
echo
echo "=== deliverFramebufferNotification 的使用 ==="
grep -rn 'deliverFramebufferNotification' "$T/kexts" 2>/dev/null | head -10
echo
echo "=== 装机的 FB kext ==="
ls -la /Library/Extensions/XeGfxIntelFB.kext/Contents/MacOS/
echo
echo "=== FB kext 自带 sysctl（debug.xegfx 系） ==="
sysctl -a 2>/dev/null | grep -E '^debug\.xegfxfb' | head -20
