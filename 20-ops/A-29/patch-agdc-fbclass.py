#!/usr/bin/env python3
# A-29 patch: NVX_XEGFX branch AGDC_FB_CLASS -> XeGfxFramebuffer
import re, sys

p = "nvrm-agdc.cpp"
s = open(p, encoding="utf-8").read()

old_line = '#define AGDC_FB_CLASS     "IONDRVFramebuffer"'
new_block = """/* NMX_XEGFX: the KMS adapter (kexts/XeGfxDisplay) publishes XeGfxFramebuffer
 * (XeGfxIntelKMSFramebuffer : XeGfxFramebuffer : IOFramebuffer).  It is NOT an
 * IONDRVFramebuffer subclass, so the historical class never matched it: AGDC
 * could not come up in S3, IOPresentment had no AGDC ("agdc 0x000000000"),
 * useIOP was cleared and WindowServer fell back to the legacy surface, which
 * asserts in CA::OGL::Context::push_surface on the first flattened layer. */
#define AGDC_FB_CLASS     "XeGfxFramebuffer" """

if new_block.strip().splitlines()[-1] in s:
    print("ALREADY_PATCHED")
    sys.exit(0)

n = s.count(old_line)
if n != 1:
    print("UNEXPECTED_COUNT=%d" % n)
    sys.exit(1)

s = s.replace(old_line, new_block, 1)
open(p, "w", encoding="utf-8").write(s)
print("PATCHED")
