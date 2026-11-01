// XeNative - drive the Arrow Lake display pipe A at the panel's native 3120x2080.
//
// A-10 revision 3 (2026-10-31).
//
// Why this is no longer a boot-time one-shot:
//   v0.2/v0.3 proved that the GGTT page table is NOT at GTTMMADR(BAR2)+8 MiB on
//   this part; the entries there are all zero. i915 takes the "direct stolen
//   access" path on MTL (i915_utils.c / intel_ggtt.c:1152 ggtt_probe_common) and
//   maps the table straight from GEN6_GSMBASE (0x108100). Reading that table at
//   the live surface's index gives 0x800003 -- present+RW, i.e. a real PTE.
//
//   That also means the prompt's recovery step ("if the screen goes black, write
//   SURF back") needs a runtime path, which a boot-time one-shot cannot offer.
//   So the kext exposes an IOKit user client with four verbs: status, scan,
//   switch, restore. Nothing is written to a display register unless "switch" is
//   asked for, and "restore" puts the five registers back exactly as they were
//   when the kext started.
//
// Safety:
//   * PLANE_CTL (0x70180) is never written: 0x84000008 = XRGB8888, TILING 12:10
//     = 0 (linear). Only SURF/SIZE/STRIDE/PIPESRC/PS_CTRL.bit31 are touched.
//   * "switch" refuses to run unless /var/db/xegfx/armed exists.
//   * GGTT entries are written and read back before any display register is
//     touched; a mismatch aborts with nothing on screen changed.
//   * the buffer lives in stolen memory, physically contiguous, 16 MiB in, clear
//     of the GGTT table (which sits at GSMBASE, below DSMBASE on this machine)
//     and clear of the range macOS uses for its own surface.

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>

extern "C" {
static kern_return_t xenative_kmod_start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
static kern_return_t xenative_kmod_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(org.xenative.XeNative, "0.17", xenative_kmod_start, xenative_kmod_stop);

// BSD VFS entry point, used only to test for the arm file.
int vnode_lookup(const char *path, int flags, void **vpp, void *ctx);
int vnode_put(void *vp);
void *vfs_context_current(void);

// The SDK parses IOMemoryDescriptor.h without __LP64__ (its withPhysicalAddress
// takes a 32-bit address and mangles to ...Ejjj); the kernel exports ...Eyyj.
IOMemoryDescriptor * _ZN18IOMemoryDescriptor19withPhysicalAddressEyyj(
        UInt64 address, UInt64 length, UInt32 direction);
}

// ---- cache write-back barrier (A-11) ---------------------------------------
// The GGTT page table and the stolen frame buffer are plain DRAM. Linux maps
// them with ioremap() -- uncached -- precisely because the display engine has
// no cache of its own and must see every PTE store immediately
// (gt/intel_ggtt.c ggtt_probe_common(): "Using direct GSM access" -> ioremap()).
// Our macOS mapping comes from IOMemoryDescriptor::withPhysicalAddress(phys)
// with kIOMapInhibitCache, and for DRAM that option is reported to mean nothing
// (it only selects the cache attribute for real MMIO). If the mapping is plain
// write-back, an 8-byte PTE store sleeps in L1 forever: it reads back perfectly
// from this CPU and never reaches the memory the display engine fetches from.
// wbinvd writes back and invalidates every cache line on the calling CPU, which
// is exactly the discriminator A-11 needs.
static inline void xnWbinvd(void)
{
    __asm__ __volatile__("mfence\n\twbinvd\n\tmfence" ::: "memory");
}

// ---- display 14, pipe A / plane 1 (SKL+ universal plane) --------------------
#define TRANS_HTOTAL_A       0x60000
#define TRANS_PIPESRC_A      0x6001C
#define TRANS_FRMCOUNT_A     0x60040
#define TRANS_DDI_FUNC_CTL_A 0x60400
#define PIPE_CONF_A          0x70008
#define PS_WIN_SZ_A          0x68174
#define PS_CTRL_A            0x68180
#define PLANE_CTL_1_A        0x70180
#define PLANE_STRIDE_1_A     0x70188
#define PLANE_SIZE_1_A       0x70190
#define PLANE_SURF_1_A       0x7019C
#define PLANE_TILEOFF_1_A    0x701A4
#define PLANE_SURFLIVE_1_A   0x701AC

#define GFX_FLSH_CNTL        0x101008
#define GGC                  0x108040
#define DSMBASE              0x1080C0   // 64-bit
#define GSMBASE              0x108100   // 64-bit, i915_reg.h:4202

#define GTT_PTE_OFFSET_BAR2  0x800000ULL

#define NAT_W   3120
#define NAT_H   2080
#define NAT_BPP 4
#define NAT_ROW ((UInt32)(NAT_W * NAT_BPP))              // 12480
#define NAT_SIZE ((UInt64)NAT_W * NAT_H * NAT_BPP)       // 25958400

#define PHYS_MASK      0x000FFFFFFFFFF000ULL
#define PTE_FLAG_MASK  0xFFFULL
#define STOLEN_BIAS    0x01000000ULL
#define PREFERRED_OFF  0x01000000ULL
#define ARMED_PATH     "/var/db/xegfx/armed"
#define BDSM_MASK64    0xFFFFFFFFFFF00000ULL   // GEN11_BDSM_MASK = 63:20 (i915_reg.h:4205)
#define HIGH_BITS_MASK 0xFFF0000000000000ULL   // nothing above bit 51 exists in a PTE

// Stolen memory is not zeroed between boots, so the unused tail of the page
// table is full of leftovers. "Free" therefore means "not a structurally valid
// PTE" (not present, or reserved bits set, or an address outside 4 GiB) rather
// than "zero" - measured 2026-10-31: the table had 0 zero entries.
static inline bool pteLooksLive(UInt64 v)
{
    if (!(v & 0x1)) return false;
    if (v & HIGH_BITS_MASK) return false;
    UInt64 p = v & PHYS_MASK;
    return p >= 0x1000 && p < 0x100000000ULL;
}

// ---- A-12: kernel-allocated buffer, written through its own VA --------------
// A-11 proved the PTE path reaches DRAM but the temporary withPhysicalAddress
// mapping used for pixels does not. v0.9 therefore stops writing into stolen and
// instead allocates an ordinary 26 MB kernel buffer once, holds it forever, and
// paints it with plain 32-bit stores through its kernel VA.
#define BUF_IDX        0x20000ULL             // GGTT index -> SURF 0x20000000
#define BUF_SURF       0x20000000U
#define BUF_ALIGN      8192
#define P1080_W        1920
#define P1080_H        1080
#define P1080_ROW      ((UInt64)P1080_W * 4)  // 7680 = stride 120 * 64

enum {
    kXeStatus   = 0,
    kXeScan     = 1,
    kXeSwitch   = 2,
    kXeRestore  = 3,
    kXeFill     = 4,
    kXePte      = 5,   // arg0 = GGTT index, arg1 = entry value
    kXeReg      = 6,   // arg0 = 0 SURF / 1 SIZE / 2 STRIDE / 3 PIPESRC / 4 PS_CTRL, arg1 = value
    kXeSwitchRel= 7,   // same as switch but PTEs encoded GSM-relative
    kXeFillAt   = 8,   // arg0 = physical address to paint (0 = the live surface's phys)
    kXeFlush    = 9,   // wbinvd only, for isolating the barrier from the writes
    kXeBuf      = 10,  // arg0 = 0 -> 1920x1080 pattern + 2025 PTEs; 1 -> 3120x2080 + 6338 PTEs
    kXeWhere    = 11,  // report buffer phys / pages / pattern fingerprint
    kXePeek     = 12,  // arg0 = physical address, arg1 = dword count (page limited)
    kXeBufVia   = 13,  // same as buf, but paints through a held physical mapping
    kXeBufLow   = 14,  // allocate below a physical-address mask, then buf
    kXeMapFast  = 15,  // arg0 idx, arg1 count, arg2 phys -> bulk PTE write in one call
    kXeApFill   = 16,  // arg0 idx, arg1 mode(0=1080,1=3120) -> paint through the BAR2 aperture
    kXeRegSet   = 17,  // A-13: [n, which0, val0, which1, val1, ...] back-to-back, one sleep
    kXeRegRead  = 18,  // A-13b: arg0 = BAR0 offset, arg1 = count(<=16) -> raw dwords, read only
    kXeApSolid  = 19,  // A-13b: arg0 idx, arg1 colour, arg2 mode(0=1080,1=3120) -> solid fill via aperture
    kXeLatch    = 20,  // A-13c: force a double-buffered plane latch (SURF nudge + restore)
};

class XeNative : public IOService {
    OSDeclareDefaultStructors(XeNative)
public:
    virtual bool start(IOService *provider) override;
    virtual void free() override;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler) override;

    IOReturn doStatus(IOExternalMethodArguments *args);
    IOReturn doScan(IOExternalMethodArguments *args);
    IOReturn switchCore(IOExternalMethodArguments *args, bool relative);
    IOReturn doSwitch(IOExternalMethodArguments *args)
    { return switchCore(args, false); }
    IOReturn doSwitchRel(IOExternalMethodArguments *args)
    { return switchCore(args, true); }
    IOReturn doRestore(IOExternalMethodArguments *args);
    IOReturn doFill(IOExternalMethodArguments *args);
    IOReturn doFillAt(IOExternalMethodArguments *args);
    IOReturn doPte(IOExternalMethodArguments *args);
    IOReturn doReg(IOExternalMethodArguments *args);
    IOReturn doRegSet(IOExternalMethodArguments *args);
    IOReturn doRegRead(IOExternalMethodArguments *args);
    IOReturn doApSolid(IOExternalMethodArguments *args);
    IOReturn doLatch(IOExternalMethodArguments *args);
    IOReturn doFlush(IOExternalMethodArguments *args);
    IOReturn doBuf(IOExternalMethodArguments *args);
    IOReturn doWhere(IOExternalMethodArguments *args);
    IOReturn doPeek(IOExternalMethodArguments *args);
    IOReturn doBufVia(IOExternalMethodArguments *args);
    IOReturn doBufLow(IOExternalMethodArguments *args);
    IOReturn doMapFast(IOExternalMethodArguments *args);
    IOReturn doApFill(IOExternalMethodArguments *args);

private:
    void snap(const char *tag);
    bool buildPattern(UInt64 phys, bool writeback = true);
    bool ensureBuf(UInt64 physMask = 0);
    bool paintVA(volatile UInt32 *pix, UInt32 w, UInt32 h);
    bool paintBuf(UInt32 w, UInt32 h);
    UInt64 mapBufPtes(UInt32 w, UInt32 h, unsigned *outBad);
    UInt64 bufPages(UInt32 w, UInt32 h) const { return ((UInt64)w * h * 4 + 4095) / 4096; }
    UInt64 bufVerify(UInt64 idx, UInt64 pages, UInt64 *outFirst, UInt64 *outMid, UInt64 *outLast,
                     unsigned *outBad);
    UInt64 findFreeRun(UInt64 pages, UInt64 prefer, bool *preferredHit);
    void publish(const char *k, UInt64 v, unsigned int bits)
    { setProperty(k, (unsigned long long)v, bits); }

    IOMemoryMap *fBar0Map = nullptr;
    IOMemoryMap *fBar2Map = nullptr;
    IOMemoryMap *fGttMap = nullptr;
    volatile UInt32 *fReg = nullptr;
    volatile UInt64 *fGtt = nullptr;
    UInt64 fEntries = 0;

    UInt64 fDsm = 0;
    UInt64 fStolen = 0;
    UInt64 fGttPhys = 0;
    UInt64 fGttBytes = 0;

    UInt32 fSurfBefore = 0, fPipesrcBefore = 0, fSizeBefore = 0,
           fStrideBefore = 0, fPsCtrlBefore = 0, fCtlBefore = 0;

    UInt64 fPteFlags = 0x3;
    UInt64 fPhysBase = 0;
    UInt64 fChosenOff = 0;
    UInt64 fLivePte = 0;
    UInt64 fLiveRun = 0;
    bool   fPlanOk = false;
    bool   fSwitched = false;
    UInt32 fFlushes = 0;

    static IOBufferMemoryDescriptor *sBuf;   // held for the life of the kext
    UInt64 fBufPages = 0;
    UInt64 fBufPhys0 = 0;
    UInt64 fBufPhysLast = 0;
    UInt64 fBufPhysMax = 0;
    UInt32 fBufW = 0, fBufH = 0;
    UInt64 fBufSum = 0;
    UInt32 fBufPx0 = 0, fBufPxMid = 0, fBufPxLast = 0;
    UInt64 fBufIdx = 0, fBufMapped = 0, fBufMask = 0;
    IOMemoryMap *fBufMapPI = nullptr;
    unsigned fBufBad = 0;
};

OSDefineMetaClassAndStructors(XeNative, IOService)

IOBufferMemoryDescriptor *XeNative::sBuf = nullptr;

// ---------------------------------------------------------------- user client
class XeNativeUC : public IOUserClient {
    OSDeclareDefaultStructors(XeNativeUC)
public:
    virtual bool start(IOService *provider) override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference) override;
private:
    XeNative *fProvider = nullptr;
};

OSDefineMetaClassAndStructors(XeNativeUC, IOUserClient)

bool XeNativeUC::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    fProvider = OSDynamicCast(XeNative, provider);
    return fProvider != nullptr;
}

IOReturn XeNativeUC::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                    IOExternalMethodDispatch *dispatch, OSObject *target,
                                    void *reference)
{
    if (!fProvider) return kIOReturnNotAttached;
    switch (selector) {
    case kXeStatus:  return fProvider->doStatus(args);
    case kXeScan:    return fProvider->doScan(args);
    case kXeSwitch:    return fProvider->doSwitch(args);
    case kXeRestore:   return fProvider->doRestore(args);
    case kXeFill:      return fProvider->doFill(args);
    case kXePte:       return fProvider->doPte(args);
    case kXeReg:       return fProvider->doReg(args);
    case kXeSwitchRel: return fProvider->doSwitchRel(args);
    case kXeFillAt:    return fProvider->doFillAt(args);
    case kXeFlush:     return fProvider->doFlush(args);
    case kXeBuf:       return fProvider->doBuf(args);
    case kXeWhere:     return fProvider->doWhere(args);
    case kXePeek:      return fProvider->doPeek(args);
    case kXeBufVia:    return fProvider->doBufVia(args);
    case kXeBufLow:    return fProvider->doBufLow(args);
    case kXeMapFast:   return fProvider->doMapFast(args);
    case kXeApFill:    return fProvider->doApFill(args);
    case kXeRegSet:    return fProvider->doRegSet(args);
    case kXeRegRead:   return fProvider->doRegRead(args);
    case kXeApSolid:   return fProvider->doApSolid(args);
    case kXeLatch:     return fProvider->doLatch(args);
    default:         return kIOReturnBadArgument;
    }
}

IOReturn XeNative::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                 OSDictionary *properties, IOUserClient **handler)
{
    XeNativeUC *uc = new XeNativeUC;
    if (!uc) return kIOReturnNoMemory;
    if (!uc->initWithTask(owningTask, securityID, type, properties) ||
        !uc->attach(this) || !uc->start(this)) {
        uc->release();
        return kIOReturnError;
    }
    *handler = uc;
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------- helpers
static UInt32 propU32(IORegistryEntry *e, const char *k)
{
    if (!e) return 0xffffffffu;
    OSData *d = OSDynamicCast(OSData, e->getProperty(k));
    if (!d || d->getLength() < 4) return 0xffffffffu;
    return *(const UInt32 *)d->getBytesNoCopy();
}

static bool armedFileExists()
{
    void *vp = nullptr;
    int err = vnode_lookup(ARMED_PATH, 0, &vp, vfs_context_current());
    if (err == 0 && vp) { vnode_put(vp); return true; }
    return false;
}

static void out64(IOExternalMethodArguments *a, unsigned i, UInt64 v)
{
    if (a && a->scalarOutput && i < a->scalarOutputCount) a->scalarOutput[i] = v;
}

// ------------------------------------------------------------------------- start
bool XeNative::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    setProperty("xnState", "start");

    IOService *gpu = nullptr;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (match) {
        OSIterator *it = IOService::getMatchingServices(match);
        if (it) {
            OSObject *o;
            while ((o = it->getNextObject())) {
                IOService *s = OSDynamicCast(IOService, o);
                if (!s) continue;
                if (propU32(s, "vendor-id") == 0x8086u && ((propU32(s, "class-code") >> 16) & 0xff) == 0x03) {
                    gpu = s; break;
                }
            }
            it->release();
        }
        match->release();
    }
    if (!gpu) { setProperty("xnState", "no-gpu"); registerService(); return true; }

    IOItemCount n = gpu->getDeviceMemoryCount();
    IODeviceMemory *bar0 = nullptr, *bar2 = nullptr;
    for (IOItemCount i = 0; i < n; i++) {
        IODeviceMemory *m = gpu->getDeviceMemoryWithIndex((unsigned)i);
        if (!m) continue;
        if (!bar0 && m->getLength() == 0x1000000ULL) bar0 = m;
        else if (!bar2 && m->getLength() >= 0x8000000ULL) bar2 = m;
    }
    if (!bar0 || !bar2) { setProperty("xnState", "no-bars"); registerService(); return true; }

    fBar0Map = bar0->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, bar0->getLength());
    fBar2Map = bar2->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, bar2->getLength());
    if (!fBar0Map || !fBar2Map) { setProperty("xnState", "map-failed"); registerService(); return true; }
    fReg = (volatile UInt32 *)fBar0Map->getVirtualAddress();
    publish("xnBar2Bytes", fBar2Map->getLength(), 64);

    // ---- what is live right now (also our restore state) -------------------
    fSurfBefore    = fReg[PLANE_SURF_1_A / 4];
    fCtlBefore     = fReg[PLANE_CTL_1_A / 4];
    fPipesrcBefore = fReg[TRANS_PIPESRC_A / 4];
    fSizeBefore    = fReg[PLANE_SIZE_1_A / 4];
    fStrideBefore  = fReg[PLANE_STRIDE_1_A / 4];
    fPsCtrlBefore  = fReg[PS_CTRL_A / 4];
    publish("xnSurfBefore", fSurfBefore, 32);
    publish("xnPlaneCtlBefore", fCtlBefore, 32);
    publish("xnPipesrcBefore", fPipesrcBefore, 32);
    publish("xnSizeBefore", fSizeBefore, 32);
    publish("xnStrideBefore", fStrideBefore, 32);
    publish("xnPsCtrlBefore", fPsCtrlBefore, 32);
    publish("xnSurfLiveBefore", fReg[PLANE_SURFLIVE_1_A / 4], 32);
    publish("xnTileOffBefore", fReg[PLANE_TILEOFF_1_A / 4], 32);

    // ---- stolen window ----------------------------------------------------
    UInt32 ggc  = fReg[GGC / 4];
    UInt32 ggms = (ggc >> 6) & 0x3;
    UInt32 gms  = (ggc >> 8) & 0x3Fu;
    UInt64 dsmRaw  = *(volatile UInt64 *)(volatile void *)((volatile UInt8 *)fReg + DSMBASE);
    UInt64 gsmAddr = *(volatile UInt64 *)(volatile void *)((volatile UInt8 *)fReg + GSMBASE);
    fDsm = dsmRaw & BDSM_MASK64;
    fStolen = (UInt64)gms * 32ULL * 1024ULL * 1024ULL;
    publish("xnGgc", ggc, 32);
    publish("xnGgms", ggms, 32);
    publish("xnGms", gms, 32);
    publish("xnDsmBase64", dsmRaw, 64);
    publish("xnStolenBytes", fStolen, 64);
    publish("xnGsmBase64", gsmAddr, 64);

    if (fStolen == 0) { setProperty("xnState", "stolen-zero"); registerService(); return true; }

    // ---- pick the page table: BAR2+8MiB first, then the GSM table ---------
    UInt64 idx = (UInt64)(fSurfBefore >> 12);
    UInt64 bar2len = fBar2Map->getLength();
    if (bar2len > GTT_PTE_OFFSET_BAR2) {
        volatile UInt64 *gttA = (volatile UInt64 *)((UInt8 *)fBar2Map->getVirtualAddress() + GTT_PTE_OFFSET_BAR2);
        UInt64 entriesA = (bar2len - GTT_PTE_OFFSET_BAR2) / 8;
        UInt64 pteA = (idx < entriesA) ? gttA[idx] : 0;
        publish("xnCandAEntries", entriesA, 64);
        publish("xnCandAPte", pteA, 64);
        if (pteA & 0x1) {
            fGtt = gttA; fEntries = entriesA; fLivePte = pteA;
            setProperty("xnChosenTable", "bar2+8M");
        }
    }
    fGttBytes = (UInt64)((ggms ? (1u << ggms) : 0)) << 20;   // 8 MiB for GGMS=3
    publish("xnGttBytes", fGttBytes, 64);
    UInt64 gttPhysGsm = gsmAddr & BDSM_MASK64;
    publish("xnGttPhysGsm", gttPhysGsm, 64);

    if (!fGtt && fGttBytes && gttPhysGsm) {
        UInt64 entriesB = fGttBytes / 8;
        IOMemoryDescriptor *td = _ZN18IOMemoryDescriptor19withPhysicalAddressEyyj(
            gttPhysGsm, fGttBytes, kIODirectionOutIn);
        if (td) {
            fGttMap = td->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, fGttBytes);
            td->release();
        }
        if (fGttMap) {
            volatile UInt64 *gttB = (volatile UInt64 *)fGttMap->getVirtualAddress();
            UInt64 pteB = (idx < entriesB) ? gttB[idx] : 0;
            publish("xnCandBEntries", entriesB, 64);
            publish("xnCandBPte", pteB, 64);
            fGtt = gttB; fEntries = entriesB; fLivePte = pteB;
            setProperty("xnChosenTable", "gsm");
        }
    }
    if (!fGtt) { setProperty("xnState", "no-gtt-table"); registerService(); return true; }

    fGttPhys = gttPhysGsm;
    publish("xnEntries", fEntries, 64);
    publish("xnLiveSurfIdx", idx, 64);
    publish("xnLivePte", fLivePte, 64);
    publish("xnLivePtePhys", fLivePte & PHYS_MASK, 64);

    {   // how many pages the live surface maps contiguously (measured 6656 here)
        UInt64 p0 = fLivePte & PHYS_MASK;
        for (UInt64 i = idx; i < fEntries && i < idx + 16384; i++) {
            UInt64 v = fGtt[i];
            if (!(v & 1) || (v & PHYS_MASK) != p0 + (i - idx) * 4096ULL) break;
            fLiveRun++;
        }
        publish("xnLiveRun", fLiveRun, 64);
    }

    fPteFlags = fLivePte & PTE_FLAG_MASK;
    if ((fPteFlags & 0x3u) != 0x3u) fPteFlags = 0x3u;
    publish("xnPteFlags", fPteFlags, 64);

    // ---- buffer: 16 MiB into stolen, clear of the table --------------------
    UInt64 base = fDsm + STOLEN_BIAS;
    if (fGttPhys && base < fGttPhys + fGttBytes && fGttPhys < base + NAT_SIZE)
        base = (fGttPhys + fGttBytes + 0xFFFFFULL) & ~0xFFFFFULL;
    fPhysBase = base;
    publish("xnPhysBase", fPhysBase, 64);
    publish("xnNeededBytes", NAT_SIZE, 64);
    publish("xnStolenEnd", fDsm + fStolen, 64);

    if (fPhysBase < fDsm || fPhysBase + NAT_SIZE > fDsm + fStolen) {
        setProperty("xnState", "stolen-too-small"); registerService(); return true;
    }

    bool hit = false;
    fChosenOff = findFreeRun((NAT_SIZE + 4095) / 4096, PREFERRED_OFF, &hit);
    publish("xnPlanOffset", fChosenOff, 64);
    setProperty("xnPlanPreferred", hit);
    fPlanOk = (fChosenOff != 0);
    setProperty("xnPlan", fPlanOk ? (hit ? "preferred-free" : "fallback-free") : "no-free-run");

    setProperty("xnState", "ready");
    registerService();
    return true;
}

UInt64 XeNative::findFreeRun(UInt64 pages, UInt64 prefer, bool *preferredHit)
{
    UInt64 back    = (fSurfBefore & ~0xFFFu) >> 12;
    UInt64 backEnd = back + fLiveRun + 64;

    // two passes: the preferred offset first, then everything above it, then below
    const UInt64 starts[3] = { prefer, 0x02000000ULL, 0x00400000ULL };
    for (int p = 0; p < 3; p++) {
        UInt64 off = starts[p];
        for (; off + pages * 4096 <= 0xFFFFF000ULL; off += 0x02000000ULL) {
            if (p == 1 && off >= prefer) break;
            if (p == 2 && off >= 0x02000000ULL) break;
            UInt64 i0 = off >> 12, i1 = i0 + pages;
            if (i1 > fEntries) break;
            if (i1 > back && i0 < backEnd) continue;
            bool free = true;
            for (UInt64 i = i0; i < i1; i++) {
                if (pteLooksLive(fGtt[i])) { free = false; break; }
            }
            if (free) { if (preferredHit) *preferredHit = (off == prefer); return off; }
        }
    }
    return 0;
}

void XeNative::snap(const char *tag)
{
    char k[64];
#define SNAP(field, reg) do { snprintf(k, sizeof(k), "%s" field, tag); publish(k, fReg[reg / 4], 32); } while (0)
    SNAP("Surf", PLANE_SURF_1_A);
    SNAP("SurfLive", PLANE_SURFLIVE_1_A);
    SNAP("Src", TRANS_PIPESRC_A);
    SNAP("Size", PLANE_SIZE_1_A);
    SNAP("Stride", PLANE_STRIDE_1_A);
    SNAP("Ps", PS_CTRL_A);
    SNAP("Ctl", PLANE_CTL_1_A);
    SNAP("TileOff", PLANE_TILEOFF_1_A);
#undef SNAP
}

// --------------------------------------------------------------------- pattern
bool XeNative::buildPattern(UInt64 phys, bool writeback)
{
    IOMemoryDescriptor *desc = _ZN18IOMemoryDescriptor19withPhysicalAddressEyyj(
        phys, NAT_SIZE, kIODirectionOutIn);
    if (!desc) { setProperty("xnState", "desc-failed"); return false; }

    IOMemoryMap *map = desc->createMappingInTask(kernel_task, 0,
                          kIOMapAnywhere | kIOMapInhibitCache, 0, NAT_SIZE);
    volatile UInt32 *pix = map ? (volatile UInt32 *)map->getVirtualAddress() : nullptr;
    setProperty("xnPhysMapped", map != nullptr);

    UInt8 *row = nullptr;
    if (!pix) {
        row = (UInt8 *)IOMalloc(NAT_ROW);
        if (!row) { if (map) map->release(); desc->release(); setProperty("xnState", "rowbuf-failed"); return false; }
    }

    for (UInt32 y = 0; y < NAT_H; y++) {
        UInt32 *dst = pix ? (UInt32 *)((UInt8 *)pix + (UInt64)y * NAT_ROW) : (UInt32 *)row;
        for (UInt32 x = 0; x < NAT_W; x++) {
            UInt32 v = (x < NAT_W / 2) ? 0xFFFF0000u : 0xFF0000FFu;   // left red, right blue
            if (y >= 1000 && y < 1080) v = ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;
            if (y >= 1040 && y < 1042 && x >= 1560 && x < 1960) v = 0xFF00FF00u;
            if (x < 8     && y < 8)     v = 0xFFFFFFFFu;
            if (x >= 3112 && y < 8)     v = 0xFFFFFFFFu;
            if (x < 8     && y >= 2072) v = 0xFFFFFFFFu;
            if (x >= 3112 && y >= 2072) v = 0xFFFFFFFFu;
            dst[x] = v;
        }
        if (!pix) desc->writeBytes((IOByteCount)((UInt64)y * NAT_ROW), row, NAT_ROW);
    }
    if (row) IOFree(row, NAT_ROW);
    OSSynchronizeIO();
    if (writeback) { xnWbinvd(); fFlushes++; publish("xnFlushCount", fFlushes, 32); }
    publish("xnPatternPx00",      pix ? (UInt32)pix[0] : 0xFFFFFFFFu, 32);
    publish("xnPatternPxCenter",  pix ? (UInt32)pix[(UInt64)1040 * NAT_W + 1700] : 0xFFFFFFFFu, 32);
    publish("xnPatternPxLastRow", pix ? (UInt32)pix[(UInt64)2079 * NAT_W + 3119] : 0xFFFFFFFFu, 32);

    if (map) map->release();
    desc->release();
    return true;
}

// --------------------------------------------------------------------- verbs
IOReturn XeNative::doStatus(IOExternalMethodArguments *args)
{
    out64(args, 0, 0x5845);                              // magic
    out64(args, 1, fSwitched ? 1 : 0);
    out64(args, 2, fGtt ? 1 : 0);
    out64(args, 3, fEntries);
    out64(args, 4, fLivePte);
    out64(args, 5, fChosenOff);
    out64(args, 6, fReg[PLANE_SURF_1_A / 4]);
    out64(args, 7, fReg[PLANE_SURFLIVE_1_A / 4]);
    out64(args, 8, fReg[PS_CTRL_A / 4]);
    out64(args, 9, fReg[TRANS_PIPESRC_A / 4]);
    out64(args, 10, fReg[PLANE_SIZE_1_A / 4]);
    out64(args, 11, fReg[PLANE_STRIDE_1_A / 4]);
    out64(args, 12, fReg[PLANE_CTL_1_A / 4]);
    out64(args, 13, fSurfBefore);
    out64(args, 14, fPhysBase);
    out64(args, 15, fReg[TRANS_FRMCOUNT_A / 4]);
    return kIOReturnSuccess;
}

// scan: [0]=nonzero entries below limit(arg0), [1]=present ones,
// [2]=length of the ascending physical run at the live surface index,
// [3]=live phys, [4..15]=PTEs liveIdx-1 .. liveIdx+10
IOReturn XeNative::doScan(IOExternalMethodArguments *args)
{
    if (!fGtt) return kIOReturnNotReady;
    UInt64 limit = (args && args->scalarInputCount > 0) ? args->scalarInput[0] : 0x100000;
    if (limit > fEntries) limit = fEntries;
    if (limit > 0x200000) limit = 0x200000;

    UInt64 nz = 0, present = 0;
    for (UInt64 i = 0; i < limit; i++) {
        UInt64 v = fGtt[i];
        if (v) { nz++; if (v & 1) present++; }
    }

    UInt64 idx = (UInt64)(fSurfBefore >> 12);
    UInt64 livePhys = fGtt[idx] & PHYS_MASK;
    UInt64 run = 0;
    for (UInt64 i = idx; i < fEntries && i < idx + 8192; i++) {
        UInt64 v = fGtt[i];
        if (!(v & 1)) break;
        if ((v & PHYS_MASK) != livePhys + (i - idx) * 4096ULL) break;
        run++;
    }

    out64(args, 0, nz);
    out64(args, 1, present);
    out64(args, 2, run);
    out64(args, 3, livePhys);
    for (unsigned k = 0; k < 12; k++) {
        UInt64 i = idx - 1 + k;
        out64(args, 4 + k, (i < fEntries) ? fGtt[i] : 0);
    }
    return kIOReturnSuccess;
}

IOReturn XeNative::switchCore(IOExternalMethodArguments *args, bool relative)
{
    if (!fGtt || !fPlanOk) return kIOReturnNotReady;
    if (!armedFileExists()) { setProperty("xnState", "disarmed"); return kIOReturnNotPermitted; }
    setProperty("xnArmedSeen", "yes");

    if (!buildPattern(fPhysBase)) return kIOReturnError;

    UInt64 idx = fChosenOff >> 12;
    UInt64 pages = (NAT_SIZE + 4095) / 4096;         // 6338
    UInt64 encBase = relative ? (fPhysBase - fDsm) : fPhysBase;
    for (UInt64 i = 0; i < pages; i++)
        fGtt[idx + i] = ((encBase + i * 4096ULL) & PHYS_MASK) | fPteFlags;

    xnWbinvd(); fFlushes++; publish("xnFlushCount", fFlushes, 32);
    fReg[GFX_FLSH_CNTL / 4] = 1;
    fReg[GFX_FLSH_CNTL / 4] = 0;
    IOSleep(50);
    OSSynchronizeIO();

    UInt64 pteFirst = fGtt[idx], pteLast = fGtt[idx + pages - 1];
    UInt64 wantFirst = (encBase & PHYS_MASK) | fPteFlags;
    UInt64 wantLast  = ((encBase + (pages - 1) * 4096ULL) & PHYS_MASK) | fPteFlags;
    publish("xnPteFirst", pteFirst, 64);
    publish("xnPteLast", pteLast, 64);
    publish("xnPteWantFirst", wantFirst, 64);
    publish("xnPteWantLast", wantLast, 64);
    setProperty("xnPteMatch", pteFirst == wantFirst && pteLast == wantLast);
    if (pteFirst != wantFirst || pteLast != wantLast) {
        setProperty("xnState", "pte-verify-failed");
        return kIOReturnError;
    }

    snap("xnA");
    fReg[TRANS_PIPESRC_A / 4]  = 0x0C2F081Fu;
    IOSleep(5);
    fReg[PLANE_SIZE_1_A / 4]   = 0x081F0C2Fu;
    IOSleep(5);
    fReg[PLANE_STRIDE_1_A / 4] = NAT_ROW / 64;
    IOSleep(5);
    fReg[PS_CTRL_A / 4]        = fReg[PS_CTRL_A / 4] & ~0x80000000u;
    IOSleep(5);
    fReg[PLANE_SURF_1_A / 4]   = (UInt32)fChosenOff;
    IOSleep(1000);
    snap("xnB");
    IOSleep(3000);
    snap("xnC");
    fSwitched = true;

    UInt32 fc1 = fReg[TRANS_FRMCOUNT_A / 4];
    IOSleep(300);
    UInt32 fc2 = fReg[TRANS_FRMCOUNT_A / 4];
    publish("xnFrameCountDelta", fc2 - fc1, 32);
    publish("xnMacSurfPteAfter", fGtt[(fSurfBefore & ~0xFFFu) >> 12], 64);
    setProperty("xnState", "switched");

    out64(args, 0, fReg[PLANE_SURF_1_A / 4]);
    out64(args, 1, fReg[PLANE_SURFLIVE_1_A / 4]);
    out64(args, 2, fReg[PS_CTRL_A / 4]);
    out64(args, 3, fReg[TRANS_PIPESRC_A / 4]);
    out64(args, 4, fReg[PLANE_SIZE_1_A / 4]);
    out64(args, 5, fReg[PLANE_STRIDE_1_A / 4]);
    return kIOReturnSuccess;
}

IOReturn XeNative::doFill(IOExternalMethodArguments *args)
{
    if (!fReg) return kIOReturnNotReady;
    UInt64 wb = (args && args->scalarInputCount > 0) ? args->scalarInput[0] : 1;
    bool ok = buildPattern(fPhysBase, wb != 0);
    out64(args, 0, ok ? 1 : 0);
    out64(args, 1, fPhysBase);
    out64(args, 2, fFlushes);
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn XeNative::doFillAt(IOExternalMethodArguments *args)
{
    UInt64 phys = (args && args->scalarInputCount > 0) ? args->scalarInput[0] : 0;
    UInt64 wb   = (args && args->scalarInputCount > 1) ? args->scalarInput[1] : 1;
    if (!phys) phys = fLivePte & PHYS_MASK;      // default: the surface macOS is scanning
    if (phys == 0) return kIOReturnBadArgument;
    bool ok = buildPattern(phys, wb != 0);
    out64(args, 0, ok ? 1 : 0);
    out64(args, 1, phys);
    out64(args, 2, fFlushes);
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn XeNative::doPte(IOExternalMethodArguments *args)
{
    if (!fGtt || !args || args->scalarInputCount < 2) return kIOReturnBadArgument;
    UInt64 idx = args->scalarInput[0], val = args->scalarInput[1];
    UInt64 wb  = (args->scalarInputCount > 2) ? args->scalarInput[2] : 1;
    if (idx >= fEntries) return kIOReturnBadArgument;
    fGtt[idx] = val;
    if (wb) { xnWbinvd(); fFlushes++; publish("xnFlushCount", fFlushes, 32); }
    fReg[GFX_FLSH_CNTL / 4] = 1;
    fReg[GFX_FLSH_CNTL / 4] = 0;
    IOSleep(20);
    OSSynchronizeIO();
    out64(args, 0, fGtt[idx]);
    out64(args, 1, fFlushes);
    return kIOReturnSuccess;
}

IOReturn XeNative::doFlush(IOExternalMethodArguments *args)
{
    xnWbinvd(); fFlushes++;
    publish("xnFlushCount", fFlushes, 32);
    setProperty("xnState", "flushed");
    out64(args, 0, 1);
    out64(args, 1, fFlushes);
    return kIOReturnSuccess;
}

IOReturn XeNative::doReg(IOExternalMethodArguments *args)
{
    if (!fReg || !args || args->scalarInputCount < 2) return kIOReturnBadArgument;
    UInt64 which = args->scalarInput[0], val = args->scalarInput[1];
    UInt32 off;
    switch (which) {
    case 0: off = PLANE_SURF_1_A;    break;
    case 1: off = PLANE_SIZE_1_A;    break;
    case 2: off = PLANE_STRIDE_1_A;  break;
    case 3: off = TRANS_PIPESRC_A;   break;
    case 4: off = PS_CTRL_A;         break;
    case 5: off = PLANE_CTL_1_A;     break;
    default: return kIOReturnBadArgument;
    }
    fReg[off / 4] = (UInt32)val;
    IOSleep(20);
    out64(args, 0, fReg[off / 4]);
    out64(args, 1, fReg[PLANE_SURFLIVE_1_A / 4]);
    return kIOReturnSuccess;
}

// A-13: write several display registers back to back and sleep once. The previous
// shape (one selector per register, 20 ms apart in user space) left the pipe in a
// half-reconfigured state for ~60 ms, which is long enough for the hardware to
// underrun. `which` uses exactly the same 0..5 map as doReg.
IOReturn XeNative::doRegSet(IOExternalMethodArguments *args)
{
    if (!fReg || !args || args->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt64 n = args->scalarInput[0];
    if (!n || n > 8) return kIOReturnBadArgument;
    if (args->scalarInputCount < 1 + 2 * n) return kIOReturnBadArgument;

    UInt32 offs[8];
    for (UInt64 i = 0; i < n; i++) {
        switch (args->scalarInput[1 + i * 2]) {
        case 0: offs[i] = PLANE_SURF_1_A;    break;
        case 1: offs[i] = PLANE_SIZE_1_A;    break;
        case 2: offs[i] = PLANE_STRIDE_1_A;  break;
        case 3: offs[i] = TRANS_PIPESRC_A;   break;
        case 4: offs[i] = PS_CTRL_A;         break;
        case 5: offs[i] = PLANE_CTL_1_A;     break;
        default: return kIOReturnBadArgument;
        }
    }

    for (UInt64 i = 0; i < n; i++)
        fReg[offs[i] / 4] = (UInt32)args->scalarInput[2 + i * 2];
    OSSynchronizeIO();
    IOSleep(20);

    for (UInt64 i = 0; i < n; i++) {
        out64(args, i * 2,     args->scalarInput[1 + i * 2]);   // which
        out64(args, i * 2 + 1, fReg[offs[i] / 4]);              // readback
    }
    out64(args, n * 2,     fReg[PLANE_SURFLIVE_1_A / 4]);
    out64(args, n * 2 + 1, fReg[PS_CTRL_A / 4]);
    return kIOReturnSuccess;
}

// A-13b: raw BAR0 read. Read-only, so it cannot destabilise the pipe; it exists
// because every live question so far (timing, scaler window, watermark,
// underrun bits) lives in registers no verb exposed.
IOReturn XeNative::doRegRead(IOExternalMethodArguments *args)
{
    if (!fReg || !args || args->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt64 off = args->scalarInput[0];
    UInt64 n = (args->scalarInputCount > 1) ? args->scalarInput[1] : 1;
    if ((off & 3) || off >= 0x1000000ULL) return kIOReturnBadArgument;
    if (!n || n > 16) return kIOReturnBadArgument;
    for (UInt64 i = 0; i < n; i++) {
        UInt64 o = off + i * 4;
        out64(args, i, (o < 0x1000000ULL) ? fReg[o / 4] : 0);
    }
    return kIOReturnSuccess;
}

// A-13b: one colour over the whole plane surface, written through the aperture.
// Same path apfill uses (the only pixel path proven to reach the engine), but
// the content carries no structure -- so "black" can only mean the fetch or the
// pipe is failing, not that the pattern is being sampled at the wrong pitch.
IOReturn XeNative::doApSolid(IOExternalMethodArguments *args)
{
    if (!fBar2Map || !args || args->scalarInputCount < 2) return kIOReturnNotReady;
    UInt64 idx   = args->scalarInput[0];
    UInt32 col   = (UInt32)args->scalarInput[1];
    UInt64 mode  = (args->scalarInputCount > 2) ? args->scalarInput[2] : 0;
    UInt32 w = mode ? NAT_W : P1080_W;
    UInt32 h = mode ? NAT_H : P1080_H;

    UInt64 off  = idx << 12;
    UInt64 need = (UInt64)w * h * 4;
    UInt64 len  = fBar2Map->getLength();
    if (off + need > len) { out64(args, 0, 0); out64(args, 3, len); return kIOReturnSuccess; }

    volatile UInt32 *pix = (volatile UInt32 *)((volatile UInt8 *)fBar2Map->getVirtualAddress() + off);
    for (UInt32 y = 0; y < h; y++)
        for (UInt32 x = 0; x < w; x++)
            pix[(UInt64)y * w + x] = col;
    OSSynchronizeIO();

    out64(args, 0, 1);
    out64(args, 1, pix[0]);
    out64(args, 2, pix[(UInt64)(h / 2) * w + w / 2]);
    out64(args, 3, len);
    return kIOReturnSuccess;
}

// ------------------------------------------------- A-12: kernel buffer path
bool XeNative::ensureBuf(UInt64 physMask)
{
    if (sBuf && fBufMask == physMask) return true;
    if (sBuf) {                       // changing the mask: drop the old buffer
        sBuf->release();
        sBuf = nullptr;
        fBufMask = 0;
    }

    if (physMask) {
        sBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                   kernel_task, kIODirectionOutIn, (mach_vm_size_t)NAT_SIZE,
                   (mach_vm_address_t)physMask);
        setProperty("xnBufAlloc", "physmask");
    } else {
        sBuf = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionOutIn,
                                                           (vm_size_t)NAT_SIZE, (vm_size_t)BUF_ALIGN);
        setProperty("xnBufAlloc", "options");
    }
    if (!sBuf) { setProperty("xnState", "buf-alloc-failed"); return false; }
    fBufMask = physMask;

    void *va = sBuf->getBytesNoCopy();
    if (!va) { sBuf->release(); sBuf = nullptr; setProperty("xnState", "buf-va-failed"); return false; }

    fBufPages = (NAT_SIZE + 4095) / 4096;
    fBufPhys0 = 0; fBufPhysLast = 0; fBufPhysMax = 0;
    for (UInt64 i = 0; i < fBufPages; i++) {
        addr64_t pa = sBuf->getPhysicalSegment((IOByteCount)(i * 4096), nullptr, 0);
        if (!pa || (pa & 0xFFFULL)) {
            setProperty("xnState", "buf-bad-phys");
            return false;
        }
        if (i == 0) fBufPhys0 = pa;
        if (pa > fBufPhysMax) fBufPhysMax = pa;
        fBufPhysLast = pa;
    }
    publish("xnBufPhys0", fBufPhys0, 64);
    publish("xnBufPhysLast", fBufPhysLast, 64);
    publish("xnBufPhysMax", fBufPhysMax, 64);
    publish("xnBufPages", fBufPages, 64);
    setProperty("xnBufHeld", "static");
    return true;
}

bool XeNative::paintVA(volatile UInt32 *pix, UInt32 w, UInt32 h)
{
    if (!pix) return false;

    const UInt32 half = w / 2;
    UInt64 sum = 0;

    for (UInt32 y = 0; y < h; y++) {
        volatile UInt32 *dst = pix + (UInt64)y * w;
        for (UInt32 x = 0; x < w; x++) {
            UInt32 v = (x < half) ? 0xFFFF0000u : 0xFF0000FFu;          // left red / right blue
            if (y >= h - 72 && y < h - 8)                                // 1px checker band
                v = ((x + y) & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
            if (y >= h / 2 && y < h / 2 + 6 && x >= (w * 3) / 4 && x < (w * 15) / 16)
                v = 0xFF00FF00u;                                         // green bar, centre right
            if ((x < 16 || x >= w - 16) && (y < 16 || y >= h - 16))
                v = 0xFFFFFFFFu;                                         // four corner blocks
            dst[x] = v;
            sum += (v & 0x00FFFFFFu);
        }
    }

    // Flush every painted cache line explicitly. wbinvd only covers the core the
    // call happens to be running on; a migration mid-loop leaves other cores'
    // dirty lines behind. clflush is per-address, so it cannot miss.
    UInt64 bytes = (UInt64)w * h * 4;
    for (UInt64 off = 0; off < bytes; off += 64)
        __asm__ __volatile__("clflush (%0)" :: "r"((const char *)pix + off) : "memory");

    __asm__ __volatile__("mfence" ::: "memory");
    xnWbinvd();
    fFlushes++;
    OSSynchronizeIO();

    fBufW = w; fBufH = h; fBufSum = sum;
    fBufPx0    = pix[0];
    fBufPxMid  = pix[(UInt64)(h / 2) * w + w / 2];
    fBufPxLast = pix[(UInt64)(h - 1) * w + (w - 1)];

    publish("xnBufSum", fBufSum, 64);
    publish("xnBufPx0", fBufPx0, 32);
    publish("xnBufPxMid", fBufPxMid, 32);
    publish("xnBufPxLast", fBufPxLast, 32);
    publish("xnBufW", fBufW, 32);
    publish("xnBufH", fBufH, 32);
    return true;
}

bool XeNative::paintBuf(UInt32 w, UInt32 h)
{
    if (!sBuf) return false;
    return paintVA((volatile UInt32 *)sBuf->getBytesNoCopy(), w, h);
}

// Read one dword of physical memory through a throwaway mapping. This is the only
// way to see what the display engine would fetch (as opposed to what our own
// virtual alias holds).
static UInt32 xnPeek32(UInt64 phys, bool *ok)
{
    UInt64 page = phys & ~0xFFFULL;
    UInt32 off  = (UInt32)(phys & 0xFFFULL);
    if (off > 0xFFC) { if (ok) *ok = false; return 0xDEADBEEFu; }

    IOMemoryDescriptor *d = _ZN18IOMemoryDescriptor19withPhysicalAddressEyyj(
        page, 0x1000, kIODirectionOutIn);
    if (!d) { if (ok) *ok = false; return 0xDEADBEEFu; }
    IOMemoryMap *m = d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, 0x1000);
    UInt32 v = 0xDEADBEEFu;
    if (m) {
        v = *(volatile UInt32 *)((volatile UInt8 *)m->getVirtualAddress() + off);
        m->release();
        if (ok) *ok = true;
    } else if (ok) *ok = false;
    d->release();
    return v;
}

// Every entry is compared against the page the descriptor reports right now.
// outBad counts mismatches; samples are the entries at 8 pseudo-random pages.
UInt64 XeNative::bufVerify(UInt64 idx, UInt64 pages, UInt64 *outFirst, UInt64 *outMid,
                           UInt64 *outLast, unsigned *outBad)
{
    unsigned bad = 0;
    UInt64 first = 0, mid = 0, last = 0;
    for (UInt64 i = 0; i < pages; i++) {
        addr64_t pa = sBuf->getPhysicalSegment((IOByteCount)(i * 4096), nullptr, 0);
        UInt64 want = (pa & PHYS_MASK) | fPteFlags;
        UInt64 got  = fGtt[idx + i];
        if (got != want) bad++;
        if (i == 0) first = got;
        if (i == pages / 2) mid = got;
        if (i == pages - 1) last = got;
    }
    if (outFirst) *outFirst = first;
    if (outMid)   *outMid = mid;
    if (outLast)  *outLast = last;
    if (outBad)   *outBad = bad;
    return bad;
}

UInt64 XeNative::mapBufPtes(UInt32 w, UInt32 h, unsigned *outBad)
{
    UInt64 pages = bufPages(w, h);
    if (BUF_IDX + pages > fEntries) return 0;

    for (UInt64 i = 0; i < pages; i++) {
        addr64_t pa = sBuf->getPhysicalSegment((IOByteCount)(i * 4096), nullptr, 0);
        if (!pa || (pa & 0xFFFULL)) return 0;
        fGtt[BUF_IDX + i] = (pa & PHYS_MASK) | fPteFlags;
    }

    __asm__ __volatile__("mfence" ::: "memory");
    xnWbinvd();
    fFlushes++;
    fReg[GFX_FLSH_CNTL / 4] = 1;
    fReg[GFX_FLSH_CNTL / 4] = 0;
    IOSleep(20);
    OSSynchronizeIO();

    UInt64 f = 0, m = 0, l = 0;
    unsigned bad = 0;
    bufVerify(BUF_IDX, pages, &f, &m, &l, &bad);
    if (outBad) *outBad = bad;

    fBufIdx = BUF_IDX; fBufMapped = pages; fBufBad = bad;
    publish("xnBufMapped", fBufMapped, 64);
    publish("xnBufBad", bad, 32);
    publish("xnBufPteFirst", f, 64);
    publish("xnBufPteMid", m, 64);
    publish("xnBufPteLast", l, 64);
    setProperty("xnBufVerify", bad == 0 ? "ok" : "mismatch");
    return pages;
}

IOReturn XeNative::doBuf(IOExternalMethodArguments *args)
{
    if (!fReg || !fGtt) return kIOReturnNotReady;
    UInt64 mode = (args && args->scalarInputCount > 0) ? args->scalarInput[0] : 0;
    UInt64 mask = (args && args->scalarInputCount > 1) ? args->scalarInput[1] : 0;
    if (!ensureBuf(mask)) return kIOReturnNoMemory;

    UInt32 w = mode ? NAT_W : P1080_W;
    UInt32 h = mode ? NAT_H : P1080_H;
    if (!paintBuf(w, h)) return kIOReturnError;

    // What the engine would actually fetch: read the buffer back by physical
    // address, not through the VA we just wrote.
    bool ok1 = false, ok2 = false, ok3 = false;
    UInt32 self0  = xnPeek32(fBufPhys0, &ok1);
    UInt32 self1  = xnPeek32(fBufPhys0 + 64, &ok2);
    UInt32 self1m = xnPeek32(fBufPhys0 + 0x100000, &ok3);
    publish("xnSelf0", self0, 32);
    publish("xnSelf64", self1, 32);
    publish("xnSelf1M", self1m, 32);
    setProperty("xnSelfPeek", (ok1 && ok2 && ok3) ? "ok" : "failed");

    unsigned bad = 0;
    UInt64 pages = mapBufPtes(w, h, &bad);
    if (!pages) { setProperty("xnState", "buf-map-failed"); return kIOReturnError; }

    setProperty("xnBufSurf", mode ? "0x20000000/3120" : "0x20000000/1080");

    out64(args, 0,  bad == 0 ? 1 : 0);
    out64(args, 1,  BUF_SURF);
    out64(args, 2,  pages);
    out64(args, 3,  fBufPhys0);
    out64(args, 4,  fBufPhysLast);
    out64(args, 5,  self0);
    out64(args, 6,  self1);
    out64(args, 7,  self1m);
    out64(args, 8,  fBufSum);
    out64(args, 9,  bad);
    out64(args, 10, w);
    out64(args, 11, h);
    out64(args, 12, fBufPx0);
    out64(args, 13, fBufPxMid);
    out64(args, 14, fBufPxLast);
    out64(args, 15, fBufPhysMax);

    if (bad) { setProperty("xnState", "buf-verify-failed"); return kIOReturnError; }
    setProperty("xnState", mode ? "buf3120-ready" : "buf1080-ready");
    return kIOReturnSuccess;
}

// Paint the very same buffer through a mapping created from its physical address
// and held forever (the shape A-11 found working for the PTE table).
IOReturn XeNative::doBufVia(IOExternalMethodArguments *args)
{
    if (!fReg || !fGtt) return kIOReturnNotReady;
    if (!ensureBuf(0)) return kIOReturnNoMemory;

    static IOMemoryMap *sMap = nullptr;
    if (!sMap) {
        IOMemoryDescriptor *d = _ZN18IOMemoryDescriptor19withPhysicalAddressEyyj(
            fBufPhys0, NAT_SIZE, kIODirectionOutIn);
        if (!d) return kIOReturnNoMemory;
        sMap = d->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, NAT_SIZE);
        d->release();
        if (!sMap) { setProperty("xnState", "bufvia-map-failed"); return kIOReturnNoMemory; }
        setProperty("xnBufViaHeld", "static");
    }

    if (!paintVA((volatile UInt32 *)sMap->getVirtualAddress(), P1080_W, P1080_H))
        return kIOReturnError;

    bool ok = false;
    UInt32 self0 = xnPeek32(fBufPhys0, &ok);

    unsigned bad = 0;
    UInt64 pages = mapBufPtes(P1080_W, P1080_H, &bad);
    if (!pages) return kIOReturnError;

    out64(args, 0, bad == 0 ? 1 : 0);
    out64(args, 1, BUF_SURF);
    out64(args, 2, pages);
    out64(args, 3, fBufPhys0);
    out64(args, 4, self0);
    out64(args, 5, bad);
    out64(args, 6, fBufSum);
    out64(args, 7, fBufPx0);
    setProperty("xnState", "bufvia-ready");
    return kIOReturnSuccess;
}

// A-12: the display engine reaches only physical addresses below ~256 MB on this
// machine (measured: 128 MB renders, 256 MB is black). Allocate the buffer with a
// physical-address mask so every page lands under that line.
IOReturn XeNative::doBufLow(IOExternalMethodArguments *args)
{
    if (!fReg || !fGtt) return kIOReturnNotReady;
    UInt64 mask  = (args && args->scalarInputCount > 0 && args->scalarInput[0])
                       ? args->scalarInput[0] : 0x0FFFFFFFULL;   // below 256 MB
    UInt64 tries = (args && args->scalarInputCount > 1) ? args->scalarInput[1] : 8;
    if (tries > 16) tries = 16;

    if (sBuf) { sBuf->release(); sBuf = nullptr; fBufMask = 0; }

    UInt64 bestPhys = 0;
    IOBufferMemoryDescriptor *best = nullptr;
    bool got = false;

    for (UInt64 t = 0; t < tries && !got; t++) {
        // 1: unmapped DMA buffer with an address mask (the path Apple documents for
        //    hardware with a limited number of address bits), 2: mapped task buffer,
        //    3: plain allocation. First one whose every page lands under the mask wins.
        IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            NULL, kIODirectionOutIn, (mach_vm_size_t)NAT_SIZE, (mach_vm_address_t)mask);
        if (!b) {
            b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task, kIODirectionOutIn | kIOMemoryPhysicallyContiguous,
                (mach_vm_size_t)NAT_SIZE, (mach_vm_address_t)mask);
        }
        if (!b) { IOSleep(20); continue; }

        addr64_t p0 = b->getPhysicalSegment(0, nullptr, 0);
        addr64_t pN = b->getPhysicalSegment((IOByteCount)(NAT_SIZE - 1), nullptr, 0);
        bool under = p0 && pN && (((p0 | pN) & ~mask) == 0);
        publish("xnLowTries", t + 1, 64);
        if (under) { best = b; bestPhys = p0; got = true; break; }
        if (!best || p0 < bestPhys) { if (best) best->release(); best = b; bestPhys = p0; }
        else b->release();
    }

    if (got) { sBuf = best; fBufMask = mask; }
    else {
        if (best) { best->release(); best = nullptr; }
        publish("xnLowBest", bestPhys, 64);
        setProperty("xnState", "no-low-buffer");
        out64(args, 0, 0);
        out64(args, 3, bestPhys);
        return kIOReturnSuccess;          // report, do not throw the outputs away
    }

    // A NULL-task buffer is unmapped; map it through the descriptor itself (not
    // through withPhysicalAddress, which does not reach the engine's memory).
    if (fBufMapPI) { fBufMapPI->release(); fBufMapPI = nullptr; }
    if (!sBuf->getBytesNoCopy()) {
        fBufMapPI = sBuf->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, NAT_SIZE);
        if (!fBufMapPI) { setProperty("xnState", "buf-map-pi-failed"); out64(args, 0, 0); return kIOReturnSuccess; }
        setProperty("xnBufMappedVIA", "descriptor");
    }

    if (!paintVA(fBufMapPI ? (volatile UInt32 *)fBufMapPI->getVirtualAddress()
                           : (volatile UInt32 *)sBuf->getBytesNoCopy(), P1080_W, P1080_H)) {
        setProperty("xnState", "buf-paint-failed"); out64(args, 0, 0); return kIOReturnSuccess;
    }

    bool okp = false;
    UInt32 self0 = xnPeek32(fBufPhys0, &okp);

    unsigned bad = 0;
    UInt64 pages = mapBufPtes(P1080_W, P1080_H, &bad);
    if (!pages) { setProperty("xnState", "buf-map-failed"); return kIOReturnError; }

    setProperty("xnBufAlloc", "physmask-low");
    setProperty("xnState", "buflow-ready");

    out64(args, 0, bad == 0 ? 1 : 0);
    out64(args, 1, BUF_SURF);
    out64(args, 2, pages);
    out64(args, 3, fBufPhys0);
    out64(args, 4, self0);
    out64(args, 5, bad);
    out64(args, 6, fBufPhysMax);
    out64(args, 7, mask);
    return kIOReturnSuccess;
}

// Paint the pattern straight through the GTT aperture (BAR2 + index*4096). MMIO
// writes are seen by the display engine by construction, and they need no
// physical-address mapping at all.
IOReturn XeNative::doApFill(IOExternalMethodArguments *args)
{
    if (!fBar2Map || !args || args->scalarInputCount < 1) return kIOReturnNotReady;
    UInt64 idx  = args->scalarInput[0];
    UInt64 mode = (args->scalarInputCount > 1) ? args->scalarInput[1] : 0;
    UInt32 w = mode ? NAT_W : P1080_W;
    UInt32 h = mode ? NAT_H : P1080_H;

    UInt64 off  = idx << 12;
    UInt64 need = (UInt64)w * h * 4;
    UInt64 len  = fBar2Map->getLength();
    if (off + need > len) {
        setProperty("xnState", "apfill-out-of-aperture");
        out64(args, 0, 0);
        out64(args, 1, len);
        out64(args, 2, off + need);
        return kIOReturnSuccess;
    }

    volatile UInt32 *pix = (volatile UInt32 *)((volatile UInt8 *)fBar2Map->getVirtualAddress() + off);
    if (!paintVA(pix, w, h)) { out64(args, 0, 0); return kIOReturnError; }

    out64(args, 0, 1);
    out64(args, 1, len);
    out64(args, 2, off + need);
    out64(args, 3, pix[0]);
    out64(args, 4, pix[(UInt64)(h / 2) * w + w / 2]);
    out64(args, 5, fBufSum);
    setProperty("xnState", "apfilled");
    return kIOReturnSuccess;
}

IOReturn XeNative::doMapFast(IOExternalMethodArguments *args)
{
    if (!fGtt || !args || args->scalarInputCount < 3) return kIOReturnBadArgument;
    UInt64 idx = args->scalarInput[0], cnt = args->scalarInput[1], phys = args->scalarInput[2];
    if (!cnt || idx + cnt > fEntries) return kIOReturnBadArgument;

    for (UInt64 i = 0; i < cnt; i++)
        fGtt[idx + i] = ((phys + i * 4096ULL) & PHYS_MASK) | fPteFlags;

    __asm__ __volatile__("mfence" ::: "memory");
    xnWbinvd();
    fFlushes++;
    fReg[GFX_FLSH_CNTL / 4] = 1;
    fReg[GFX_FLSH_CNTL / 4] = 0;
    IOSleep(20);
    OSSynchronizeIO();

    UInt64 bad = 0;
    for (UInt64 i = 0; i < cnt; i++)
        if (fGtt[idx + i] != (((phys + i * 4096ULL) & PHYS_MASK) | fPteFlags)) bad++;

    out64(args, 0, idx);
    out64(args, 1, cnt);
    out64(args, 2, fGtt[idx]);
    out64(args, 3, fGtt[idx + cnt - 1]);
    out64(args, 4, bad);
    return kIOReturnSuccess;
}

IOReturn XeNative::doPeek(IOExternalMethodArguments *args)
{
    if (!args || args->scalarInputCount < 1) return kIOReturnBadArgument;
    UInt64 phys = args->scalarInput[0];
    UInt64 n = (args->scalarInputCount > 1) ? args->scalarInput[1] : 8;
    if (n > 16) n = 16;
    for (UInt64 i = 0; i < n; i++) {
        bool ok = false;
        UInt32 v = xnPeek32(phys + i * 4, &ok);
        out64(args, i, ok ? v : 0xDEADBEEFu);
    }
    return kIOReturnSuccess;
}

IOReturn XeNative::doWhere(IOExternalMethodArguments *args)
{
    UInt64 f = 0, m = 0, l = 0;
    unsigned bad = 0;
    if (fGtt && fBufMapped) bufVerify(BUF_IDX, fBufMapped, &f, &m, &l, &bad);

    out64(args, 0,  fBufPhys0);
    out64(args, 1,  fBufPages);
    out64(args, 2,  fBufW);
    out64(args, 3,  fBufH);
    out64(args, 4,  BUF_SURF);
    out64(args, 5,  fBufMapped);
    out64(args, 6,  fBufSum);
    out64(args, 7,  fBufPx0);
    out64(args, 8,  fBufPxMid);
    out64(args, 9,  fBufPxLast);
    out64(args, 10, f);
    out64(args, 11, m);
    out64(args, 12, l);
    out64(args, 13, bad);
    out64(args, 14, fBufPhysMax);
    out64(args, 15, fFlushes);
    return kIOReturnSuccess;
}

// A-13c: STRIDE/SIZE/PIPESRC are double buffered. They are latched on the
// update that carries SURF, so writing them without a following SURF write
// leaves them staged -- the readback looks correct while the pipe keeps using
// the old geometry. Nudging SURF one page and back is a cheap, always-legal
// latch (the nudge stays inside the surface, so nothing unmapped is fetched).
IOReturn XeNative::doLatch(IOExternalMethodArguments *args)
{
    if (!fReg) return kIOReturnNotReady;
    UInt32 cur = fReg[PLANE_SURF_1_A / 4];
    fReg[PLANE_SURF_1_A / 4] = cur + 0x1000;
    OSSynchronizeIO();
    IOSleep(20);
    fReg[PLANE_SURF_1_A / 4] = cur;
    OSSynchronizeIO();
    IOSleep(20);
    out64(args, 0, fReg[PLANE_SURF_1_A / 4]);
    out64(args, 1, fReg[PLANE_SURFLIVE_1_A / 4]);
    return kIOReturnSuccess;
}

// Restore order matters: geometry first (staged), SURF last so the same update
// latches it. Writing SURF first -- the pre-A-13c order -- latched the old 3120
// geometry onto the desktop surface and left the screen sheared.
IOReturn XeNative::doRestore(IOExternalMethodArguments *args)
{
    if (!fReg) return kIOReturnNotReady;
    fReg[PLANE_STRIDE_1_A / 4] = fStrideBefore;
    fReg[PLANE_SIZE_1_A / 4]   = fSizeBefore;
    fReg[TRANS_PIPESRC_A / 4]  = fPipesrcBefore;
    fReg[PS_CTRL_A / 4]        = fPsCtrlBefore;
    OSSynchronizeIO();
    IOSleep(5);
    fReg[PLANE_SURF_1_A / 4]   = fSurfBefore + 0x1000;   // force the latch
    OSSynchronizeIO();
    IOSleep(20);
    fReg[PLANE_SURF_1_A / 4]   = fSurfBefore;
    OSSynchronizeIO();
    IOSleep(60);
    fSwitched = false;
    setProperty("xnState", "restored");

    out64(args, 0, fReg[PLANE_SURF_1_A / 4]);
    out64(args, 1, fReg[PLANE_SURFLIVE_1_A / 4]);
    out64(args, 2, fReg[PS_CTRL_A / 4]);
    out64(args, 3, fReg[TRANS_PIPESRC_A / 4]);
    out64(args, 4, fReg[PLANE_SIZE_1_A / 4]);
    out64(args, 5, fReg[PLANE_STRIDE_1_A / 4]);
    return kIOReturnSuccess;
}

void XeNative::free()
{
    if (fBar0Map) fBar0Map->release();
    if (fBar2Map) fBar2Map->release();
    if (fGttMap)  fGttMap->release();
    IOService::free();
}
