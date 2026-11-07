import sys
p = sys.argv[1]
s = open(p).read()

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit("ANCHOR MISSING: " + label)
    s = s.replace(old, new, 1)

# 1) drop the too-early call (it ran before the raster was measured, so xifbScale1xOffered() was false)
rep('        // the panel is already scanning a 3120x2080 raster here; take the plane to the smaller picture and\n'
    '        // let the scaler fill the panel, so the answer above and what the hardware shows agree from boot\n'
    '        if (xifbScale1xOffered()) xifbApplyScale1x(fScale1x);\n',
    '',
    "drop early call")

# 2) apply once the base class has built the mode list and the raster is measured
rep('        const bool started = IONDRVFramebuffer::start(provider);\n'
    '        if (started) xifbPublishDetailedTimings();      // start() rewrites IOFBConfig, so publish again',
    '        const bool started = IONDRVFramebuffer::start(provider);\n'
    '        if (started) xifbPublishDetailedTimings();      // start() rewrites IOFBConfig, so publish again\n'
    '        // A-58: only now does the mode list exist, and only now is the raster measured -- so this is the first\n'
    '        // moment the plane can follow the 1560x1040 backing this framebuffer reports.  Doing it earlier found\n'
    '        // xifbScale1xOffered() false and left the plane on the 3120x2080 raster while the compositor was told\n'
    '        // something else.\n'
    '        if (started && xifbScale1xOffered()) {\n'
    '            XIFBLOG("1x: base mode %#x answers %ux%u, taking the plane and the scaler", (unsigned)fBaseMode, XIFB_1X_W, XIFB_1X_H);\n'
    '            xifbApplyScale1x(fScale1x);\n'
    '        }',
    "apply after start")

# 3) the detailed timing has to describe the scaled picture too, or CoreDisplay keeps compositing at the raster
rep('        if (mode == XIFB_1X_MODE_ID) {\n'
    '            // same raster timing, but the picture that fills it is 1560x1040 -- kIOScalingInfoValid is what\n'
    '            // tells CoreDisplay the scaled size differs from the active one',
    '        if (mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) {\n'
    '            // same raster timing, but the picture that fills it is 1560x1040 -- kIOScalingInfoValid is what\n'
    '            // tells CoreDisplay the scaled size differs from the active one',
    "timing scaled")

open(p, "w").write(s)
print("patched v2")
