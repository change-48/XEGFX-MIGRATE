import sys

p = sys.argv[1]
s = open(p).read()

# ---------------------------------------------------------------- stage 1 block
anchor = "static bool nvAccelXeFlipArm(IOSurface *s, unsigned w, unsigned h, unsigned pitch, bool waitIdle)"
if anchor not in s:
    raise SystemExit("ANCHOR nvAccelXeFlipArm NOT FOUND")

block = r'''// ---- A-53 stage 1: a scanout of our own, laid out at the compositor's row pitch --------------------
// CoreAnimation aligns IOSurface rows to 256 bytes, so a 3120-wide panel asks for 12544 while the firmware
// console scans 12480 (PLANE_STRIDE_1_A = 0xc3).  The deferred flip in performTransaction() only arms when
// q.pitch == P, so on this panel it never runs and every frame is copied by the CPU.  The boot aperture
// cannot simply be widened (25,958,400 B; 12544 x 2080 needs 26,091,520), but XeGfx already reserves GGTT
// slots of exactly the surface size -- take one for a scanout that is 12544 wide and the equality holds
// without touching that check.  Stage 1 allocates and maps only: no plane register is written here.
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
static unsigned gScanoutState;		/* 0 idle, 1 mapped, 3 failed */

static bool nvScanoutAlloc(void)
{
    if (gScanout.live) return true;
    const unsigned pitch = 12544u, h = 2080u;
    const unsigned long long bytes = (unsigned long long)pitch * h;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionOutIn | kIOMemoryKernelUserShared, (vm_size_t)bytes, 4096);
    if (!md) { ALOG("scanout: %llu B allocation failed", bytes); gScanoutState = 3; return false; }
    unsigned long long ggtt[8] = { 0 }; unsigned n = 0;
    if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)1, ggtt, &n)
            != kIOReturnSuccess || n == 0) {
        ALOG("scanout: xgFlipInit refused (%u slots)", n); md->release(); gScanoutState = 3; return false;
    }
    ALOG("scanout: xgFlipInit -> %u slot(s), first 0x%llx (asked %llu B)", n, ggtt[0], bytes);
    if (gXeFlipSlots && bytes > gXeFlipBytes) {
        ALOG("scanout: reserved slot is %llu B, need %llu -- refusing", gXeFlipBytes, bytes);
        md->release(); gScanoutState = 3; return false;
    }
    const unsigned int need = (unsigned int)((bytes + 4095ull) >> 12);
    if (need > NV_XE_FLIP_MAX_PAGES) { md->release(); gScanoutState = 3; return false; }
    static unsigned long long sphys[NV_XE_FLIP_MAX_PAGES];
    unsigned int npages = 0; IOByteCount off = 0;
    while (off < (IOByteCount)bytes && npages < need) {
        IOByteCount segLen = 0;
        addr64_t pa = md->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
        if (!pa || !segLen) break;
        IOByteCount remain = (IOByteCount)bytes - off;
        for (IOByteCount q = 0; q < segLen && npages < need && q < remain; q += 4096)
            sphys[npages++] = ((unsigned long long)pa + q) & ~0xfffull;
        off += segLen;
    }
    IOReturn rc = (npages < need) ? kIOReturnNoSpace :
        nvAccelResidentCall("xgFlipMap", (void *)(uintptr_t)0, sphys, (void *)(uintptr_t)npages,
                            (void *)(uintptr_t)bytes);
    if (rc != kIOReturnSuccess) {
        ALOG("scanout: xgFlipMap(slot 0) refused (%u/%u pages, 0x%x)", npages, need, (unsigned)rc);
        md->release(); gScanoutState = 3; return false;
    }
    gScanout.md = md;
    gScanout.kva = md->getBytesNoCopy();
    gScanout.phys = (IOPhysicalAddress)md->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    gScanout.bytes = bytes; gScanout.pitch = pitch; gScanout.ggtt = (unsigned)ggtt[0];
    gScanout.live = true; gScanoutState = 1;
    ALOG("scanout: %llu B kva %p phys 0x%llx ggtt 0x%x pitch %u (%u pages, core slot 0)",
         bytes, gScanout.kva, (unsigned long long)gScanout.phys, gScanout.ggtt, pitch, npages);
    return true;
}

'''
s = s.replace(anchor, block + anchor, 1)

# ---------------------------------------------------------------- call it once, after bootArm arms
call_anchor = '''    ALOG("boot-armed: 6/6 debug.xegfxaccel_iop_xe_flip=1 + debug.xegfxaccel_iop_xe_defer=1 (wait_idle 1, idle_ms 40, "'''
if call_anchor not in s:
    raise SystemExit("ANCHOR boot-armed 6/6 NOT FOUND")
# place the call right after that multi-line ALOG statement ends; simplest: before the next statement marker
after = '''         "report_busy 1)");
'''
if after not in s:
    raise SystemExit("ANCHOR after-6/6 NOT FOUND")
s = s.replace(after, after + '''
    // A-53 stage 1: take a 12544-wide scanout out of the GGTT slots (allocate + map only; the plane is
    // untouched here).  If it cannot be had, the copy path stays exactly as it is.
    nvScanoutAlloc();
''', 1)

open(p, "w").write(s)
print("patched ok")
