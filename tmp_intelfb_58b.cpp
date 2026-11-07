// XeGfxIntelFramebuffer: Apple's IONDRVFramebuffer under a class name with "Intel" in it. CoreDisplay picks the GPU
// vendor from the IOFramebuffer class name, and IONDRVFramebuffer reads as no vendor, which turns IOPresentment off
// for WindowServer (I-108). Lives in the AuxKC because IONDRVSupport is in the system collection.
// <wtl> fork: nvgfx | author: watchthelight </wtl>
#include <IOKit/ndrvsupport/IONDRVFramebuffer.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <mach/mach_time.h>
#include <kern/thread_call.h>
#include <kern/clock.h>

#define XIFBLOG(fmt, ...) IOLog("XeGfxIntelFB: " fmt "\n", ##__VA_ARGS__)

// Boot guard as in NMIntelFB: each start counts in NVRAM, 120 s of uptime after start clears it, and two counted starts
// in a row make probe decline so IONDRVFramebuffer drives the panel again. Boot-arg -xeintelfboff declines always.
#define XIFB_GUARD_VAR "xeintelfb-starts"
// About This Mac shows "model" followed by the VRAM size; name the chip generation and graphics architecture
static const char *xifbModelName(uint16_t dev)
{
    switch (dev) {
    case 0x7d67: case 0x7d41: return "Intel Arrow Lake Xe-LPG";
    case 0x7d51: case 0x7dd1: return "Intel Arrow Lake Xe-LPG+";
    case 0x7d40: case 0x7d45: case 0x7d55: case 0x7dd5: return "Intel Meteor Lake Xe-LPG";
    default: return "Intel Xe Graphics";
    }
}
static unsigned xifbGuardStarts(IORegistryEntry *o)
{
    OSObject *v = o->copyProperty(XIFB_GUARD_VAR); OSData *d = OSDynamicCast(OSData, v);
    unsigned n = (d && d->getLength() >= 1) ? *(const unsigned char *)d->getBytesNoCopy() : 0;
    if (v) v->release();
    return n;
}
static bool xifbGuardTripped()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return false;
    bool t = xifbGuardStarts(o) >= 2; o->release(); return t;
}
static void xifbGuardCount()
{
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    unsigned char n = (unsigned char)(xifbGuardStarts(o) + 1);
    if (OSData *d = OSData::withBytes(&n, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard: start %u without WindowServer so far (off after 2)", n); d->release(); }
    o->release();
}
static void xifbGuardClear()
{
    static bool done; if (done) return; done = true;
    IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane); if (!o) return;
    // removeProperty did not reach NVRAM (N-283); a zero count does, the same way the count itself is written
    unsigned char z = 0;
    if (OSData *d = OSData::withBytes(&z, 1)) { if (o->setProperty(XIFB_GUARD_VAR, d)) XIFBLOG("boot guard cleared: still up 120 s after start"); d->release(); }
    o->release();
}

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; this build does not)
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxIntelFB, "0.8", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }


// The panel's EDID over the eDP AUX channel (AUX A, display version 14 layout), which the firmware left powered for
// its own modeset. Without it macOS sees "Unknown Display" and none of the panel's overrides (name, HiDPI sizes) apply.
#define XIFB_AUX_CTL  0x64010
#define XIFB_AUX_DATA 0x64014
static volatile uint32_t *gRegs;
static uint32_t xr(uint32_t o) { return gRegs[o / 4]; }
static void xw(uint32_t o, uint32_t v) { gRegs[o / 4] = v; }
static int xifbAux(const uint8_t *tx, int ntx, uint8_t *rx, int nrx)
{
    for (int i = 0; i < 100 && (xr(XIFB_AUX_CTL) & (1u << 31)); i++) IODelay(100);
    if (xr(XIFB_AUX_CTL) & (1u << 31)) return -1;
    for (int i = 0; i < ntx; i += 4) {
        uint32_t v = 0;
        for (int j = 0; j < 4 && i + j < ntx; j++) v |= (uint32_t)tx[i + j] << (24 - 8 * j);
        xw(XIFB_AUX_DATA + i, v);
    }
    // send busy, done, irq, timeout/receive error clears, max timeout, size, sync pulses 18 and 32, power request kept
    uint32_t ctl = (1u << 31) | (1u << 30) | (1u << 29) | (1u << 28) | (3u << 26) | (1u << 25) | ((uint32_t)ntx << 20) |
                   ((18u - 1) << 5) | (32u - 1) | (1u << 19);
    xw(XIFB_AUX_CTL, ctl);
    uint32_t st = 0;
    for (int i = 0; i < 200; i++) { st = xr(XIFB_AUX_CTL); if (!(st & (1u << 31))) break; IODelay(50); }
    xw(XIFB_AUX_CTL, st | (1u << 30) | (1u << 28) | (1u << 25));
    if ((st & (1u << 31)) || (st & ((1u << 28) | (1u << 25)))) return -1;
    int n = (st >> 20) & 0x1f; if (n > nrx) n = nrx;
    for (int i = 0; i < n; i++) rx[i] = (uint8_t)(xr(XIFB_AUX_DATA + (i & ~3)) >> (24 - 8 * (i & 3)));
    return n;
}
// one I2C-over-AUX transfer to the EDID address 0x50, retried while the sink defers
static int xifbI2c(uint8_t req, const uint8_t *data, int nd, uint8_t *rx, int nrx)
{
    uint8_t tx[20] = { (uint8_t)(req << 4), 0, 0x50, (uint8_t)(nd ? nd - 1 : (nrx > 1 ? nrx - 2 : 0)) };
    int ntx = (nd || nrx > 1) ? 4 : 3;
    for (int i = 0; i < nd; i++) tx[ntx++] = data[i];
    for (int t = 0; t < 7; t++) {
        int n = xifbAux(tx, ntx, rx, nrx);
        if (n >= 1 && !(rx[0] & 0xf0)) return n;
        IODelay(500);
    }
    return -1;
}
static bool xifbReadEdid(uint8_t *edid, int *len)
{
    uint8_t off = 0, rx[17];
    if (xifbI2c(0x4, &off, 1, rx, 1) < 1) return false;    // write, middle of transaction: offset 0
    int want = 128;
    for (int pos = 0; pos < want; pos += 16) {
        if (xifbI2c(0x5, nullptr, 0, rx, 17) != 17) return false;
        bcopy(rx + 1, edid + pos, 16);
        if (pos == 112) want = edid[126] ? 256 : 128;       // base block plus at most one extension (no segment pointer)
    }
    xifbI2c(0x1, nullptr, 0, rx, 1);                        // stop
    static const uint8_t hdr[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
    if (memcmp(edid, hdr, 8)) return false;
    for (int b = 0; b < want; b += 128) { uint8_t c = 0; for (int i = 0; i < 128; i++) c += edid[b + i]; if (c) return false; }
    *len = want;
    return true;
}
// pipe A's frame counter over about half a second: the refresh rate the firmware's mode runs at, in 16.16
static uint32_t xifbMeasureHz()
{
    const uint32_t fc = 0x70040;
    uint32_t a = xr(fc), b = a;
    for (int i = 0; i < 100 && (b = xr(fc)) == a; i++) IODelay(500);
    if (b == a) return 0;
    uint64_t t0 = mach_absolute_time(); IOSleep(500);
    uint32_t c = xr(fc); for (int i = 0; i < 100 && xr(fc) == c; i++) IODelay(100);
    uint64_t t1 = mach_absolute_time(), ns; absolutetime_to_nanoseconds(t1 - t0, &ns);
    uint32_t frames = xr(fc) - b;
    return ns ? (uint32_t)(((uint64_t)frames << 16) * 1000000000ull / ns) : 0;
}
// the EDID detailed timing nearest the measured rate, so the reported refresh is the panel's exact one
static uint32_t xifbSnapHz(const uint8_t *e, int len, uint32_t meas)
{
    uint32_t best = meas, bd = ~0u;
    auto dtd = [&](const uint8_t *d) {
        uint32_t clk = (d[0] | d[1] << 8) * 10000u; if (!clk) return;
        uint32_t ht = d[2] | (d[4] & 0xf0) << 4, hb = d[3] | (d[4] & 0x0f) << 8;
        uint32_t vt = d[5] | (d[7] & 0xf0) << 4, vb = d[6] | (d[7] & 0x0f) << 8;
        uint64_t tot = (uint64_t)(ht + hb) * (vt + vb); if (!tot) return;
        uint32_t hz = (uint32_t)(((uint64_t)clk << 16) / tot), d0 = hz > meas ? hz - meas : meas - hz;
        if (d0 < bd) { bd = d0; best = hz; }
    };
    for (int i = 0; i < 4; i++) dtd(e + 54 + 18 * i);
    if (len >= 256 && e[128] == 0x02) for (int o = e[130]; o >= 4 && o + 18 <= 127; o += 18) dtd(e + 128 + o);
    return (meas && bd > meas / 50) ? meas : best;    // more than 2 % off: keep the measurement
}

// ---- A-55: a quarter-size desktop that the display pipe stretches back to the panel ------------------
// The panel keeps its own timing (3120x2080 @ 120 Hz); what changes is how many pixels CoreAnimation
// composites.  Offering a mode whose nominal size is 1560x1040 *and whose backing is 1560x1040* makes
// WindowServer render a quarter of the pixels, and the pipe's scaler doubles them on the way out.
// Stage A only reinterprets the scanout buffer the plane already reads (PLANE_SIZE / PLANE_STRIDE); the
// scaler is a separate step, so a failure of the first can never leave a corrupt panel behind.
#define XIFB_1X_MODE_ID   ((IODisplayModeID)0x58470101)
#define XIFB_1X_W         1560
#define XIFB_1X_H         1040
#define XIFB_1X_ROW       6400      /* 1560*4 = 6240, rounded up to the 256 B IOSurface row alignment */

#define XIFB_PLANE_STRIDE_A  0x70188
#define XIFB_PLANE_SIZE_A    0x70190
#define XIFB_TRANS_PIPESRC_A 0x6001C
#define XIFB_PS_CTRL_1A      0x68180
#define XIFB_PS_WIN_POS_1A   0x68170
#define XIFB_PS_WIN_SZ_1A    0x68174
#define XIFB_PS_VPHASE_1A    0x68188
#define XIFB_PS_HPHASE_1A    0x68194

class XeGfxIntelFramebuffer;
static XeGfxIntelFramebuffer *gScale1xFB = nullptr;     // the single instance, for the sysctl below

class XeGfxIntelFramebuffer : public IONDRVFramebuffer {
    OSDeclareDefaultStructors(XeGfxIntelFramebuffer)
    uint8_t fEdid[256]; int fEdidLen = 0; uint32_t fHz16 = 0;
    // ---- A-55 state -------------------------------------------------------------------------------
    int  fScale1x = 2;                  // 0 off, 1 quarter desktop, 2 quarter desktop + pipe scaler
    IODisplayModeID fCurrentMode = 0;   // the mode we last selected (0 = whatever the base class drives)
    IODisplayModeID fBaseMode = 0;      // the firmware's mode, as the base class numbers it
    IOMemoryMap *fMmioMap = nullptr;
    volatile uint32_t *fReg = nullptr;  // BAR0, held for the lifetime of the driver (plane writes later)
    bool fPlaneSaved = false;
    uint32_t fSavedStride = 0, fSavedSize = 0, fSavedPipeSrc = 0, fSavedPs = 0;
    // ---- the raster we are actually driving ------------------------------------------------------------
    enum { T_PCLK, T_HVIS, T_HSS, T_HSE, T_HTOT, T_VVIS, T_VSS, T_VSE, T_VTOT, T_HZ_MHZ, T_HPOS, T_VPOS, T_COUNT };
    uint32_t fT[T_COUNT] = {};
    bool fTValid = false;
    uint32_t fDimSurf = 0;
    // the raster we publish to the accelerator; PLANE_SIZE gives it, PLANE_STRIDE (64 B units) the pitch
    uint32_t fW = 0, fH = 0, fRowBytes = 0;

    // The firmware leaves us a raster with no timing description, and display 14 does not decode the transcoder
    // registers the way the older generations do (HTOTAL reads back as a value that cannot be the active width).
    // So: geometry from PLANE_SIZE, totals from whichever transcoder agrees with it, and a CVT-RB shape for the
    // cadence the frame counter shows when none does.  Nothing here programs a display register.
    bool xifbMeasureTiming(IOPCIDevice *pci) {
        if (!pci) return false;
        IOMemoryDescriptor *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        IOMemoryMap *map = bar ? bar->map(kIOMapInhibitCache) : nullptr;
        if (!map || map->getLength() < 0x71000) { if (map) map->release(); XIFBLOG("panel timing: BAR0 unavailable"); return false; }
        volatile uint32_t *r = (volatile uint32_t *)map->getVirtualAddress();
        const uint32_t conf = r[0x70008 / 4], plane = r[0x70180 / 4], size = r[0x70190 / 4];
        fDimSurf = r[0x7019c / 4];
        const uint32_t w = (size & 0xffff) + 1, h = (size >> 16) + 1;
        // exactly what the plane is scanning: width/height from PLANE_SIZE, pitch from PLANE_STRIDE (64 B)
        fW = w; fH = h; fRowBytes = (r[0x70188 / 4] & 0xffff) * 64u;
        if (!fRowBytes) fRowBytes = w * 4;
        XIFBLOG("scanout geometry for accel: %ux%u rowBytes %u", fW, fH, fRowBytes);
        const uint32_t ddi = r[0x60400 / 4];
        uint32_t htot = 0, vtot = 0, hss = 0, hse = 0, vss = 0, vse = 0, hpos = 0, vpos = 0, tob = 0;
        for (unsigned k = 0; k < 4 && !tob && w && h; k++) {
            const uint32_t c = 0x60000 + k * 0x1000;
            if (((r[c / 4] & 0xffff) + 1) != w || ((r[(c + 0x0c) / 4] & 0xffff) + 1) != h) continue;
            const uint32_t ht = r[c / 4], hs = r[(c + 0x08) / 4], vt = r[(c + 0x0c) / 4], vs = r[(c + 0x14) / 4];
            htot = (ht >> 16) + 1; hss = (hs & 0xffff) + 1; hse = (hs >> 16) + 1;
            vtot = (vt >> 16) + 1; vss = (vs & 0xffff) + 1; vse = (vs >> 16) + 1;
            hpos = (ddi >> 16) & 1; vpos = (ddi >> 17) & 1;
            if (htot <= w || vtot <= h || hss < w || hse <= hss || hse > htot ||
                vss < h || vse <= vss || vse > vtot) continue;
            tob = c;
        }
        if (!tob) {     // synthetic CVT-RB shape: only the cadence and the raster size are measured
            htot = w + 160; hss = w + 48; hse = w + 80;
            vtot = h + 25;  vss = h + 3;  vse = h + 8;
        }
        uint64_t t0 = 0, t1 = 0, ns0 = 0, ns1 = 0;
        clock_get_uptime(&t0);
        const uint32_t f0 = r[0x70040 / 4];
        IOSleep(500);
        const uint32_t f1 = r[0x70040 / 4];
        clock_get_uptime(&t1);
        absolutetime_to_nanoseconds(t0, &ns0);
        absolutetime_to_nanoseconds(t1, &ns1);
        map->release();
        const uint32_t frames = f1 - f0;
        const uint64_t elapsed = ns1 - ns0;
        if (!w || !h || !frames || !elapsed) { XIFBLOG("panel timing: %ux%u, %u frames in %llu ns -- rejected",
                                                       w, h, frames, (unsigned long long)elapsed); return false; }
        const uint64_t hz_mhz = (uint64_t)frames * 1000000000000ull / elapsed;
        const uint64_t pclk = hz_mhz * (uint64_t)htot * (uint64_t)vtot / 1000;
        if (pclk > 0xffffffffull) return false;
        fT[T_PCLK] = (uint32_t)pclk; fT[T_HVIS] = w; fT[T_HSS] = hss; fT[T_HSE] = hse; fT[T_HTOT] = htot;
        fT[T_VVIS] = h; fT[T_VSS] = vss; fT[T_VSE] = vse; fT[T_VTOT] = vtot;
        fT[T_HZ_MHZ] = (uint32_t)hz_mhz; fT[T_HPOS] = hpos; fT[T_VPOS] = vpos;
        fTValid = true;
        XIFBLOG("panel timing: %ux%u active, %u..%u / %u..%u sync, %ux%u total, %u kHz, %u.%03u Hz "
                "(transcoder %#x, conf %#x plane %#x surf %#x)",
                w, h, hss, hse, vss, vse, htot, vtot, fT[T_PCLK] / 1000, fT[T_HZ_MHZ] / 1000, fT[T_HZ_MHZ] % 1000,
                tob, conf, plane, fDimSurf);
        return true;
    }

    // IODetailedTimingInformationV2 carries the same facts in the shape CoreDisplay expects.
    void xifbFillDetailedTiming(IODetailedTimingInformationV2 &t) const {
        bzero(&t, sizeof t);
        t.pixelClock = fT[T_PCLK];
        // setVBLTiming() clears the pixel count when min != max, and with no pixel count the shared-memory VBL
        // delta is never written; keep them equal.
        t.minPixelClock = t.pixelClock;
        t.maxPixelClock = t.pixelClock;
        t.horizontalActive = fT[T_HVIS];
        t.horizontalBlanking = fT[T_HTOT] - fT[T_HVIS];
        t.horizontalSyncOffset = fT[T_HSS] - fT[T_HVIS];
        t.horizontalSyncPulseWidth = fT[T_HSE] - fT[T_HSS];
        t.verticalActive = fT[T_VVIS];
        t.verticalBlanking = fT[T_VTOT] - fT[T_VVIS];
        t.verticalSyncOffset = fT[T_VSS] - fT[T_VVIS];
        t.verticalSyncPulseWidth = fT[T_VSE] - fT[T_VSS];
        t.horizontalSyncConfig = fT[T_HPOS] ? kIOSyncPositivePolarity : 0;
        t.verticalSyncConfig = fT[T_VPOS] ? kIOSyncPositivePolarity : 0;
        t.numLinks = 1;
        t.pixelEncoding = 1;            // RGB, CTA-861
        t.bitsPerColorComponent = 1;    // 8 bits
        t.colorimetry = 1;              // sRGB
        t.dynamicRange = 1;             // SDR
        t.horizontalScaled = t.horizontalActive;
        t.verticalScaled = t.verticalActive;
    }

    // An empty IOFBDetailedTimings is what makes IOFBBuildModeList record IOFB0Hz=Yes, which zeroes
    // CGDisplayModeGetRefreshRate() and leaves CoreDisplay without a timing for the mode.
    void xifbPublishDetailedTimings() {
        int dt = 1;
        if (PE_parse_boot_argn("-xeintelfbdt", &dt, sizeof dt) && !dt) return;
        if (!fTValid) { XIFBLOG("IOFBDetailedTimings not published: no measured timing"); return; }
        IODetailedTimingInformationV2 dtv;
        xifbFillDetailedTiming(dtv);
        OSData *d = OSData::withBytes(&dtv, sizeof dtv);
        if (!d) return;
        OSArray *a = OSArray::withCapacity(1);
        if (!a) { d->release(); return; }
        a->setObject(d);
        const bool ok = setProperty("IOFBDetailedTimings", a);
        XIFBLOG("published IOFBDetailedTimings: %ux%u, %u kHz (ok %d)", fT[T_HVIS], fT[T_VVIS], fT[T_PCLK] / 1000, (int)ok);
        a->release();
        d->release();
    }

    bool xifbMapMmio(IOPCIDevice *pci) {
        if (fReg) return true;
        if (!pci) return false;
        IOMemoryDescriptor *bar = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        fMmioMap = bar ? bar->map(kIOMapInhibitCache) : nullptr;
        if (!fMmioMap || fMmioMap->getLength() < 0x71000) {
            if (fMmioMap) { fMmioMap->release(); fMmioMap = nullptr; }
            XIFBLOG("scale1x: BAR0 unavailable, the quarter mode stays off");
            return false;
        }
        fReg = (volatile uint32_t *)fMmioMap->getVirtualAddress();
        return true;
    }
    bool xifbEnsureBaseMode() {
        if (fBaseMode) return true;
        const IOItemCount n = IONDRVFramebuffer::getDisplayModeCount();
        if (n < 1 || n > 16) return false;
        IODisplayModeID ids[16] = {};
        if (IONDRVFramebuffer::getDisplayModes(ids) != kIOReturnSuccess) return false;
        fBaseMode = ids[0];
        return true;
    }
    bool xifbScale1xOffered() { return fScale1x > 0 && fW && fH && fRowBytes && fReg && xifbEnsureBaseMode(); }
    // A-58: CoreDisplay kept only the base mode when the smaller backing was offered as an extra one, so
    // the base mode itself is the one that answers 1560x1040 -- same raster, smaller picture, scaled up by
    // the pipe.  Everything else still goes to the base class.
    bool xifbIs1x(IODisplayModeID mode) { (void)mode; return xifbScale1xOffered(); }

public:
    // Stage A reinterprets the buffer the plane already scans: same base address, 1560x1040 of pixels at
    // a 6400 byte row pitch.  Stage B hands that smaller picture to the pipe scaler and asks it for the
    // panel's own window; the register order and values follow i915's skl_pfit_enable().
    void xifbApplyScale1x(int level) {
        if (!fReg || !fW || !fH) return;
        if (!fPlaneSaved) {
            fSavedStride  = fReg[XIFB_PLANE_STRIDE_A / 4];
            fSavedSize    = fReg[XIFB_PLANE_SIZE_A / 4];
            fSavedPipeSrc = fReg[XIFB_TRANS_PIPESRC_A / 4];
            fSavedPs      = fReg[XIFB_PS_CTRL_1A / 4];
            fPlaneSaved = true;
        }
        if (level <= 0) {
            fReg[XIFB_PS_CTRL_1A / 4] = fSavedPs & ~(1u << 31);                 /* PS_SCALER_EN off first */
            fReg[XIFB_TRANS_PIPESRC_A / 4] = fSavedPipeSrc;
            fReg[XIFB_PLANE_STRIDE_A / 4] = fSavedStride;
            fReg[XIFB_PLANE_SIZE_A / 4] = fSavedSize;
        } else {
            const uint32_t src = ((uint32_t)(XIFB_1X_H - 1) << 16) | (uint32_t)(XIFB_1X_W - 1);
            fReg[XIFB_PLANE_STRIDE_A / 4] = XIFB_1X_ROW / 64;                    /* stride before size */
            fReg[XIFB_PLANE_SIZE_A / 4] = src;
            if (level >= 2) {
                // 0x3000 is skl_scaler_calc_phase(1, 0.5): -0.5 + 0.5/2 -> 0x10000-0x4000, then >>2
                const uint32_t phase = 0x3000u;
                fReg[XIFB_PS_CTRL_1A / 4] = fSavedPs & ~((7u << 25) | (1u << 31));  /* BINDING = PIPE, EN off */
                fReg[XIFB_PS_WIN_POS_1A / 4] = 0;
                fReg[XIFB_PS_WIN_SZ_1A / 4]  = ((uint32_t)(fH - 1) << 16) | (uint32_t)(fW - 1);
                fReg[XIFB_PS_VPHASE_1A / 4]  = phase;
                fReg[XIFB_PS_HPHASE_1A / 4]  = phase;
                fReg[XIFB_TRANS_PIPESRC_A / 4] = src;
                fReg[XIFB_PS_CTRL_1A / 4] = (fSavedPs & ~(7u << 25)) | (1u << 31);
            } else {
                fReg[XIFB_PS_CTRL_1A / 4] = fSavedPs & ~(1u << 31);
            }
        }
        (void)fReg[XIFB_PLANE_SIZE_A / 4];
        XIFBLOG("scale1x %d: plane %ux%u, stride %u, pipesrc %#x, win %ux%u, ps_ctrl %#x", level,
                (unsigned)((fReg[XIFB_PLANE_SIZE_A / 4] & 0xffff) + 1),
                (unsigned)((fReg[XIFB_PLANE_SIZE_A / 4] >> 16) + 1),
                (unsigned)fReg[XIFB_PLANE_STRIDE_A / 4], (unsigned)fReg[XIFB_TRANS_PIPESRC_A / 4],
                (unsigned)fW, (unsigned)fH, (unsigned)fReg[XIFB_PS_CTRL_1A / 4]);
    }
    void xifbSetScale1x(int level) { fScale1x = level; xifbApplyScale1x(level); }

    IOService *probe(IOService *provider, SInt32 *score) override {
        int off = 0;
        if (PE_parse_boot_argn("-xeintelfboff", &off, sizeof off)) { XIFBLOG("off by boot-arg"); return nullptr; }
        if (xifbGuardTripped()) { XIFBLOG("boot guard tripped (two starts without WindowServer): declining"); return nullptr; }
        return IONDRVFramebuffer::probe(provider, score);
    }
    bool start(IOService *provider) override {
        xifbGuardCount();
        // CoreDisplay ties a framebuffer to a GPU's Metal device through these two, and reads them while
        // IONDRVFramebuffer::start() builds the display connection, so they must already be there. The ID is unique per
        // GPU: NVRM owns 0x4E56524D..01 with indexes 0-3, so 'XEGF' 1 with index 0 is this GPU's single head. (Borrowing
        // NVRM's ID with index 4 put this framebuffer into NVIDIA's head group and panicked the boot.)
        setProperty("IOFBDependentID", (unsigned long long)0x5845474600000001ull, 64);
        setProperty("IOFBDependentIndex", (unsigned long long)0, 32);
        // System Profiler names the GPU from the PCI device's "model"; without it About This Mac shows a bare "Display"
        if (!provider->getProperty("model")) {
            IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
            const char *model = xifbModelName(pci ? pci->configRead16(kIOPCIConfigDeviceID) : 0);
            if (OSData *m = OSData::withBytes(model, (unsigned)strlen(model) + 1)) { provider->setProperty("model", m); m->release(); }
        }
        // System Profiler shows VRAM,totalMB of an Intel PCI device as "VRAM (Dynamic, Max)", else the 15 MB GOP
        // framebuffer. Same size ANV gives its heap: 3/4 of RAM above 4 GB, half below. xeintelfbvram=<MB> overrides.
        if (!provider->getProperty("VRAM,totalMB")) {
            uint32_t mb = 0;
            if (!PE_parse_boot_argn("xeintelfbvram", &mb, sizeof mb)) {
                uint64_t ram = 0; size_t len = sizeof ram;
                if (!sysctlbyname("hw.memsize", &ram, &len, nullptr, 0) && ram)
                    mb = (uint32_t)((ram <= (4ull << 30) ? ram / 2 : ram / 4 * 3) >> 20);
            }
            if (mb) { provider->setProperty("VRAM,totalMB", (unsigned long long)mb, 32); XIFBLOG("VRAM,totalMB %u", mb); }
        }
        // the guard is for boots that hang: still running 120 s after start clears it. IOGraphics keeps the
        // WindowServer-active attribute and WindowServer's connection skips newUserClient here (N-282 to N-284)
        static thread_call_t tc;
        if (!tc) tc = thread_call_allocate([](thread_call_param_t, thread_call_param_t) { xifbGuardClear(); }, nullptr);
        if (tc) { uint64_t dl; clock_interval_to_deadline(120, kSecondScale, &dl); thread_call_enter_delayed(tc, dl); }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            int off = 0;
            if (!PE_parse_boot_argn("-xeintelfbnoedid", &off, sizeof off))
                if (IOMemoryMap *m = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)) {
                    gRegs = (volatile uint32_t *)m->getVirtualAddress();
                    if (xifbReadEdid(fEdid, &fEdidLen))
                        XIFBLOG("EDID %d B, maker %02x%02x product %02x%02x, %ux%u mm", fEdidLen, fEdid[8], fEdid[9], fEdid[11],
                                fEdid[10], fEdid[21] * 10, fEdid[22] * 10);
                    else { fEdidLen = 0; XIFBLOG("EDID: AUX A read failed, the display stays unnamed"); }
                    uint32_t meas = xifbMeasureHz();
                    fHz16 = fEdidLen ? xifbSnapHz(fEdid, fEdidLen, meas) : meas;
                    XIFBLOG("refresh measured %u.%02u Hz, reported %u.%02u Hz", meas >> 16, ((meas & 0xffff) * 100) >> 16,
                            fHz16 >> 16, ((fHz16 & 0xffff) * 100) >> 16);
                    gRegs = nullptr; m->release();
                }
        }
        // A-55: -xeintelfb1x=<0|1|2> picks the stage, -xeintelfb1xoff (bare) always returns to the panel raster
        {
            int lvl = 1;
            if (PE_parse_boot_argn("-xeintelfb1x", &lvl, sizeof lvl)) fScale1x = lvl;
            int off1x = 1;
            if (PE_parse_boot_argn("-xeintelfb1xoff", &off1x, sizeof off1x)) fScale1x = 0;
            if (fScale1x < 0) fScale1x = 0;
            if (fScale1x > 2) fScale1x = 2;
        }
        xifbMapMmio(OSDynamicCast(IOPCIDevice, provider));
        gScale1xFB = this;
        // A kext's static sysctls are not in the kernel's own link set, so the oid has to be registered by hand
        // (the accelerator does the same for debug.xegfxaccel_*).  Without this the valve simply does not exist.
        {
            static bool sctRegistered;
            if (!sctRegistered) {
                sctRegistered = true;
                extern struct sysctl_oid sysctl__debug_xegfxintelfb_scale1x;
                sysctl_register_oid(&sysctl__debug_xegfxintelfb_scale1x);
            }
        }
        XIFBLOG("scale1x: stage %d, mmio %s, -xeintelfb1xoff disables", fScale1x, fReg ? "mapped" : "unavailable");
        // the firmware mode carries no timing at all: measure the live raster and publish IOFBDetailedTimings
        // before the base class builds its mode list, because CoreDisplay only fills a mode's timing when that
        // list is non-empty (0003/0010)
        xifbMeasureTiming(OSDynamicCast(IOPCIDevice, provider));
        xifbPublishDetailedTimings();
        const bool started = IONDRVFramebuffer::start(provider);
        if (started) xifbPublishDetailedTimings();      // start() rewrites IOFBConfig, so publish again
        // A-58: only now does the mode list exist, and only now is the raster measured -- so this is the first
        // moment the plane can follow the 1560x1040 backing this framebuffer reports.  Doing it earlier found
        // xifbScale1xOffered() false and left the plane on the 3120x2080 raster while the compositor was told
        // something else.
        if (started && xifbScale1xOffered()) {
            XIFBLOG("1x: base mode %#x answers %ux%u, taking the plane and the scaler", (unsigned)fBaseMode, XIFB_1X_W, XIFB_1X_H);
            xifbApplyScale1x(fScale1x);
        }
        // the accelerator registering this framebuffer's display pipe delivers a mode change, which asks IOFramebuffer
        // for attributes; asked before start() returned, that panicked.  Nothing may touch this framebuffer before the
        // base class is done, so the readiness flag is published only now (ArrowWhatever 0014-intelfb-l0-ready).
        if (started && setProperty("XeGfxL0Ready", kOSBooleanTrue))
            XIFBLOG("start complete: XeGfxL0Ready published -- the accelerator may register this framebuffer now");
        return started;
    }
    bool hasDDCConnect(IOIndex ci) override { return fEdidLen ? true : IONDRVFramebuffer::hasDDCConnect(ci); }
    IOReturn getDDCBlock(IOIndex ci, UInt32 block, IOSelect type, IOOptionBits opt, UInt8 *data, IOByteCount *len) override {
        if (!fEdidLen) return IONDRVFramebuffer::getDDCBlock(ci, block, type, opt, data, len);
        if (!data || !len || block < 1 || (int)(block * 128) > fEdidLen) return kIOReturnUnsupported;
        IOByteCount n = *len < 128 ? *len : 128; bcopy(fEdid + (block - 1) * 128, data, n); *len = n;
        return kIOReturnSuccess;
    }
    IOReturn getAttributeForConnection(IOIndex ci, IOSelect attr, uintptr_t *value) override {
        if (fEdidLen && attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;
        return IONDRVFramebuffer::getAttributeForConnection(ci, attr, value);
    }
    // The accelerator asks these while wiring the display pipe; IONDRVFramebuffer would forward them to an NDRV
    // instance this machine does not have, so answer from the raster we measured. The mode ID must be the one the base
    // class published (100 here): CoreDisplay uses it as an index into its mode list and aborts on an unknown ID.
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    {
        if (!mode || !depth) return kIOReturnBadArgument;
        if (fCurrentMode == XIFB_1X_MODE_ID) { *mode = XIFB_1X_MODE_ID; *depth = 0; return kIOReturnSuccess; }
        if (kIOReturnSuccess == IONDRVFramebuffer::getCurrentDisplayMode(mode, depth)) return kIOReturnSuccess;
        // no NDRV instance answers: the first mode of the list we published is the firmware's raster
        IODisplayModeID m = 0;
        if (getDisplayModeCount() && kIOReturnSuccess == getDisplayModes(&m)) { *mode = m; *depth = 0; return kIOReturnSuccess; }
        XIFBLOG("getCurrentDisplayMode: no mode list to answer from");
        return kIOReturnUnsupported;
    }

    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    { return getCurrentDisplayMode(mode, depth); }

    // A-55: the mode list gets one more entry -- a desktop of 1560x1040 backed by 1560x1040 pixels.  The
    // panel raster is untouched, so this is a scaled mode in CoreDisplay's sense and no modeset happens.
    IOItemCount getDisplayModeCount(void) override {
        const IOItemCount n = IONDRVFramebuffer::getDisplayModeCount();
        static unsigned logged;
        if (logged++ < 3) XIFBLOG("getDisplayModeCount: %u (the base mode itself carries the 1x backing)", (unsigned)n);
        return n;
    }
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override {
        const IOItemCount n = IONDRVFramebuffer::getDisplayModeCount();
        const IOReturn r = IONDRVFramebuffer::getDisplayModes(allDisplayModes);
        (void)n;
        return r;
    }

    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *pi) override
    {
        if (!pi || aperture != kIOFBSystemAperture) return kIOReturnUnsupportedMode;
        if (!fW || !fH || !fRowBytes) return kIOReturnUnsupported;      // not measured: refuse honestly
        uint32_t w = fW, h = fH, row = fRowBytes;
        {
            static unsigned probe;
            if (probe++ < 3) XIFBLOG("1x probe: mode %#x base %#x level %d w %u h %u row %u reg %d -> %s", (unsigned)mode,
                                     (unsigned)fBaseMode, fScale1x, fW, fH, fRowBytes, fReg ? 1 : 0,
                                     xifbIs1x(mode) ? "1560x1040" : "raster");
        }
        if (mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) { w = XIFB_1X_W; h = XIFB_1X_H; row = XIFB_1X_ROW; }
        else {
            IODisplayModeInformation mi = {};
            if (kIOReturnSuccess != IONDRVFramebuffer::getInformationForDisplayMode(mode, &mi)) return kIOReturnUnsupportedMode;
        }
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = row; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32;
        pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = w; pi->activeHeight = h;
        static unsigned logged;
        if (logged++ < 4) XIFBLOG("scanout described: mode %#x depth %u %ux%u rowBytes %u",
                                  (unsigned)mode, (unsigned)depth, w, h, row);
        return kIOReturnSuccess;
    }
    // CoreDisplay only builds a mode with a detailed timing when this returns one (IOFBBuildModeList reads its mode
    // list from IOFBConfig and leaves IOFB0Hz set when IOFBDetailedTimings is empty).
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
        if (!info) return kIOReturnBadArgument;
        if (!fTValid) return IONDRVFramebuffer::getTimingInfoForDisplayMode(mode, info);
        if (mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) {
            // same raster timing, but the picture that fills it is 1560x1040 -- kIOScalingInfoValid is what
            // tells CoreDisplay the scaled size differs from the active one
            xifbFillDetailedTiming(info->detailedInfo.v2);
            // A-59: the direction of these two matters.  horizontalActive is what the pipe takes from the
            // framebuffer -- 1560 here, so CoreDisplay sizes its backing at 1560 and composites there -- and
            // horizontalScaled is what the panel ends up showing, which is its own 3120.  With them the other
            // way round CoreDisplay kept compositing at 3120 while the plane read 1560, which is exactly the
            // torn, half-black picture A-58 produced.  The totals stay the panel's, so the blanking shrinks to
            // keep htotal/vtotal where the panel expects them.
            info->detailedInfo.v2.horizontalActive = XIFB_1X_W;
            info->detailedInfo.v2.horizontalBlanking = fT[T_HTOT] - XIFB_1X_W;
            info->detailedInfo.v2.horizontalScaled = fT[T_HVIS];
            info->detailedInfo.v2.verticalActive = XIFB_1X_H;
            info->detailedInfo.v2.verticalBlanking = fT[T_VTOT] - XIFB_1X_H;
            info->detailedInfo.v2.verticalScaled = fT[T_VVIS];
            info->flags |= kIODetailedTimingValid | kIOScalingInfoValid;
            info->appleTimingID = (IOAppleTimingID)kIOTimingIDInvalid;
            static unsigned l1;
            if (l1++ < 3) XIFBLOG("quarter mode %#x: %ux%u scaled to %ux%u, %u kHz", (unsigned)mode,
                                  XIFB_1X_W, XIFB_1X_H, fT[T_HVIS], fT[T_VVIS], fT[T_PCLK] / 1000);
            return kIOReturnSuccess;
        }
        IODisplayModeInformation mi = {};
        if (kIOReturnSuccess != IONDRVFramebuffer::getInformationForDisplayMode(mode, &mi) ||
            (uint32_t)mi.nominalWidth != fT[T_HVIS] || (uint32_t)mi.nominalHeight != fT[T_VVIS])
            return IONDRVFramebuffer::getTimingInfoForDisplayMode(mode, info);
        xifbFillDetailedTiming(info->detailedInfo.v2);
        info->flags |= kIODetailedTimingValid;
        info->flags &= ~kIOScalingInfoValid;
        info->appleTimingID = (IOAppleTimingID)kIOTimingIDInvalid;
        static unsigned logged;
        if (logged++ < 3)
            XIFBLOG("detailed timing for mode %#x: %ux%u, %u kHz", (unsigned)mode, fT[T_HVIS], fT[T_VVIS], fT[T_PCLK] / 1000);
        return kIOReturnSuccess;
    }
    // we only describe the raster the firmware drives; a programmable mode would need a real modeset
    IOReturn validateDetailedTiming(void *description, IOByteCount length) override {
        static unsigned logged;
        if (logged++ < 2) XIFBLOG("validateDetailedTiming: %u bytes offered; not a modeset driver", (unsigned)length);
        return kIOReturnUnsupported;
    }

    // the panel's size and refresh, and ValidForHiRes so the 2x sizes reach Displays settings (I-059 on NVRMFB)
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
        if (!info) return kIOReturnBadArgument;
        if ((mode == XIFB_1X_MODE_ID || xifbIs1x(mode)) && xifbEnsureBaseMode()) {
            IODisplayModeInformation base = {};
            if (kIOReturnSuccess != IONDRVFramebuffer::getInformationForDisplayMode(fBaseMode, &base)) return kIOReturnUnsupportedMode;
            *info = base;
            info->nominalWidth = XIFB_1X_W; info->nominalHeight = XIFB_1X_H;
            if (fHz16) info->refreshRate = fHz16;
            // CoreDisplay builds the menu from IOFBConfig; a mode that keeps the panel raster but hands the
            // compositor a smaller backing still has to look like a first-class mode there, so mark it native
            // and high-res the way the base path does, and carry the panel physical size across with it.
            info->flags = (base.flags & kDisplayModeSafetyFlags) | kDisplayModeValidFlag | kDisplayModeSafeFlag
                        | kDisplayModeNativeFlag | kDisplayModeValidForHiResFlag;
            info->imageWidth = base.imageWidth; info->imageHeight = base.imageHeight;
            static unsigned l2;
            if (l2++ < 3) XIFBLOG("quarter mode %#x: %ux%u nominal, refresh %u.%03u Hz, flags %#x (base %#x)",
                                  (unsigned)mode, XIFB_1X_W, XIFB_1X_H, (unsigned)(info->refreshRate >> 16),
                                  (unsigned)(((info->refreshRate & 0xffff) * 1000) >> 16), (unsigned)info->flags,
                                  (unsigned)base.flags);
            return kIOReturnSuccess;
        }
        IOReturn r = IONDRVFramebuffer::getInformationForDisplayMode(mode, info);
        if (r != kIOReturnSuccess || !info) return r;
        if (fHz16) info->refreshRate = fHz16;
        if (fEdidLen && fEdid[21] && fEdid[22]) { info->imageWidth = fEdid[21] * 10; info->imageHeight = fEdid[22] * 10; }
        info->flags |= kDisplayModeValidForHiResFlag;
        return r;
    }

    // Selecting the quarter mode never touches the transcoder: the panel keeps scanning its own raster and
    // only the plane's idea of the picture changes.  Anything else goes to the base class and puts the
    // panel back the way the firmware left it.
    IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override {
        if ((displayMode == XIFB_1X_MODE_ID || xifbIs1x(displayMode)) && xifbScale1xOffered()) {
            fCurrentMode = displayMode;
            xifbApplyScale1x(fScale1x);
            XIFBLOG("mode %#x selected: %ux%u desktop at %u B rows, panel stays %ux%u", (unsigned)displayMode,
                    XIFB_1X_W, XIFB_1X_H, XIFB_1X_ROW, (unsigned)fW, (unsigned)fH);
            return kIOReturnSuccess;
        }
        const IOReturn r = IONDRVFramebuffer::setDisplayMode(displayMode, depth);
        if (r == kIOReturnSuccess) { fCurrentMode = displayMode; xifbApplyScale1x(0); }
        return r;
    }
    void stop(IOService *provider) override {
        xifbApplyScale1x(0);
        if (gScale1xFB == this) gScale1xFB = nullptr;
        if (fMmioMap) { fMmioMap->release(); fMmioMap = nullptr; fReg = nullptr; }
        IONDRVFramebuffer::stop(provider);
    }
};

OSDefineMetaClassAndStructors(XeGfxIntelFramebuffer, IONDRVFramebuffer)

// A-55 safety valve: the same three stages as -xeintelfb1x, but reachable over ssh while the panel is live,
// so a mistimed scaler is one command away from the firmware raster instead of one reboot away.  0 restores
// every register the driver saved before it first wrote.
static int gScale1xLevel = 1;
static int xifbScale1xSysctl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
    int v = gScale1xLevel;
    (void)arg1; (void)arg2;
    int e = sysctl_handle_int(oidp, &v, 0, req);
    if (e || !req->newptr) return e;
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    gScale1xLevel = v;
    if (gScale1xFB) gScale1xFB->xifbSetScale1x(v);
    XIFBLOG("scale1x: sysctl -> %d", v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfxintelfb_scale1x, CTLTYPE_INT | CTLFLAG_RW, 0, 0,
            xifbScale1xSysctl, "I", "0 = panel raster, 1 = quarter desktop, 2 = quarter desktop + pipe scaler");
