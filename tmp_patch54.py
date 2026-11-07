import sys
p = sys.argv[1]
s = open(p).read()

# ---- globals next to the flip slots
g_anchor = "static unsigned long long gXeFlipGgtt[8];"
if g_anchor not in s:
    raise SystemExit("GLOBALS ANCHOR NOT FOUND")
s = s.replace(g_anchor, g_anchor + '''
// What each flip slot currently maps.  Rewriting 6370 PTEs and flushing the GPU TLB every frame cost
// about 20 ms at 3120x2080 -- more than the CPU copy this path replaced -- and CoreAnimation only cycles
// a few surfaces, so remember the mapping and reuse it while it still describes the same pages.
static uint32_t gXeSlotSid[8];		/* surface id each slot maps (0 = none) */
static unsigned int gXeSlotPages[8];	/* page count that mapping covers */
static unsigned long long gXeSlotFirst[8];	/* its first page, so a recycled id cannot be mistaken for it */
static SInt64 gXeMapHits, gXeMapMisses;''', 1)

# ---- replace the walk + map with a cache-aware version
old = '''    if (slot == gXeFlipLive) { OSAddAtomic64(1, &gCntXeFlipMiss); return (false); }
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    if (!md || md->getLength() < bytes) { OSAddAtomic64(1, &gCntXeFlipFail); return (false); }
    if (md->prepare(kIODirectionNone) != kIOReturnSuccess) { OSAddAtomic64(1, &gCntXeFlipFail); return (false); }
    static unsigned long long phys[NV_XE_FLIP_MAX_PAGES];
    const unsigned int need = (unsigned int)((bytes + 4095ull) >> 12);
    unsigned int npages = 0;
    IOByteCount off = 0;
    while (off < (IOByteCount)bytes && npages < need) {
        IOByteCount segLen = 0;
        addr64_t pa = md->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
        if (!pa || !segLen) break;
        IOByteCount remain = (IOByteCount)bytes - off;
        for (IOByteCount q = 0; q < segLen && npages < need && q < remain; q += 4096)
            phys[npages++] = ((unsigned long long)pa + q) & ~0xfffull;
        off += segLen;
    }
    const IOReturn mapRc = (npages < need) ? kIOReturnNoSpace :
        nvAccelResidentCall("xgFlipMap", (void *)(uintptr_t)slot, phys, (void *)(uintptr_t)npages,
            (void *)(uintptr_t)bytes);
    md->complete(kIODirectionNone);	/* pages are in the GGTT now; do not keep the descriptor */
    md = nullptr;
    if (mapRc != kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntXeFlipFail);
        return (false);
    }'''
if old not in s:
    raise SystemExit("MAP BLOCK ANCHOR NOT FOUND")

new = '''    if (slot == gXeFlipLive) { OSAddAtomic64(1, &gCntXeFlipMiss); return (false); }
    // A-54: a slot that already maps this very surface needs no work -- its PTEs still point at the same
    // pages, and the surface is retained while we are about to arm it.  Verified by the first page, so an
    // id that got recycled for a different surface cannot be mistaken for the old one.
    const unsigned int need = (unsigned int)((bytes + 4095ull) >> 12);
    const uint32_t sid = (uint32_t)_ZNK9IOSurface12getSurfaceIDEv((void *)s);
    IOReturn mapRc = kIOReturnSuccess;
    bool mappedAlready = false;
    if (sid && gXeSlotSid[slot] == sid && gXeSlotPages[slot] == need) {
        IOMemoryDescriptor *vmd = s->getMemoryDescriptor();
        if (vmd && vmd->prepare(kIODirectionNone) == kIOReturnSuccess) {
            IOByteCount vlen = 0;
            addr64_t vpa = vmd->getPhysicalSegment(0, &vlen, kIOMemoryMapperNone);
            mappedAlready = vpa && (((unsigned long long)vpa & ~0xfffull) == gXeSlotFirst[slot]);
            vmd->complete(kIODirectionNone);
        }
        if (mappedAlready) {
            const SInt64 hits = OSAddAtomic64(1, &gXeMapHits);
            if (hits % 300 == 0)
                ALOG("iop: flip mapping reused %lld times, %lld rebuilt -- slot %u surface %u",
                     (long long)hits, (long long)gXeMapMisses, slot, sid);
        } else {
            gXeSlotSid[slot] = 0;
        }
    } else if (gXeSlotSid[slot]) {
        gXeSlotSid[slot] = 0;	/* a different surface is queued for this slot */
    }
    if (!mappedAlready) {
        OSAddAtomic64(1, &gXeMapMisses);
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!md || md->getLength() < bytes) { OSAddAtomic64(1, &gCntXeFlipFail); return (false); }
        if (md->prepare(kIODirectionNone) != kIOReturnSuccess) { OSAddAtomic64(1, &gCntXeFlipFail); return (false); }
        static unsigned long long phys[NV_XE_FLIP_MAX_PAGES];
        unsigned int npages = 0;
        IOByteCount off = 0;
        while (off < (IOByteCount)bytes && npages < need) {
            IOByteCount segLen = 0;
            addr64_t pa = md->getPhysicalSegment(off, &segLen, kIOMemoryMapperNone);
            if (!pa || !segLen) break;
            IOByteCount remain = (IOByteCount)bytes - off;
            for (IOByteCount q = 0; q < segLen && npages < need && q < remain; q += 4096)
                phys[npages++] = ((unsigned long long)pa + q) & ~0xfffull;
            off += segLen;
        }
        mapRc = (npages < need) ? kIOReturnNoSpace :
            nvAccelResidentCall("xgFlipMap", (void *)(uintptr_t)slot, phys, (void *)(uintptr_t)npages,
                (void *)(uintptr_t)bytes);
        if (mapRc == kIOReturnSuccess) {
            IOByteCount slen = 0;
            addr64_t sp0 = md->getPhysicalSegment(0, &slen, kIOMemoryMapperNone);
            gXeSlotSid[slot] = sid; gXeSlotPages[slot] = need;
            gXeSlotFirst[slot] = sp0 ? ((unsigned long long)sp0 & ~0xfffull) : 0;
        }
        md->complete(kIODirectionNone);	/* pages are in the GGTT now; do not keep the descriptor */
        md = nullptr;
        if (mapRc != kIOReturnSuccess) {
            OSAddAtomic64(1, &gCntXeFlipFail);
            return (false);
        }
    }'''
s = s.replace(old, new, 1)
open(p, "w").write(s)
print("patched ok")
