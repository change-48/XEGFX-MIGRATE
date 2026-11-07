import sys
p = sys.argv[1]
s = open(p).read()

old_decl = '''struct NVScanoutBuf {
    IOBufferMemoryDescriptor *md;
    void    *kva;
    IOPhysicalAddress phys;
    unsigned long long bytes;
    unsigned pitch;
    unsigned ggtt;
    bool     live;
};
static NVScanoutBuf gScanout;
static unsigned gScanoutState;		/* 0 idle, 1 mapped, 3 failed */
'''
if old_decl not in s:
    raise SystemExit("DECL BLOCK NOT FOUND")
s = s.replace(old_decl, "", 1)

anchor_top = '#include "IOAccelMemoryMap.h"'
if anchor_top not in s:
    raise SystemExit("TOP ANCHOR NOT FOUND")
decl = anchor_top + '''

#ifdef NMX_XEGFX
// A scanout owned by the accelerator, laid out at the compositor's row pitch (see nvScanoutAlloc).
struct NVScanoutBuf {
    IOBufferMemoryDescriptor *md;
    void    *kva;
    IOPhysicalAddress phys;
    unsigned long long bytes;
    unsigned pitch;
    unsigned ggtt;
    bool     live;
};
static NVScanoutBuf gScanout;
static unsigned gScanoutState;	/* 0 idle, 1 mapped (copies go here), 2 plane taken, 3 failed */
static bool nvScanoutAlloc(void);
#endif'''
s = s.replace(anchor_top, decl, 1)

anchor_desc = '''        if (0) { ALOG("describeBootFramebuffer: DISABLED by experiment -- no copy target"); return kIOReturnUnsupported; }'''
if anchor_desc not in s:
    raise SystemExit("DESC ANCHOR NOT FOUND")
s = s.replace(anchor_desc, anchor_desc + '''
        // A-53: once the 12544-wide scanout exists, describe it instead of the firmware raster.  Copies
        // then go there at 12544 stride, and q.pitch becomes equal to the compositor's P -- the one thing
        // the deferred flip asks for.
        if (gScanout.live) {
            r->kva = gScanout.kva; r->phys = gScanout.phys;
            r->width = 3120; r->height = 2080; r->pitch = gScanout.pitch;
            r->actualSize = r->size = gScanout.bytes;
            return kIOReturnSuccess;
        }''', 1)

anchor_copy = '''        return kIOReturnSuccess; // Keep the prior frame, without claiming a displayed copy.
    }
#endif
    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));'''
if anchor_copy not in s:
    raise SystemExit("COPY ANCHOR NOT FOUND")
s = s.replace(anchor_copy, '''        return kIOReturnSuccess; // Keep the prior frame, without claiming a displayed copy.
    }
#endif
    // A-53: the buffer the copies go to is not on the panel yet.  A complete frame is in it now, so hand
    // the plane over -- row pitch first, then the surface (the order the flip path uses too).
    if (gScanout.live && gScanoutState == 1 && (void *)q.kva == gScanout.kva) {
        gDispMmio[NV_PLANE_STRIDE_A / 4] = gScanout.pitch / 64u;
        gDispMmio[NV_PLANE_SURF_A / 4] = gScanout.ggtt;
        (void)gDispMmio[NV_PLANE_SURF_A / 4];
        gScanoutState = 2;
        ALOG("scanout: plane taken -- stride %u, surf 0x%x (after a full frame)", gScanout.pitch / 64u, gScanout.ggtt);
    }
    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));''', 1)

s = s.replace(
    'if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)1, ggtt, &n)',
    'if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)4, ggtt, &n)',
    1)

open(p, "w").write(s)
print("patched ok")
