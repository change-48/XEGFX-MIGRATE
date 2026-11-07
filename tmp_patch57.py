import sys
p = sys.argv[1]
s = open(p).read()

if "fBootDescAt" in s:
    raise SystemExit("ALREADY PATCHED")

# 1) add the timestamp next to the cache flag
a1 = """    struct NVRMVramRequest fBootDesc = {};
    bool fBootDescOK = false;"""
if a1 not in s:
    raise SystemExit("ANCHOR 1 NOT FOUND")
s = s.replace(a1, """    struct NVRMVramRequest fBootDesc = {};
    bool fBootDescOK = false;
    uint64_t fBootDescAt = 0;		/* mach time of the last re-ask, see below */""", 1)

# 2) serve the cache only for a second, then re-ask
a2 = """        if (fBootDescOK) {
            r->kva = fBootDesc.kva; r->phys = fBootDesc.phys; r->width = fBootDesc.width; r->height = fBootDesc.height;
            r->pitch = fBootDesc.pitch; r->actualSize = fBootDesc.actualSize; r->size = fBootDesc.size;
            return kIOReturnSuccess;
        }"""
if a2 not in s:
    raise SystemExit("ANCHOR 2 NOT FOUND")
s = s.replace(a2, """        // A-57: the firmware raster stopped being the only answer this can give.  A mode change that keeps
        // the panel timing but hands the compositor a smaller backing (1560x1040 under a 3120x2080 raster)
        // leaves this description stale, and the copy path then fails the "source is not the scanout's
        // size" guard and the panel freezes on the last frame.  Re-ask once a second; in between the answer
        // is still served from here, which is what keeps the per-frame IONDRV call -- and the N-258 panic
        // path that came with it -- out of the hot path.
        uint64_t nowNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
        if (fBootDescOK && (nowNs - fBootDescAt) < 1000000000ull) {
            r->kva = fBootDesc.kva; r->phys = fBootDesc.phys; r->width = fBootDesc.width; r->height = fBootDesc.height;
            r->pitch = fBootDesc.pitch; r->actualSize = fBootDesc.actualSize; r->size = fBootDesc.size;
            return kIOReturnSuccess;
        }""", 1)

# 3) stamp the moment we re-asked
a3 = "        fBootDescOK = r->kva && r->pitch && r->height;"
if a3 not in s:
    raise SystemExit("ANCHOR 3 NOT FOUND")
s = s.replace(a3, "        fBootDescOK = r->kva && r->pitch && r->height;\n        fBootDescAt = nowNs;", 1)

open(p, "w").write(s)
print("patched ok")
