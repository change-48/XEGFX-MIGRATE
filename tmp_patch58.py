import sys
p = sys.argv[1]
s = open(p).read()

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit("ANCHOR MISSING: " + label)
    s = s.replace(old, new, 1)

# 1) default to the scaler stage: replacing the mode only makes sense with the plane and scaler moved too
rep("    int  fScale1x = 1;                  // 0 off, 1 quarter desktop, 2 quarter desktop + pipe scaler",
    "    int  fScale1x = 2;                  // 0 off, 1 quarter desktop, 2 quarter desktop + pipe scaler",
    "default level")

# 2) a helper: is this mode the one that answers with the smaller backing?
rep("    bool xifbScale1xOffered() { return fScale1x > 0 && fW && fH && fRowBytes && fReg && xifbEnsureBaseMode(); }",
    "    bool xifbScale1xOffered() { return fScale1x > 0 && fW && fH && fRowBytes && fReg && xifbEnsureBaseMode(); }\n"
    "    // A-58: CoreDisplay kept only the base mode when the smaller backing was offered as an extra one, so\n"
    "    // the base mode itself is the one that answers 1560x1040 -- same raster, smaller picture, scaled up by\n"
    "    // the pipe.  Everything else still goes to the base class.\n"
    "    bool xifbIs1x(IODisplayModeID mode) { return mode && mode == fBaseMode && xifbScale1xOffered(); }",
    "helper")

# 3) do not add a mode any more
rep("""        if (!xifbScale1xOffered()) return n;
        static unsigned logged;
        if (logged++ < 3) XIFBLOG("getDisplayModeCount: base %u -> %u", (unsigned)n, (unsigned)(n + 1));
        return n + 1;""",
    """        static unsigned logged;
        if (logged++ < 3) XIFBLOG("getDisplayModeCount: %u (the base mode itself carries the 1x backing)", (unsigned)n);
        return n;""",
    "mode count")

rep("""        if (r == kIOReturnSuccess && allDisplayModes && xifbScale1xOffered()) {
            allDisplayModes[n] = XIFB_1X_MODE_ID;
            static unsigned logged;
            if (logged++ < 3) XIFBLOG("getDisplayModes: base %u + quarter mode %#x", (unsigned)n, (unsigned)XIFB_1X_MODE_ID);
        }""",
    """        (void)n;""",
    "mode list")

# 4) the base mode describes a 1560x1040 backing
rep("        if (mode == XIFB_1X_MODE_ID) { w = XIFB_1X_W; h = XIFB_1X_H; row = XIFB_1X_ROW; }",
    "        if (mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) { w = XIFB_1X_W; h = XIFB_1X_H; row = XIFB_1X_ROW; }",
    "pixel info")

# 5) and reports that size as its nominal one
rep("        if (mode == XIFB_1X_MODE_ID && xifbEnsureBaseMode()) {",
    "        if ((mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) && xifbEnsureBaseMode()) {",
    "mode info")

# 6) selecting it (or coming up on it) moves the plane and turns the scaler on
rep("""        if (displayMode == XIFB_1X_MODE_ID && xifbScale1xOffered()) {
            fCurrentMode = XIFB_1X_MODE_ID;
            xifbApplyScale1x(fScale1x);""",
    """        if ((displayMode == XIFB_1X_MODE_ID || xifbIs1x(displayMode)) && xifbScale1xOffered()) {
            fCurrentMode = displayMode;
            xifbApplyScale1x(fScale1x);""",
    "set mode")

# 7) apply at bring-up too: the compositor may never call setDisplayMode for the mode it finds current
rep('        XIFBLOG("scale1x: stage %d, mmio %s, -xeintelfb1xoff disables", fScale1x, fReg ? "mapped" : "unavailable");',
    '        XIFBLOG("scale1x: stage %d, mmio %s, -xeintelfb1xoff disables", fScale1x, fReg ? "mapped" : "unavailable");\n'
    '        // the panel is already scanning a 3120x2080 raster here; take the plane to the smaller picture and\n'
    '        // let the scaler fill the panel, so the answer above and what the hardware shows agree from boot\n'
    '        if (xifbScale1xOffered()) xifbApplyScale1x(fScale1x);',
    "apply at boot")

open(p, "w").write(s)
print("patched all")
