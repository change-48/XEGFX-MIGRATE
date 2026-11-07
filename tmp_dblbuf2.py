import sys
p = sys.argv[1]
s = open(p).read()

def rep(old, new, label):
    global s
    if old not in s:
        raise SystemExit("ANCHOR MISSING: " + label)
    s = s.replace(old, new, 1)

# 1) the second buffer, and the state that says which one the panel is on
anchor = "IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)"
rep(anchor, '''

#ifdef NMX_XEGFX
// A-68: the copy used to write into the very buffer the panel was scanning.  A 26 MB copy measures about
// 8 ms and a 120 Hz frame is 8.3 ms, so every frame was written while it was being scanned -- that is the
// tear line, and the reason a dragged window seems to trail the pointer.  A second buffer of the same
// geometry lets the copy land off-screen; one PLANE_SURF write then hands the finished frame over, and the
// panel picks it up at the next vblank.
struct NvBackBuf { IOBufferMemoryDescriptor *md; void *kva; unsigned long long bytes; unsigned ggtt; bool live; };
static NvBackBuf gBack = {};
static bool  gFrontIsFb = true;		/* the panel is scanning the firmware buffer */
static unsigned gFbSurf;		/* its GGTT offset, captured once */

static bool nvBackAlloc(unsigned long long bytes)
{
    if (gBack.live) return true;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionOutIn | kIOMemoryKernelUserShared, (vm_size_t)bytes, 4096);
    if (!md) { ALOG("back buffer: %llu B allocation failed", bytes); return false; }
    unsigned long long ggtt[8] = { 0 }; unsigned n = 0;
    if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)1, ggtt, &n)
            != kIOReturnSuccess || n == 0) {
        ALOG("back buffer: no GGTT slot for %llu B", bytes); md->release(); return false; }
    static unsigned long long bp[NV_XE_FLIP_MAX_PAGES];
    const unsigned need = (unsigned)((bytes + 4095ull) >> 12);
    if (need > NV_XE_FLIP_MAX_PAGES) { md->release(); return false; }
    unsigned np = 0; IOByteCount off = 0;
    while (off < (IOByteCount)bytes && np < need) {
        IOByteCount segLen = 0;
        addr64_t pa = md->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
        if (!pa || !segLen) break;
        IOByteCount rem = (IOByteCount)bytes - off;
        for (IOByteCount q = 0; q < segLen && np < need && q < rem; q += 4096)
            bp[np++] = ((unsigned long long)pa + q) & ~0xfffull;
        off += segLen;
    }
    if (np < need || nvAccelResidentCall("xgFlipMap", (void *)(uintptr_t)0, bp, (void *)(uintptr_t)np,
            (void *)(uintptr_t)bytes) != kIOReturnSuccess) {
        ALOG("back buffer: map refused (%u/%u pages)", np, need); md->release(); return false; }
    gBack.md = md; gBack.kva = md->getBytesNoCopy(); gBack.bytes = bytes;
    gBack.ggtt = (unsigned)ggtt[0]; gBack.live = true;
    ALOG("back buffer: %llu B at ggtt 0x%x (%u pages) -- copies land off-screen", bytes, gBack.ggtt, np);
    return true;
}
#endif

''' + anchor, "globals")

# 2) the pre-copy wait only makes sense without a back buffer
old = """    const uint64_t copyStart = mach_absolute_time();
    if (gIopMemcpy) {
        if (q.pitch == pitch) {
            memcpy((void *)q.kva, (const void *)src, (size_t)h * (size_t)pitch);
        } else {
            for (unsigned y = 0; y < h; y++)
                memcpy((char *)q.kva + (unsigned long)y * q.pitch,
                       (const char *)src + (unsigned long)y * pitch, (size_t)words * 4);
        }
    } else {
        volatile unsigned int *d = (volatile unsigned int *)q.kva;"""
new = """    const uint64_t copyStart = mach_absolute_time();
#ifdef NMX_XEGFX
    // Copy into the buffer the panel is NOT scanning; the handover waits for a vblank at the end.
    if (!gFbSurf && gDispMmio) gFbSurf = gDispMmio[NV_PLANE_SURF_A / 4];
    const unsigned long long frameBytes = (unsigned long long)q.pitch * (unsigned long long)h;
    if (gDispMmio && !gBack.live && q.kva && q.pitch && h) nvBackAlloc(frameBytes);
    const bool toBack = (gBack.live && frameBytes <= gBack.bytes) ? gFrontIsFb : false;
    void *const dstKva = toBack ? gBack.kva : (void *)q.kva;
    const unsigned dstGgtt = (gBack.live && frameBytes <= gBack.bytes) ? (toBack ? gBack.ggtt : gFbSurf) : 0;
#else
    void *const dstKva = (void *)q.kva;
#endif
    if (gIopMemcpy) {
        if (q.pitch == pitch) {
            memcpy(dstKva, (const void *)src, (size_t)h * (size_t)pitch);
        } else {
            for (unsigned y = 0; y < h; y++)
                memcpy((char *)dstKva + (unsigned long)y * q.pitch,
                       (const char *)src + (unsigned long)y * pitch, (size_t)words * 4);
        }
    } else {
        volatile unsigned int *d = (volatile unsigned int *)dstKva;"""
rep(old, new, "copy target")

# 3) hand the finished frame over
old2 = """    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));"""
new2 = """#ifdef NMX_XEGFX
    if (gDispMmio && dstGgtt) {
        // The switch has to land while the scan is at the top of the frame; writing PLANE_SURF at an
        // arbitrary moment makes the panel swap buffers mid-scan, which tears worse than the single
        // buffer ever did.  The copy above already happened off-screen, so waiting here costs nothing.
        nvAccelIopWaitVblank();
        gDispMmio[NV_PLANE_SURF_A / 4] = dstGgtt;
        (void)gDispMmio[NV_PLANE_SURF_A / 4];
        gFrontIsFb = !toBack;
    }
#endif
    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));"""
rep(old2, new2, "handover")

open(p, "w").write(s)
print("patched")
