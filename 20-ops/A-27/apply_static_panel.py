#!/usr/bin/env python3
# A-27 Stage A: inject a highest-priority "static-panel" timing source into
# xg_timing_fallback() of gpu/xegfx/shim/src/xnu/xg_kext.cpp
import io, sys, os

P = os.path.expanduser("~/src/NvGfx-xe-2.6.2/gpu/xegfx/shim/src/xnu/xg_kext.cpp")
MARK = "A-27: this panel"

src = io.open(P, "r", encoding="utf-8").read()
if MARK in src:
    print("ALREADY_PATCHED")
    sys.exit(0)

SIG = "xg_timing_fallback(XeGfxDisplayNub *nub)\n{\n"
i = src.find(SIG)
if i < 0:
    print("SIG_NOT_FOUND"); sys.exit(2)
ins_at = i + len(SIG)

BLOCK = u"""\t/* A-27: this panel (3120x2080 @119.891 Hz, 900.045 MHz) cannot be expressed as an EDID DTD
\t * (the DTD pixel clock field tops out at 655.35 MHz), and the compiled-in gPanelEdidDtd is the
\t * NE160QDM-NZL 2560x1600 reference panel.  Publish the measured live raster directly. */
\tstatic const uint32_t kOurPanel[XG_NVK5_TIMING_WORDS] = {
\t\t0x4b350001u, 900045000u, 3120u, 3332u, 3336u, 3400u,
\t\t2080u, 2202u, 2203u, 2208u, 119891u, 1u, 0u
\t};
\t{
\t\tuint32_t w[XG_NVK5_TIMING_WORDS];
\t\tmemcpy(w, kOurPanel, sizeof(w));
\t\tgTimingEdid = 0;
\t\txg_timing_store(nub, w, "static-panel");
\t\tsnprintf(gTimingNote, sizeof(gTimingNote), "A-27 static panel 3120x2080@119.891");
\t\tIOLog("XeGfx: timing fallback: static-panel pclk %u Hz %ux%u ht %u vt %u hs %u..%u vs %u..%u refresh %u mHz\\n",
\t\t    w[1], w[2], w[6], w[5], w[9], w[3], w[4], w[7], w[8], w[10]);
\t\treturn;
\t}

#if 0\t/* A-27: original fallback kept for reference (edid-dtd / vbt-dtd / none) */
"""

END_MARK = u"\tIOLog(\"XeGfx: timing fallback: no panel timing (%s); adapter will have no modes\\n\", gTimingNote);\n}\n"
j = src.find(END_MARK, ins_at)
if j < 0:
    print("END_NOT_FOUND"); sys.exit(3)
end_at = j + len(END_MARK)

body = src[ins_at:end_at]
new = src[:ins_at] + BLOCK + body + "#endif\t/* A-27 */\n}\n" + src[end_at:]

io.open(P, "w", encoding="utf-8", newline="\n").write(new)
print("PATCHED ok, chars %d -> %d" % (len(src), len(new)))
