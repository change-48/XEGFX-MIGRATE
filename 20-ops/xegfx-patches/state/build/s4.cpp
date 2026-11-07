/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "../nvrm_off.h"
#ifdef NMX_ACCEL
// NMXAccel: this accelerator built for the AMD and Intel GPUs that NMXGPU drives (see nmx_accel_names.h)
#include "nmx_accel_names.h"
#include "xegfx_accel_names.h"
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#ifdef NMX_XEGFX
#include <IOKit/graphics/IOFramebuffer.h>   // the boot framebuffer's aperture and mode
#endif
#include <pexpert/pexpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"

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
#endif
#include "../nvrm_gpuva_abi.h"
class NVMemoryMap;
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               nvAccelGpuVaFree(IOAccelMemoryMap *map);
static bool               nvAccelFlipTo(IOAccelResource2 *res);
static bool               nvAccelCopyToScanout(IOAccelResource2 *res);
void                      nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
bool                      nvAccelResHasContent(IOAccelResource2 *res);
#ifdef NMX_XEGFX
bool nvAccelXeSwapCopy(IOAccelResource2 *source);
#endif
static void               nvAccelWriteStamp(int index, unsigned int value);
#ifdef NMX_ACCEL
#define NV_LOG_TAG "NMXAccel: "
#else
#define NV_LOG_TAG "NVAccel: "
#endif
#define ALOG(fmt, ...) do { kprintf(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); IOLog(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include "nvrm_vram_abi.h"
#include <IOKit/IODeviceMemory.h>
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include "nvaccel_stubs.h"
#include "nvkms-kapi.h"
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
extern "C" {   // kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void proc_selfname(char *buf, int size);
// M4a: a real kernel thread (kern/thread.h is reachable here; only thread_call.h clashes).  The boot sequence
// waits on the display stack with IOSleep, which a one-shot thread_call cannot do.
}
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static volatile uint32_t *gDispMmio = nullptr;   // BAR0 display registers, mapped in start() for the vblank wait
static void nvaccelInstallSysctl(void);
#ifdef NMX_XEGFX
extern SInt64 gCntPipeInit;
static void nvXeTraceStart(void);
#endif
static int gNvAccelArmed = 0;
static SInt64 gCntIopFlipHome, gCntIopLate;
// 1 once the headless check restored the plugin name without arming (no NVIDIA display head). The Metal plugin reads it:
// a headless GPU hands frames to another GPU's WindowServer through system memory, so IOSurfaces must be copied out.
static int gNvHeadless = 0;
#ifdef NMX_ACCEL
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = AMD/Intel Metal device for apps; frames are copied to the framebuffer WindowServer draws");
#else
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = no NVIDIA display: Metal device for apps only, WindowServer on the GPU that owns the display");
#endif
// 4 NVIDIA heads (nvfbheads=4) + 1 for an Intel built-in panel (NMIntelFB, IOFBDependentIndex 4): WindowServer composites
// that panel on the NVIDIA card and the display-pipe copy path below writes each frame into the Intel scan-out surface.
static const unsigned kNvMaxFB = 5;
static unsigned gNvHeadsPublished = 0, gNvHeadsRegistered = 0;
static unsigned gNvHeadsPiped = 0;
// The Intel built-in panel (head 4) joins only when this is 1. Off by default while the path is proven: a write at run
// time (then debug.xegfxaccelfb=3 and a WindowServer restart) can be undone by a reboot, so a hang never needs hands
// (the first virtual-panel test froze the machine at boot and needed a power cycle).
#ifdef NMX_XEGFX
static int gNvIgpu = 1;   // over XeGfx the Intel panel (NMIntelFB, head 4) is the only display this GPU has
#else
static int gNvIgpu = 0;
#endif
SYSCTL_INT(_debug, OID_AUTO, nvaccel_igpu, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvIgpu, 0,
           "1 = give an Intel built-in panel (NMIntelFB, head 4) a display pipe on this accelerator");
#ifdef NMX_XEGFX
// --- M4a: boot-armed auto-configuration (docs/19-M4A-ACCEL.md) ---------------------------------------
// gBootArmState: 0 idle, 1 a sequence is running, 2 ready, 3 failed.  gBootArmSteps: 1 igpu, 2 fb=1, 4 head
// registered, 8 fb=3, 16 pipe, 32 iop, 64 flip, 128 the AGDC request was left behind.  gBootArmReason: 1 boot
// argument, 2 IOResources property, 3 provider property, 4 debug.xegfxaccel_boot=1.
static int gBootArmState = 0;
static int gBootArmSteps = 0;
static int gBootArmReason = 0;
static int gBootArmDelayMs = 3000;      // AGDC and the display machine have their own early MMIO work to finish first
static int gBootArmHeadWaitMs = 30000;  // a panel that has not published in 30 s does not come up this boot
static int gBootArmPipeWaitMs = 25000;  // the family's sweep and the first mode change can be slow at boot
static int gBootArmBudgetMs = 90000;    // hard cap for the whole sequence, whatever the individual waits say
// --- M3d: the adapter <-> accelerator present contract (docs/20-M3D-ACCEL.md) ---------------------------------
// The KMS adapter (XeGfxDisplay.kext, XeGfxFramebuffer) owns the mode and the scanout allocation, but the plane
// registers are this accelerator's: core's KAPI keeps .flip == NULL (M3c), and xg_i915_flip.c's own header says
// "The accelerator (XeGfxAccel) writes PLANE_SURF_1_A itself; this file only owns the GGTT side".  So the M3d
// interface is two directions, both through callPlatformFunction:
//   A) accelerator -> framebuffer  "xgGetScanoutInfo"  (p1 = struct XeGfxScanoutInfo *): the adapter's KAPI
//      scanout block.  askFramebufferForScanout() imports it as the display-pipe resource, so the first composited
//      frame is a zero-copy flip of the adapter's own memory (docs/12 S4 item 4).  NotReady = the adapter has no
//      scanout yet: the pipe stays without a resource instead of being pointed at the boot aperture.
//   B) framebuffer -> accelerator  "xgFlipPresent" (p1 = struct XeGfxPresent *), "xgVblankCount" (p1 = uint64_t
//      *count, p2 = uint64_t *ns), "xgVblankTime" (p1 = uint64_t *ns, p2 = uint64_t *count), "xgGetPresentInfo"
//      (p1 = struct XeGfxPresentInfo *), "xgCursorImage" (p1 = struct XeGfxCursorImage *, validate only).
//      The adapter finds this object by the XeGfxAccelPresent property it publishes (see nvXePublishPresentAPI).
// Every struct below is a fixed 64-bit layout whose first two words are version and size, and both sides check
// them, so the ABI cannot drift silently.  Nothing here writes a display register unless the adapter calls
// xgFlipPresent, and that call returns kIOReturnNotReady until the adapter has published XeGfxFbReady.
#define NVXE_ABI_VERSION 2u
#define NVXE_FORMAT_XRGB8888 0x42475241u   // the adapter's format word ('ARGB' in the FB's byte order)
struct XeGfxScanoutInfo {                  // A: 80 bytes, version 2
    unsigned int version;                  // NVXE_ABI_VERSION
    unsigned int size;                     // sizeof (80)
    unsigned long long phys;               // host physical of the KAPI scanout (< 4 GiB)
    unsigned long long kva;                // kernel mapping of the same block (0 if none)
    unsigned int pitch;                    // bytes per row, 256-byte aligned by the KAPI allocator
    unsigned int width, height;            // active raster
    unsigned int format;                   // NVXE_FORMAT_XRGB8888
    unsigned int head;                     // 4 (this accelerator's panel head)
    unsigned int flags;                    // NVXE_SCANOUT_* below
    unsigned int activeScanout;            // adapter's index (0 when it has one scanout)
    unsigned int scanoutCount;             // how many scanouts the adapter has handed out
    unsigned long long vblankCount;        // adapter's own count (informational; accel answers xgVblankCount)
    unsigned long long vblankNs;           // adapter's own timestamp, ns
    unsigned int ggtt;                     // low 32 bits of the core GGTT offset of this block (0 = unknown)
    unsigned int reserved;
};
#define NVXE_SCANOUT_READY 0x1u            // allocated backing importable; plane writes use FbReady
#define NVXE_SCANOUT_GGTT  0x2u            // the ggtt word is usable (the adapter spells this bit ZEROCOPY); the accelerator only checks bit 0
struct XeGfxPresent {                      // B: 80 bytes, version 1|2 (identical layout; the adapter sends 2 since 2026-10-28, 1 stays accepted)
    unsigned int version;                  // 1 or NVXE_ABI_VERSION (2)
    unsigned int size;                     // sizeof (80)
    unsigned long long phys;               // frame physical (< 4 GiB) unless ggtt != 0
    unsigned long long kva;                // frame CPU mapping (informational; never read here)
    unsigned int ggtt;                     // GGTT offset of the frame (preferred: no PTE write), 0 = map phys
    unsigned int pitch, width, height;     // bytes per row (>= 4*width, 4-aligned) and raster
    unsigned int format;                   // NVXE_FORMAT_XRGB8888
    unsigned int head;                     // 4 (0 is accepted as 4: adapter header says "4 (0 => 4)")
    unsigned int regionX, regionY, regionW, regionH;  // 0 width/height = the whole frame
    unsigned long long flipCount;          // the adapter's sequence number (informational)
    unsigned int flags;                    // NVXE_PRESENT_* in, NVXE_PRESENT_RESULT_* out
    unsigned int reserved;
};
#define NVXE_PRESENT_WAIT_LATCH   0x1u     // (in) wait for the panel to latch; flags==0 also waits (default)
#define NVXE_PRESENT_NO_WAIT      0x2u     // (in) write and return immediately (interrupt-safe, no IOSleep); for version-1 requests bit 0 also means no-wait (the adapter's spelling)
#define NVXE_PRESENT_DONE         0x4u     // (out) PLANE_SURF_1_A now points at this frame
#define NVXE_PRESENT_LATCHED      0x8u     // (out) the panel latched it at a vblank
#define NVXE_PRESENT_LATCH_TIMEOUT 0x10u   // (out) written but no vblank was seen: the previous frame was put back
struct XeGfxPresentInfo {                  // B: 72 bytes, version 2 (diagnostics only)
    unsigned int version;
    unsigned int size;
    unsigned long long phys;               // last frame this accelerator programmed
    unsigned long long flipCount;          // how many presents this accelerator has programmed
    unsigned long long vblankCount;
    unsigned long long vblankNs;
    unsigned int pitch, width, height, format, head;
    unsigned int flags;                    // NVXE_PRESENT_RESULT_* plus NVXE_PRESENT_INFO_* below
    unsigned int planeSurfGgtt;            // live read-back of PLANE_SURF_1_A
    unsigned int reserved;
};
#define NVXE_PRESENT_INFO_READY 0x100u     // the present gate is open (adapter FB known and ready)
#define NVXE_PRESENT_INFO_BUSY  0x200u     // the K9b deferred flip worker is queued or in flight
struct XeGfxCursorImage {                  // B: 56 bytes, version 2 (skeleton: validated, never programmed)
    unsigned int version;
    unsigned int size;
    unsigned int width, height, format, flags;
    unsigned long long phys;
    unsigned int ggtt;
    unsigned int pitch;
    unsigned int hotspotX, hotspotY;
    unsigned int reserved;
};
static int gXePresent = 1;                 // debug.xegfxaccel_present: master switch for the B direction
static bool nvXePublishPresentAPI(IOService *fb, const char *why, IOService *accel);
static IOReturn nvXeKmsScanoutInfo(IOService *fb, struct NVRMVramRequest *r);
#endif

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#if defined(NMX_ACCEL) && defined(NMX_XEGFX)
// the same accelerator over XeGfx (Intel Xe, branch xe): its own bundle id, matched only after XeGfx claims the GPU
extern "C" { KMOD_EXPLICIT_DECL(org.watchthelight.XeGfxAccel, "0.1", _start, _stop) }
#elif defined(NMX_ACCEL)
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAccel, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVAccel, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class NVEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVEventMachine)
    NM_TAHOE_FWD(NVEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        nvAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(NVEventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
static int gNvScanout = 0;
static int gNvAperture = 0;
class NVDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(NVDisplayMachine)
    NM_TAHOE_FWD(NVDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NVDisplayMachine, IOAccelLegacyDisplayMachine)
#ifdef NMX_XEGFX
static volatile SInt64 gXeFakePhys = 0x400000000000ll;   // names of system-memory "VRAM", never real addresses
static IORangeAllocator *gXeVa;                          // GPU addresses the family wants; the Intel GPU never uses them
static IOLock *gXeVaLock;
#endif
#define NV_PRIVATE_VA_BASE  0x800000000000ull
#define NV_PRIVATE_VA_SIZE  0x000010000000ull
class NVTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVTask)
    NM_TAHOE_FWD(NVTask)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = nvAccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12)
            ALOG("NVTask::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        nvAccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(NVTask, IOAccelTask)
class NVMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVMemoryMap)
    NM_TAHOE_FWD(NVMemoryMap)
public:
    IOAccelMemory     *fMem       = nullptr;
    unsigned int       fHVirt     = 0;
    unsigned int       fHRmMemory = 0;
    unsigned long long fGpuVa     = 0;
    unsigned long long fVaLen     = 0;
    bool               fMapped    = false;
    unsigned int fCommits = 0;
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m,
              unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(NVMemoryMap, IOAccelMemoryMap)

#define NV_CARRIERS 32
class IOAccelNVRMCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelNVRMCarrier)
};
OSDefineMetaClassAndStructors(IOAccelNVRMCarrier, IOService)

#ifdef NMX_XEGFX
// WindowServer's sandbox lets it open /dev/dri read-only but never ioctl(2) a device node; it may open IOAccel*
// user clients. libdrm sends its ioctls here with the minor of its read-only fd, XeGfx checks the caller opened it.
class IOAccelXeGfxDRMClient : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelXeGfxDRMClient)
public:
    IOReturn clientClose() APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
    IOReturn externalMethod(uint32_t sel, IOExternalMethodArguments *a, IOExternalMethodDispatch *,
                            OSObject *, void *) APPLE_KEXT_OVERRIDE {
        static IOService *res;
        if (sel != 0 || a->scalarInputCount != 3 || a->scalarOutputCount != 1) return kIOReturnBadArgument;
        if (!res) { OSDictionary *m = IOService::serviceMatching("XeGfxResident"); if (m) res = waitForMatchingService(m, 1000ULL * 1000 * 1000); }
        if (!res) return kIOReturnNotAttached;
        int err = EIO;
        const OSSymbol *fn = OSSymbol::withCStringNoCopy("xgDrmIoctl");
        IOReturn r = res->callPlatformFunction(fn, false, (void *)(uintptr_t)a->scalarInput[0],
                                               (void *)(uintptr_t)a->scalarInput[1],
                                               (void *)(uintptr_t)a->scalarInput[2], &err);
        if (fn) fn->release();
        a->scalarOutput[0] = (uint64_t)err;
        return r;
    }
};
OSDefineMetaClassAndStructors(IOAccelXeGfxDRMClient, IOUserClient)

// The XeGfx display stack has two IOFramebuffer classes: today's placeholder XeGfxIntelFramebuffer (XeGfxIntelFB.kext)
// and the KMS adapter XeGfxFramebuffer (XeGfxDisplay.kext, the end state).  The adapter is a direct IOFramebuffer
// subclass, so neither an IONDRVFramebuffer match nor a class-name match on the old name ever sees it.  Both are ours;
// nothing else may become a head (an IOFramebuffer whose start never ran page-faults in getAttributeExt).
static bool xgIsXeGfxFB(IOService *s)
{
    return s && (s->metaCast("XeGfxIntelFramebuffer") || s->metaCast("XeGfxFramebuffer"));
}
// The class name for logs, so a log line says which display stack is up.
static const char *xgXeGfxFBClass(IOService *s)
{
    if (!s) return "?";
    if (s->metaCast("XeGfxIntelFramebuffer")) return "XeGfxIntelFramebuffer";
    if (s->metaCast("XeGfxFramebuffer")) return "XeGfxFramebuffer";
    return "not a XeGfx framebuffer";
}
// A head may only be handed to a framebuffer whose start() has returned (see fbReadyForRegistration for the panic).
// XeGfxIntelFB publishes XeGfxL0Ready after IONDRVFramebuffer::start() returned; the KMS adapter publishes
// XeGfxFbRegistered and XeGfxL0Ready after IOFramebuffer::start() returned.  Either property means "started";
// XeGfxL0Ready wins when both are present.  Returns the property that approved the head (nullptr = not ready), so
// callers and logs can name the gate.
static bool nvXePropTrue(IOService *s, const char *key);
static const char *xgXeGfxFBGate(IOService *s)
{
    if (!s) return nullptr;
    // The adapter can be visible while its scanout allocator is still running.
    // Its delayed L0 gate is published only after startS3 preparation completes;
    // FbRegistered alone must not admit an unfinished or inert adapter.
    if (s->metaCast("XeGfxFramebuffer"))
        return nvXePropTrue(s, "XeGfxL0Ready") ? "XeGfxL0Ready" : nullptr;
    if (nvXePropTrue(s, "XeGfxL0Ready")) return "XeGfxL0Ready";
    if (nvXePropTrue(s, "XeGfxFbRegistered")) return "XeGfxFbRegistered";
    return nullptr;
}
// M3d: is this the KMS adapter class (as opposed to the old IONDRVFramebuffer placeholder)?  Everything the
// adapter path adds is behind this test, so the placeholder path runs exactly the code it always ran.
static bool nvXeIsKmsFB(IOService *s) { return s && s->metaCast("XeGfxFramebuffer"); }
// Presence counts even when the value is not an OSBoolean: core re-publishes some gate properties as a bare key
// (XeGfxArm is set to gIOServiceKey, not to a boolean), and a false property -- not a missing one -- is what a
// gate must close on.  Only a missing property means "not published yet".
static bool nvXePropTrue(IOService *s, const char *key)
{
    if (!s) return false;
    OSObject *o = s->getProperty(key);
    if (!o) return false;
    OSBoolean *b = OSDynamicCast(OSBoolean, o);
    return b ? b->isTrue() : true;
}
// L0Ready permits pipe registration; only FbReady permits adapter plane writes.
// The first-present path explicitly publishes FbReady after its checks. Neither
// an allocated scanout nor a start/controller-ready marker grants that permission.
static bool nvXeFBReady(IOService *fb)
{
    return nvXeIsKmsFB(fb) && nvXePropTrue(fb, "XeGfxFbReady");
}
#endif

class NVAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccel)
    NM_TAHOE_FWD(NVAccel)
    IOAccelNVRMCarrier *fCarriers[NV_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { NVEventMachine *m = OSTypeAlloc(NVEventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        NVTask *t = OSTypeAlloc(NVTask); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 0u;   /* EXPERIMENT: was 15 (AccelCaps 0b1111). Claim nothing we cannot do; a wrong capability bit makes CoreDisplay take a path whose pixels end up as colour noise at the panel bottom. */
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x04380780u;   /* EXPERIMENT: was 0x40004000 (16384x16384); 1920x1080 */
        *(uint32_t *)(p + 0x34) = 0x00000780u;   /* EXPERIMENT: was 32768; 1920 */
        *(uint32_t *)(p + 0x38) = 0x00000438u;   /* EXPERIMENT: was 32768; 1080 */
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kNvMaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kNvMaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (NVRM.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        NVDisplayPipe *p = OSTypeAlloc(NVDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { NVDisplayMachine *d = OSTypeAlloc(NVDisplayMachine);
        fDM = d;
        ALOG("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { NVGLContext *c = OSTypeAlloc(NVGLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { NVCLContext *c = OSTypeAlloc(NVCLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { NVSurface *o = OSTypeAlloc(NVSurface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { NV2DContext *c = OSTypeAlloc(NV2DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { NVVideoContext *c = OSTypeAlloc(NVVideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { NVSysMemory *m = OSTypeAlloc(NVSysMemory); ALOG("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { NVVidMemory *m = OSTypeAlloc(NVVidMemory); ALOG("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { NVResource *r = OSTypeAlloc(NVResource); OSAddAtomic64(1, &gCntResNew); ALOG("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { NVMemoryMap *m = OSTypeAlloc(NVMemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    NVDisplayMachine *fDM = nullptr;
    IOPCIDevice *fPCI = nullptr;
    IONotifier *fFBNote = nullptr;
    IONotifier *fIntelNote = nullptr;
    IONotifier *fBootNote = nullptr;   // NMX_XEGFX: Apple's boot framebuffer on the iGPU
    IONotifier *fXeNote = nullptr;     // NMX_XEGFX: XeGfxIntelFB's IONDRVFramebuffer subclass (Intel class name)
    IONotifier *fKmsNote = nullptr;    // NMX_XEGFX: XeGfxDisplay's direct IOFramebuffer subclass XeGfxFramebuffer
    static NVAccel *gAccel;
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kNvMaxFB] = {};
#ifdef NMX_XEGFX
    // The KMS adapter's framebuffer and the placeholder both want head 4.  The adapter is parked here, never in
    // fFBs[]/fFB: registerPendingLocked hands head 4 to it only when it has published a start gate and the
    // placeholder has not, so an inert adapter cannot shadow the live placeholder.
    IOService *volatile fKmsFB = nullptr;
#endif
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    OSObject *fMetalPlugin = nullptr;
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !newService) return true;
        unsigned idx = 0;
        OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex"));
        if (num) idx = num->unsigned32BitValue();
#ifdef NMX_XEGFX
        // the Intel panel is head 4 here, the slot the NVIDIA build gives another GPU's panel (the copy path); a boot
        // framebuffer on any other device is not ours
        if (me->fPCI && newService->getProvider() != me->fPCI) {
            // The KMS adapter XeGfxFramebuffer is published under a nub (XeGfxDisplayNub), not directly on the PCI
            // device, so for that class look up the provider chain instead.  XeGfxIntelFramebuffer keeps the exact
            // one-hop check it has always had.
            bool ours = false;
            if (newService->metaCast("XeGfxFramebuffer")) {
                IOService *p = newService->getProvider();
                for (int hop = 0; p && hop < 8; hop++, p = p->getProvider())
                    if (p == me->fPCI) { ours = true; break; }
            }
            if (!ours) {
                ALOG("%s is on another device -- not ours", newService->getName());
                return true;
            }
        }
        // only the XeGfx display stack's own IOFramebuffer may become the head: the generic IONDRVFramebuffer
        // notifier also matches Apple's EFI framebuffer nubs, and registering one whose start never ran is what
        // crashed IOFramebuffer::getAttributeExt (getAttributeExt on a reserved that is still null).  The class check
        // plus the XeGfxL0Ready/XeGfxFbRegistered gate keep that pipe off this machine.
        if (!xgIsXeGfxFB(newService)) {
            ALOG("%s is not an XeGfxIntelFramebuffer -- not ours (registering it would be unsafe)", newService->getName());
            return true;
        }
        ALOG("%s: %s accepted as head 4", newService->getName(), xgXeGfxFBClass(newService));
        if (newService->metaCast("XeGfxFramebuffer")) {
            // The KMS adapter never takes fFBs[4]/fFB on arrival: the two display stacks coexist during the M3d
            // transition, and whichever service the kernel enumerates first would otherwise own head 4.  Park it and
            // let registerPendingLocked choose by start gate (the placeholder wins when both are open), so an inert
            // adapter cannot stop the live placeholder from being registered.
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fKmsFB)) {
                OSBitOrAtomic(1u << 4, &gNvHeadsPublished);
                ALOG("%s: %s parked as the alternative head 4 (the placeholder keeps the head while its gate is open)",
                     newService->getName(), xgXeGfxFBClass(newService));
                // M3d: make this accelerator findable by the adapter (it property-matches XeGfxAccelPresent on the
                // IOService plane and on NVAccel).  Published before head-4 registration on purpose: the adapter's
                // first xgFlipPresent must be able to reach us as soon as its own XeGfxFbReady is up.
                nvXePublishPresentAPI(newService, "adapter parked", me);
            } else {
                newService->release();
                ALOG("%s: %s DUPLICATE -- the first KMS framebuffer keeps the alternative slot",
                     newService->getName(), xgXeGfxFBClass(newService));
            }
            return true;
        }
        idx = 4;
#endif
        const char *what = "kept";
        if (idx >= kNvMaxFB) what = "OUT OF RANGE -- this head will get no pipe";
        else {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[idx])) OSBitOrAtomic(1u << idx, &gNvHeadsPublished);
            else { newService->release(); what = "DUPLICATE -- the first one keeps the slot"; }
        }
        newService->retain();
        if (idx == 0 && me->fFBs[0] == newService) {
            IOService *old;
            do { old = me->fFB; } while (!OSCompareAndSwapPtr(old, newService, (void *volatile *)&me->fFB));
            if (old == newService) newService->release();
            else if (old) {
                bool slotted = false;
                for (unsigned j = 0; j < kNvMaxFB; j++) if (me->fFBs[j] == old) slotted = true;
                ALOG("fFB: head 0 (%s) displaced %s, which published first (%s)", newService->getName(), old->getName(),
                     slotted ? "its extra reference returned" : "NOT slotted: keeping its reference rather than risk a reader");
                if (slotted) old->release();
            }
        // only head 0 may become the primary framebuffer: an Intel panel (index 4) publishing first became fFB (10-10 log:
        // "head 0 displaced NMIntelFB, which published first"), and on a laptop with nothing on the NVIDIA ports it stays
#ifdef NMX_XEGFX
        // over XeGfx there is no NVIDIA head 0: the Intel panel (head 4) is the primary framebuffer (N-243: register
        // and the copy path both stopped at "no framebuffer" with it only slotted)
        } else if (idx != 4 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
#else
        } else if (idx != 0 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
#endif
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s published -- display machine framebuffer count = %u (index %u %s)", newService->getName(), before, idx, what);
        if (gNvAccelArmed)
            ALOG("  index %u published AFTER the arm: it gets a pipe only when debug.xegfxaccelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", idx);
        else
            ALOG("  to register it:  sudo sysctl -w debug.xegfxaccelfb=1   (2 = re-run the family's own sweep)");
        return true;
    }
    // Headless GPU (laptops whose panel and outputs are all on the Intel or AMD GPU: issues #76, #106). NVKMS finds no
    // display, so no NVRMFB head ever publishes and the withheld MetalPluginName was never restored: Metal never offered
    // the card, although NVRM and NVK run it fine (#106: vkCreateInstance enumerates the GTX 1660 Ti). Apple's own
    // two-GPU MacBook Pros work this way: the integrated GPU drives every display and WindowServer, and the discrete GPU
    // is a second Metal device that apps choose and macOS copies their frames from. So when no head has published by the
    // deadline, the plugin name goes back on WITHOUT arming: apps get the NVIDIA device, and WindowServer (allow-listed
    // "!WindowServer" = only when armed) keeps the GPU that owns the display. Nothing here touches a display pipe or a
    // mode change. Boot-arg -nvaccelnoheadless keeps the old behaviour.
    static constexpr uint32_t kHeadlessDeadlineSec = 60;   // chosen: NVRMFB heads publish within ~20 s of start (#76 log)
    struct thread_call *fHeadlessCall = nullptr;
    void armHeadlessCheck() {
        int off = 0;
        if (PE_parse_boot_argn("-nvaccelnoheadless", &off, sizeof off)) { ALOG("headless check: off (-nvaccelnoheadless)"); return; }
        fHeadlessCall = thread_call_allocate(&NVAccel::headlessCheck, this);
        if (!fHeadlessCall) { ALOG("headless check: thread_call_allocate FAILED -- a GPU with no display stays hidden from Metal"); return; }
        uint64_t when = 0;
        clock_interval_to_deadline(kHeadlessDeadlineSec, kSecondScale, &when);
        thread_call_enter_delayed(fHeadlessCall, when);
        ALOG("headless check: armed for %u s from now", kHeadlessDeadlineSec);
    }
    static void headlessCheck(void *p0, void *) {
        NVAccel *me = (NVAccel *)p0;
        if (!me || me != gAccel) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const char *why = me->fFB ? "a display head published" : !me->fMetalPlugin ? "no plugin name to restore"
                        : me->getProperty("MetalPluginName") ? "the plugin name is already back" : nullptr;
        if (why) {
            ALOG("headless check: not headless (%s) -- nothing to do", why);
        } else {
            // name the GPU that owns the display, so a log says which driver WindowServer is on
            char owner[64] = "none found";
            if (OSIterator *it = getMatchingServices(serviceMatching("IOFramebuffer"))) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *s = OSDynamicCast(IOService, o);
                    if (s && !s->metaCast("NVRMNVDAFramebuffer")) { strlcpy(owner, s->getMetaClass()->getClassName(), sizeof owner); break; }
                }
                it->release();
            }
            me->setProperty("MetalPluginName", me->fMetalPlugin);
            me->setProperty("NVAccelHeadless", kOSBooleanTrue);
            gNvHeadless = 1;
            ALOG("headless check: no NVIDIA display head after %u s -- MetalPluginName restored WITHOUT arming: apps get "
                 "the NVIDIA Metal device, WindowServer stays on the display's GPU (%s)", kHeadlessDeadlineSec, owner);
            // Not a good start for the boot guard: a card whose connectors carry no display clears the guard in the
            // framebuffer's own enumeration (nvrm-fb.cpp), with the connector list as evidence. Reaching this deadline
            // without that means the framebuffer never enumerated at all, which is a failure the guard must keep counting.
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
    }
    // A framebuffer may only be given a display pipe after its start() has returned: the family asks the
    // framebuffer object for its attributes (IOFramebuffer::getAttributeExt) while delivering the mode change,
    // and a framebuffer whose start never ran has a null "reserved" pointer -> page fault in the kernel
    // (three panics on 2026-10-27: nvaccelfb_sysctl -> registerFramebufferLocked -> deliverModeChange ->
    // display_mode_did_change -> displayModeDidChange -> IOFramebuffer::getAttributeExt, CR2=0x120).
    static bool fbReadyForRegistration(IOService *fb, const char **why) {
        if (!fb) { *why = "null"; return false; }
#ifdef NMX_XEGFX
        // Two display stacks publish their framebuffers here: the placeholder XeGfxIntelFB (XeGfxIntelFramebuffer,
        // which publishes XeGfxL0Ready after IONDRVFramebuffer::start() returned) and the KMS adapter
        // XeGfxFramebuffer (a direct IOFramebuffer subclass, which publishes XeGfxFbRegistered and XeGfxL0Ready
        // after IOFramebuffer::start() returned).  Either property means "started"; XeGfxL0Ready wins when both are
        // present.  Neither implies the object is safe to ask earlier: the family asks it for attributes while
        // delivering a mode change, and doing so before start() returned is what panicked on 2026-10-27.
        if (!xgIsXeGfxFB(fb)) { *why = "not an XeGfxIntelFramebuffer"; return false; }
        if (!xgXeGfxFBGate(fb)) {
            *why = "neither XeGfxL0Ready nor XeGfxFbRegistered has been published by the FB start";
            return false;
        }
#endif
        *why = "ready";
        return true;
    }
    bool deliverModeChangeIfPipesReady(const char *whyTag) {
        if (!fDM) return false;
        uint8_t *dm = (uint8_t *)fDM;
        const uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("deliverModeChange(%s): implausible pipe count %u -- NOT delivering", whyTag, n); return false; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) continue;
            IOService *pfb = *(IOService *const volatile *)(p + 0x98);
            const char *ready = nullptr;
            if (!fbReadyForRegistration(pfb, &ready)) {
                ALOG("deliverModeChange(%s): pipe %u drives %p (%s) -- NOT delivering the mode change",
                     whyTag, i, pfb, ready);
                return false;
            }
        }
        deliverModeChange();
        return true;
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + 0xc78);
        ALOG("  GATES %s: accel+0xc78 = 0x%02x (enabled bit 0x2 %s)",
             when, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
#ifdef NMX_XEGFX
            // Head 4 has two candidates: the placeholder (fFBs[4]) and the KMS adapter (fKmsFB).  Use the adapter only
            // when its start gate is open and the placeholder's is not; otherwise the placeholder path is exactly the
            // one that ran before M3d.  Selection happens here, at registration time, because the gate properties are
            // published after start() returned -- later than the first-publish notification that filled the slots.
            if (i == 4 && fKmsFB && fKmsFB != fb) {
                const char *kmsGate = xgXeGfxFBGate(fKmsFB);
                const char *curGate = fb ? xgXeGfxFBGate(fb) : nullptr;
                if (kmsGate && !curGate) {
                    ALOG("  %s: head 4 -> %s (gate %s open; placeholder %s)", why, xgXeGfxFBClass(fKmsFB), kmsGate,
                         fb ? "not ready" : "absent");
                    fb = fKmsFB;
                }
            }
#endif
            if (!fb) {
                // The Intel panel (index 4, NMIntelFB) does not wait for NVIDIA heads that never publish: a machine with one
                // monitor publishes only head 0, and the walk stopped there, so the Intel head got no pipe (measured 10-10,
                // published 0x11 registered 0x1). Pipes are found by their framebuffer pointer, not their number.
                if (gNvIgpu && i < 4 && ((gNvHeadsPublished >> 4) & 1)) { i = 3; continue; }
                if (gNvHeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gNvHeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            if (i == 4 && !gNvIgpu) break;   // the Intel panel waits for debug.xegfxaccel_igpu=1
            const char *ready = nullptr;
            if (!fbReadyForRegistration(fb, &ready)) {
                ALOG("  %s: index %u (%s) is not registrable yet (%s) -- stopping this walk", why, i,
                     fb->getName() ? fb->getName() : "?", ready);
                break;
            }
            unsigned b = fDM->getFramebufferCount();
            uint8_t *dm = (uint8_t *)fDM;
            // The family also discovers framebuffers itself. Its notification
            // can win the race with this walk; identify the existing pipe by
            // its actual framebuffer, never by a count increment or head ID.
            auto findPipe = [&](unsigned count, void **out, unsigned *slot) {
                if (count > 8) return false;
                unsigned matches = 0;
                for (unsigned j = 0; j < count; ++j) {
                    void *p = *(void *const volatile *)(dm + 0x88 + 8 * j);
                    if (!p) return false;
                    if (*(IOService *const volatile *)((const uint8_t *)p + 0x98) == fb) {
                        *out = p; *slot = j; ++matches;
                    }
                }
                return matches == 1;
            };
            if (b > 8) {
                ALOG("  %s: implausible pipe count %u -- refusing registration", why, b);
                break;
            }
#ifdef NMX_XEGFX
            // KMS owns real framebuffer resources. The family's initial pipe
            // setup can request their CPU mappings before found_framebuffer
            // returns, so both gates must precede it and Metal publication.
            // The CPU-only placeholder continues to use its existing path.
            if (nvXeIsKmsFB(fb)) {
                gNvScanout = 1;
                gNvAperture = 1;
                setProperty("XeGfxAccelKmsMemoryEnabled", kOSBooleanTrue);
                ALOG("  %s: KMS scanout and resource aperture enabled before pipe registration", why);
            }
            ALOG("  %s: %s at index %u passed the start gate %s -- registering it", why,
                 xgXeGfxFBClass(fb), i, xgXeGfxFBGate(fb));
#endif
            void *pipe = nullptr;
            unsigned pipeSlot = 0;
            bool adopted = findPipe(b, &pipe, &pipeSlot);
            if (!adopted) {
                ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
                fDM->found_framebuffer((IOFramebuffer *)fb);
            }
            unsigned c = fDM->getFramebufferCount();
            // Re-scan as well: the family may have added the matching pipe
            // between our first scan and found_framebuffer (which deduplicates).
            if (!findPipe(c, &pipe, &pipeSlot)) {
                if (c == b + 1 && b < 8 &&
                    !*(void *const volatile *)(dm + 0x88 + 8 * b)) {
                    *(volatile uint32_t *)(dm + 0x108) = b;
                    ALOG("  %s: NULL pipe in new slot %u -- restored count to %u", why, b, b);
                }
                ALOG("  %s: index %u has no unique matching pipe (count %u -> %u); not registered", why, i, b, c);
                break;
            }
#ifdef NMX_XEGFX
            if (fb == fKmsFB && !fFB && OSCompareAndSwapPtr(nullptr, fb, (void *volatile *)&fFB))
                ALOG("  %s: head 4 -> %s is now the primary framebuffer", why, xgXeGfxFBClass(fb));
#endif
            ALOG("  %s: index %u -> pipe %u (%p), count %u (%s)", why, i, pipeSlot, pipe, c,
                 adopted ? "adopted existing family pipe" : "matched after registration");
            fFBRegistered |= 1u << i;
            gNvHeadsRegistered = fFBRegistered;
            gNvHeadsPiped |= 1u << i;
#ifdef NMX_XEGFX
            // M3d: the head-4 pipe is the display pipe now.  This is the first moment the adapter is genuinely driving
            // the panel through us, so (re)publish the present API: from here the adapter may call xgFlipPresent.
            if (fb == fKmsFB) nvXePublishPresentAPI(fb, "head 4 piped", this);
#endif
            added++;
        }
        return added;
    }
    void publishArmLocked() {
#ifdef NMX_XEGFX
        if (nvXeIsKmsFB(fFB) && (!gNvScanout || !gNvAperture)) {
            ALOG("KMS memory gates incomplete -- Metal arm remains closed");
            return;
        }
#endif
        gNvAccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.xegfxaccelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);
    }
    void onCountGrewLocked() {
        if (deliverModeChangeIfPipesReady("count grew")) publishArmLocked();
        else ALOG("mode change rejected -- Metal arm remains closed");
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        IOService *candidate = fFB;
#ifdef NMX_XEGFX
        // The claim adapter is parked until registerPendingLocked selects it.
        // With no IntelFB placeholder, fFB is intentionally null until that
        // registration. Test the same candidate here instead of returning
        // before the selection can run. Closed start gates still defer.
        if (fKmsFB && xgXeGfxFBGate(fKmsFB) && !xgXeGfxFBGate(candidate))
            candidate = fKmsFB;
#endif
        if (!candidate) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        const char *ready = nullptr;
        if (!fbReadyForRegistration(candidate, &ready)) {
            ALOG("registerFramebuffer(%d): %s is not registrable yet (%s) -- deferring, no display pipe",
                 how, candidate->getName(), ready);
            return false;
        }
        unsigned before = fDM->getFramebufferCount();
        unsigned reconciled = 0;
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            reconciled = registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        if (reconciled || (how == 2 && after > before)) { onCountGrewLocked(); return true; }
        return false;
    }
#ifdef NMX_XEGFX
    // --- M4a: boot-armed auto-configuration, defined out of line below (it needs the iop/flip globals) -----------
    // The same five actions userspace drives by hand, run by a kernel thread inside this kext so they happen before
    // loginwindow/WindowServer asks for a Metal device.  The manual debug.xegfxaccelfb path is untouched.
    int fBootArmed = 0;                       // 0 idle, 1 a sequence is running, 2 it finished (see gBootArmState)
    bool bootArmRequested(const char **why, int *source);
    void bootArmStart(const char *why, int source);
    bool bootHeadReady(const char **cls, const char **gate);
    void bootArmRun(void);
    void bootArmFinish(int ok, const char *msg);
    static void bootArmThread(void *p0, int resume);
#endif
    const OSSymbol *fSymAlloc = nullptr, *fSymFree = nullptr;
    IOReturn askFramebufferForVram(struct NVRMVramRequest *r, bool alloc) {
#ifdef NMX_XEGFX
        // the Intel GPU has no VRAM: "vidmem" is wired system memory with a CPU mapping. phys is a made-up, never
        // reused address that only names the allocation (the family wants one); nothing maps it, see the aperture
        if (alloc) {
            IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                kIODirectionInOut | kIOMemoryKernelUserShared, r->size, 4096);
            if (!b) return kIOReturnNoMemory;
            if (b->prepare() != kIOReturnSuccess) { b->release(); return kIOReturnNoMemory; }
            r->handle = b; r->kva = b->getBytesNoCopy(); r->actualSize = b->getLength();
            r->phys = OSAddAtomic64((SInt64)((r->actualSize + 0x1fffff) & ~0x1fffffull), &gXeFakePhys);
            return kIOReturnSuccess;
        }
        if (IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)r->handle)) {
            b->complete(); b->release();
        }
        return kIOReturnSuccess;
#endif
        if (!fFB) return kIOReturnNotAttached;
        if (!fSymAlloc) fSymAlloc = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
        if (!fSymFree)  fSymFree  = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
        const OSSymbol *fn = alloc ? fSymAlloc : fSymFree;
        if (!fn) return kIOReturnNoMemory;
        return fFB->callPlatformFunction(fn, false, r, nullptr, nullptr, nullptr);
    }
    IOReturn askFramebufferForScanout(struct NVRMVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb) return kIOReturnNotAttached;
#ifdef NMX_XEGFX
        // M3d: the KMS adapter answers with its own 80-byte v2 descriptor, not with the old 5x u32 VRAM answer, and a
        // NotReady from it means "no scanout yet" -- deliberately NOT described from the boot aperture: that fallback
        // exists for the placeholder path only, whose behaviour must stay byte-identical.  An adapter that has not
        // allocated its scanout leaves the pipe without a resource instead of pointing it at stale boot memory.
        if (nvXeIsKmsFB(fb)) return nvXeKmsScanoutInfo(fb, r);
#endif
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_SCANOUT);
        IOReturn ret = fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
#ifdef NMX_XEGFX
        // Apple's boot framebuffer on our iGPU does not speak the VRAM ABI: describe its scan-out from its own aperture
        // and current mode, a CPU mapping only, so frames take the copy path (NMIntelFB at boot left the panel on text)
        if (ret != kIOReturnSuccess)
            if (IOFramebuffer *ifb = OSDynamicCast(IOFramebuffer, fb)) ret = describeBootFramebuffer(ifb, r);
#endif
        return ret;
    }
#ifdef NMX_XEGFX
    IOMemoryMap *fBootFBMap = nullptr;
    IOPhysicalAddress fBootFBPhys = 0;
    // asked on every copied frame: filled once, then served from here. Each ask used to call into IONDRV outside its
    // gate ("_doStatus not gated"); the desktop ran two hours on the Intel GPU and then one CPU stopped answering
    // right after such a call (N-258, panic 00:17). The boot mode never changes under us.
    struct NVRMVramRequest fBootDesc = {};
    bool fBootDescOK = false;
    IOReturn describeBootFramebuffer(IOFramebuffer *ifb, struct NVRMVramRequest *r) {
        /* EXPERIMENT: refuse to describe the boot raster.  With the accelerator loaded the panel
           shows a band of colour noise at the bottom that neither the firmware raster nor a pure
           software desktop has; if it disappears without this, the write-combined map of the
           aperture below is what corrupts the scanout.  Remove to restore the copy target. */
        if (0) { ALOG("describeBootFramebuffer: DISABLED by experiment -- no copy target"); return kIOReturnUnsupported; }
        // A-53: once the 12544-wide scanout exists, describe it instead of the firmware raster.  Copies
        // then go there at 12544 stride, and q.pitch becomes equal to the compositor's P -- the one thing
        // the deferred flip asks for.
        if (gScanout.live) {
            r->kva = gScanout.kva; r->phys = gScanout.phys;
            r->width = 3120; r->height = 2080; r->pitch = gScanout.pitch;
            r->actualSize = r->size = gScanout.bytes;
            return kIOReturnSuccess;
        }
        if (fBootDescOK) {
            r->kva = fBootDesc.kva; r->phys = fBootDesc.phys; r->width = fBootDesc.width; r->height = fBootDesc.height;
            r->pitch = fBootDesc.pitch; r->actualSize = fBootDesc.actualSize; r->size = fBootDesc.size;
            return kIOReturnSuccess;
        }
        IODisplayModeID mode = 0; IOIndex depth = 0; IOPixelInformation pi = {};
        if (ifb->getCurrentDisplayMode(&mode, &depth) != kIOReturnSuccess ||
            ifb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pi) != kIOReturnSuccess) {
            ALOG("boot framebuffer %s: no current mode / pixel information", ifb->getName());
            return kIOReturnNotReady;
        }
        if (!fBootFBMap) {
            IODeviceMemory *mem = ifb->getApertureRange(kIOFBSystemAperture);
            if (!mem) { ALOG("boot framebuffer %s: no system aperture", ifb->getName()); return kIOReturnNoMemory; }
            fBootFBPhys = mem->getPhysicalAddress();
            fBootFBMap = mem->map(kIOMapWriteCombineCache);
            mem->release();
            if (!fBootFBMap) { ALOG("boot framebuffer %s: aperture map failed", ifb->getName()); return kIOReturnNoMemory; }
        }
        r->kva = (void *)fBootFBMap->getVirtualAddress();
        // the aperture's real address: the family wants a scanout resource on the pipe before it marks it active and
        // sends transactions (N-253: phys 0 = CPU-only, no resource, pipe never active, no frames)
        r->phys = fBootFBPhys;
        r->width = pi.activeWidth; r->height = pi.activeHeight; r->pitch = pi.bytesPerRow;
        r->actualSize = fBootFBMap->getLength(); r->size = r->actualSize;
        ALOG("boot framebuffer %s: %u x %u pitch %u, %llu bytes at %p (CPU copy target)", ifb->getName(), r->width,
             r->height, r->pitch, (unsigned long long)r->actualSize, r->kva);
        fBootDesc.kva = r->kva; fBootDesc.phys = r->phys; fBootDesc.width = r->width; fBootDesc.height = r->height;
        fBootDesc.pitch = r->pitch; fBootDesc.actualSize = r->actualSize; fBootDesc.size = r->size;
        fBootDescOK = r->kva && r->pitch && r->height;
        return kIOReturnSuccess;
    }
#endif
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kNvMaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
#ifdef NMX_XEGFX
        // The claim adapter stays in the alternative slot. Pipe init can ask
        // for scanout before found_framebuffer returns and sets fFB primary.
        if (fb == fKmsFB && xgXeGfxFBGate(fb)) {
            if (idx) *idx = 4;
            return fb;
        }
#endif
        return nullptr;
    }
    IOService *fNVRM = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
#ifdef NMX_XEGFX
        if (type == 0x58474452u) {   // 'XGDR'
            IOService *carrier = nullptr;
            for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
                carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
            fCarrierNext++;
            IOAccelXeGfxDRMClient *uc = OSTypeAlloc(IOAccelXeGfxDRMClient);
            if (!uc || !carrier || !uc->initWithTask(owningTask, securityID, type)) { if (uc) uc->release(); return kIOReturnNoResources; }
            if (!uc->attach(carrier)) { uc->release(); return kIOReturnInternalError; }
            if (!uc->start(carrier)) { uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
            if (fUcLogged < 40) { fUcLogged++; ALOG("newUserClient('XGDR') -> DRM ioctl client for pid %d", proc_selfpid()); }
            *handler = uc;
            return kIOReturnSuccess;
        }
#endif
        if ((type & 0xFFFF0000u) != 0x4E560000u) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOG("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }
#ifdef NMX_ACCEL
        // NVIDIA RM clients are NVK's; RADV and ANV talk to NMXGPU's DRM device nodes instead
        ALOG("newUserClient(0x%x): an NVIDIA RM client type on an AMD/Intel accelerator -- refused", type);
        return kIOReturnUnsupported;
#endif

        if (!fNVRM) {
            OSDictionary *m = serviceMatching("NVRM");
            if (m) fNVRM = waitForMatchingService(m, 100ULL * 1000 * 1000);
        }
        if (!fNVRM) { ALOG("newUserClient(0x%x): no NVRM service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nvNewAccelClient");
        if (!fnSym) return kIOReturnNoMemory;
        IOUserClient *uc = nullptr;
        IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NVRM refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOG("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    static void triggerFromSysctl(int how) {
        if (!gAccel) { ALOG("sysctl debug.xegfxaccelfb=%d but no NVAccel is running", how); return; }
        if (how >= 4) gNvAperture = 1;
        if (how >= 3) {
            gNvScanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            if (gAccel->registerFramebufferLocked(1))
                ALOG("sysctl debug.xegfxaccelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.xegfxaccelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                if (gAccel->deliverModeChangeIfPipesReady("sysctl"))
                    gAccel->publishArmLocked();
            }
            if (gAccel->fRegLock) IOLockUnlock(gAccel->fRegLock);
            return;
        }
        gAccel->registerFramebuffer(how);
    }
    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        gAccel->fEnableCount++;
        ALOG("enableAccelerator() from %s (call #%u)", why, gAccel->fEnableCount);
        gAccel->setProperty("NVAccelEnabledFrom", why);
        gAccel->setProperty("NVAccelEnableCount", gAccel->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
#ifdef NMX_ACCEL
            static const char *const kind = "AMD/Intel";
            if (cls != 0x03 || (ven != 0x1002 && ven != 0x8086)) {
#else
            static const char *const kind = "NVIDIA";
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x10de || dev < 0x0020) {
#endif
                ALOG("%04x:%04x class %02x.%02x is not an %s display device we support - not claiming",
                     ven, dev, cls, sub, kind);
                return NULL;
            }
            ALOG("%s display device %04x:%04x class %02x.%02x - claiming", kind, ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        { if (nvrm_driver_off()) return false; }
#ifdef NMX_ACCEL
        // NMXGPU decides which AMD/Intel GPUs are driven (nmxamd=1 / nmxintel=1 / nullmoth-gpu); this accelerator only
        // starts on a device it has claimed, so a GPU macOS drives itself never gets a second accelerator
        // No waiting here: NMXGPU attaches from a thread once the disks with its firmware are mounted, and mounting waits
        // for matching to settle - a start() that blocked matching for the claim would hold the boot at the logo. NMXGPU
        // re-runs matching on the device (registerService) after it sets nmxgpu-claimed, which offers it here again.
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            if (pci->getProperty("nmxgpu-claimed") != kOSBooleanTrue) {
                ALOG("NMXGPU has not claimed %04x:%04x yet - not starting now; offered again when it does",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NMXGPU drives %04x:%04x - starting", pci->configRead16(0x00), pci->configRead16(0x02));
        }
#else
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("nvaccel", &gate, sizeof gate) || !gate) { ALOG("boot-arg nvaccel=1 absent: NOT starting (boot-loop guard)"); return false; }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("nvrm-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("nvrm-claimed") != kOSBooleanTrue) {
                ALOG("NVRM never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NVRM owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
        }
#endif
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            // Tells the Metal plugin that NVResource::pageoff waits for a surface WindowServer's GPU is still writing
            // (nvSurfWaitReady). Only then does the plugin mark WindowServer's surfaces newest in VRAM at commit; with an
            // accelerator that does not wait, that mark would let a CPU lock or a copied present read a surface before
            // its pixels landed, so the plugin keeps marking them after the GPU finishes.
            setProperty("NVAccelSurfaceWait", 1ULL, 32);
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "NVMTLDriver");
            gAccel = this; gEnableAccel = &NVAccel::enableFromHook;
            for (unsigned i = 0; i < NV_CARRIERS; i++) {
                IOAccelNVRMCarrier *c = OSTypeAlloc(IOAccelNVRMCarrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < NV_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)NV_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
#ifdef NMX_XEGFX
            // vblank sync: the display pipe's frame counter lives in BAR0 (display 14 register offsets, the same
            // snapshot AGDC reads).  A read-only uncached map is enough and stays mapped for the boot.
            if (fPCI) {
                IOMemoryDescriptor *bar = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
                IOMemoryMap *map = bar ? bar->map(kIOMapInhibitCache) : nullptr;
                if (map && map->getLength() >= 0x71000) {
                    gDispMmio = (volatile uint32_t *)map->getVirtualAddress();
                    ALOG("display MMIO %p: frames are copied right after the panel's vblank", gDispMmio);
                    {
                        const uint32_t pc = gDispMmio[0x70180u / 4], ps = gDispMmio[0x70188u / 4];
                        const uint32_t pz = gDispMmio[0x70190u / 4], pp = gDispMmio[0x70194u / 4];
                        const uint32_t pf = gDispMmio[0x7019cu / 4];
                        ALOG("PLANE ctl=%08x stride=%08x size=%08x pos=%08x surf=%08x", pc, ps, pz, pp, pf);
                        for (uint32_t o = 0x701a0u; o < 0x70200u; o += 4u)
                            ALOG("  PS[%03x] = %08x", o - 0x70100u, gDispMmio[o / 4]);
                    }
                } else {
                    if (map) map->release();
                    ALOG("display MMIO unavailable: frames are copied as soon as WindowServer sends them");
                }
            }
#endif
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            fRegLock = IOLockAlloc();
            ALOG("multi-head: up to %u heads, registration lock %p%s", kNvMaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
#if defined(NMX_XEGFX)
            // over XeGfx the Intel GPU owns the built-in panel: NMIntelFB publishes it as head 4 and WindowServer
            // composites here once debug.xegfxaccelfb=1 registered it; frames reach the panel through the CPU copy path
            nvaccelInstallSysctl();
            gNvHeadless = 0;
            ALOG("Intel accelerator over XeGfx: watching for NMIntelFB / XeGfxFramebuffer (head 4); register with sysctl debug.xegfxaccelfb=1");
            if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                im->release();
            }
            // without NMIntelFB the panel stays on Apple's boot framebuffer (IONDRVFramebuffer) on this same iGPU
            if (OSDictionary *bm = serviceMatching("IONDRVFramebuffer")) {
                fBootNote = addMatchingNotification(gIOFirstPublishNotification, bm, &NVAccel::fbPublished, this, nullptr, 0);
                bm->release();
            }
            // and XeGfxIntelFB publishes the panel under an Intel class name (CoreDisplay's vendor check, I-108)
            if (OSDictionary *xm = serviceMatching("XeGfxIntelFramebuffer")) {
                fXeNote = addMatchingNotification(gIOFirstPublishNotification, xm, &NVAccel::fbPublished, this, nullptr, 0);
                xm->release();
            }
            // the KMS adapter XeGfxFramebuffer is a direct IOFramebuffer subclass, so neither of the two notifiers above
            // ever sees it: give it its own (fbPublished rejects the wrong device and the wrong class)
            if (OSDictionary *km = serviceMatching("XeGfxFramebuffer")) {
                fKmsNote = addMatchingNotification(gIOFirstPublishNotification, km, &NVAccel::fbPublished, this, nullptr, 0);
                km->release();
            }
            // M4a: arm the accelerated desktop from the kernel when THIS boot asked for it (boot argument or the
            // XeGfxBootArm property).  Without a trigger nothing changes: the manual sysctls stay the only way.
            {
                const char *armWhy = nullptr; int armSource = 0;
                if (bootArmRequested(&armWhy, &armSource)) bootArmStart(armWhy, armSource);
            }
            OSDictionary *fbm = nullptr;
#elif defined(NMX_ACCEL)
            // AMD/Intel: the Metal device is for apps; WindowServer draws into NMXGPU's framebuffer, and the plugin copies
            // finished frames into the IOSurfaces it reads (the headless path NVIDIA laptops use)
            nvaccelInstallSysctl();
            gNvHeadless = 1;
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            ALOG("AMD/Intel accelerator up: Metal device for apps, frames copied to the framebuffer WindowServer draws");
            OSDictionary *fbm = nullptr;
#else
            OSDictionary *fbm = serviceMatching("NVRMNVDAFramebuffer");
#endif
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &NVAccel::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for NVRMFramebuffer to publish -> notifier %p", fFBNote);
                // a laptop whose built-in panel is on the Intel GPU: NMIntelFB publishes as head 4 of this accelerator
                if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                    fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                    im->release();
                    ALOG("watching for an Intel built-in panel (NMIntelFB) -> notifier %p", fIntelNote);
                }
                nvaccelInstallSysctl();
                if (getProperty("MetalPluginName")) {
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.xegfxaccelfb=1` puts it back, and a reboot takes it away again");
                    armHeadlessCheck();
                }
            } else ALOG("serviceMatching(NVRMFramebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE { ALOG("stop"); IOGraphicsAccelerator2::stop(provider); }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVAccel, IOGraphicsAccelerator2)
NVAccel *NVAccel::gAccel = nullptr;

#ifdef NMX_XEGFX
bool NVResource::init(IOGraphicsAccelerator2 *accel, IOAccelShared2 *shared, unsigned int flags)
{
    const bool trace = !shared && (flags & 0x40u);
    if (trace) ALOG("resource init BEGIN flags=0x%x shared=%p", flags, shared);
    bool ok = IOAccelResource2::init(accel, shared, flags);
    if (trace) ALOG("resource init END ok=%d", ok);
    return ok;
}

bool NVDisplayPipe::init(IOGraphicsAccelerator2 *accel, IOAccelDisplayMachine *machine,
                         IOFramebuffer *fb, unsigned int index)
{
    ALOG("pipe init BEGIN index=%u controller=%s", index,
         fb && fb->getWorkLoop() ? "READY" : "MISSING");
    bool ok = IOAccelLegacyDisplayPipe::init(accel, machine, fb, index);
    ALOG("pipe init END index=%u ok=%d", index, ok);
    return ok;
}
#endif

IOAccelMemory *NVDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res)
{
#ifdef NMX_XEGFX
    OSIncrementAtomic64(&gCntPipeInit);
#endif
    if (!gNvScanout) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): scanout gate CLOSED -- returning NULL, "
             "which is the family's own default (sysctl debug.xegfxaccelfb=3 opens it)", index);
        return nullptr;
    }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !res) { ALOG("NVDisplayPipe::initFramebufferResource: no accelerator or no resource"); return nullptr; }
    unsigned head = 0;
    IOService *fb = a->pipeFramebuffer(this, &head);
    if (!fb) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): pipe %p drives framebuffer %p, which is none of ours -- "
             "refusing (never head 0's pages: that IS the bug)", index, this,
             *(void *const volatile *)((const uint8_t *)this + 0x98));
        return nullptr;
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&r, fb);
    if (ret == kIOReturnSuccess && r.kva && !r.phys) {
        // Another GPU's panel (NMIntelFB on a laptop without a MUX): its scanout is Intel memory with a CPU mapping and
        // no NVIDIA VRAM address. No vidmem may stand for it (addToAperture/getApertureMemoryDescriptor would hand
        // BAR1 offset 0 to the GPU), and none may be allocated in its place (fresh VRAM mapped here is the 10-10
        // os_map_kernel_space hang shape). NULL is the family's own default: frames reach this panel only through
        // performTransaction's copy into kva (debug.xegfxaccel_iop_ok_h4 counts them).
        ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u): CPU-only scanout %u x %u pitch %u (another GPU's "
             "panel) -- no framebuffer resource, frames are copied", head, index, r.width, r.height, r.pitch);
        return nullptr;
    }
    if (ret != kIOReturnSuccess || !r.phys || !r.actualSize) {
        ALOG("NVDisplayPipe::initFramebufferResource: framebuffer would not describe its scanout (0x%x)", (unsigned)ret);
        return nullptr;
    }

    OSObject *o = a->createVidMemory(nullptr, res, r.actualSize, nullptr);
    NVVidMemory *m = OSDynamicCast(NVVidMemory, o);
    if (!m) {
        ALOG("NVDisplayPipe::initFramebufferResource: createVidMemory gave %p, not an NVVidMemory", o);
        if (o) o->release();
        return nullptr;
    }
    m->fScanout = true;
    m->fPhys    = r.phys;
    m->fBytes   = r.actualSize;
#ifdef NMX_XEGFX
    m->fKva     = r.kva;
#endif
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = r.phys;
    ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p over the LIVE SCANOUT "
         "phys 0x%llx %u x %u pitch %u (%llu bytes)",
         head, index, res, m, (unsigned long long)r.phys, r.width, r.height, r.pitch,
         (unsigned long long)r.actualSize);
    return (IOAccelMemory *)m;
}

static NVVidMemory *nvres_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + 0x88);
    return OSDynamicCast(NVVidMemory, o);
}

bool NVResource::addToAperture()
{
    NVVidMemory *m = nvres_vidmem(this);
    if (fApLogged < 4) { fApLogged++;
        ALOG("NVResource::addToAperture -> true (vidmem %p phys 0x%llx %llu bytes, already in BAR1)",
             m, m ? m->fPhys : 0ull, m ? m->fBytes : 0ull); }
    return m && m->fPhys && m->fBytes;
}

void NVResource::removeFromAperture() { }

void *NVResource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gNvAperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.xegfxaccelfb=4 opens it)"); }
        return nullptr;
    }
    if (fApDesc) return fApDesc;
    NVVidMemory *m = nvres_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        return nullptr;
    }
#ifdef NMX_XEGFX
    // system memory: the buffer itself, never a device range over the made-up phys; the scanout is the real aperture
    if (m->fScanout) {
        IODeviceMemory *sd = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
        if (sd) fApDesc = sd;
        return fApDesc;
    }
    if (IOBufferMemoryDescriptor *b = OSDynamicCast(IOBufferMemoryDescriptor, (OSObject *)m->fRmHandle)) {
        b->retain(); fApDesc = b; return fApDesc;
    }
    return nullptr;
#endif
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("NVResource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); return nullptr; }
    fApDesc = d;
    if (fApLogged < 8) { fApLogged++;
        ALOG("NVResource::getApertureMemoryDescriptor -> %p over BAR1 phys 0x%llx (%llu bytes), offset 0",
             d, m->fPhys, m->fBytes); }
    return fApDesc;
}

static void nvSVForgetResource(NVResource *resource);

void NVResource::free()
{
    nvSVForgetResource(this);
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

// 1 = a surface keeps its VRAM across the family's deallocPhysical/allocPhysical (see deallocPhysical).
static int gVmSticky = 1;
static SInt64 gCntVmParked, gCntVmUnparked, gCntVmParkReleased;

bool NVVidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("NVVidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        ALOG("NVVidMemory::allocPhysical(%p): SCANOUT phys 0x%llx (%llu bytes) -- already backed, nothing allocated",
             this, fPhys, fBytes);
        return true;
    }
    if (fRmHandle) {
        ALOG("NVVidMemory::allocPhysical(%p): already backed by 0x%llx (%llu bytes) -- false, as AMD's does",
             this, fPhys, fBytes);
        return false;
    }
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
    if (!want) { ALOG("NVVidMemory::allocPhysical(%p): length at +0x40 is 0 -- nothing to back", this); return false; }
    if (!NVAccel::gAccel) { ALOG("NVVidMemory::allocPhysical(%p): no accelerator", this); return false; }
    if (fParkHandle) {
        if (fParkBytes >= want) {
            fRmHandle = fParkHandle; fKva = fParkKva; fPhys = fParkPhys; fBytes = fParkBytes; fAllocationCookie = fParkCookie;
            fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
            *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
            OSAddAtomic64(1, &gCntVmUnparked);
            return true;
        }
        releaseParked();   // the resource grew: its old VRAM cannot hold it
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = NVAccel::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        OSAddAtomic64(1, &gCntVmRefused);
        ALOG("NVVidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        // once per boot: how much of the window this side holds as sticky parks (kept across the family's dealloc)
        static volatile SInt32 said = 0;
        if (OSCompareAndSwap(0, 1, &said))
            ALOG("VRAM refused: %lld allocs (%lld bytes), %lld deallocs (%lld bytes), %lld parked now",
                 (long long)gCntVmAlloc, (long long)gCntVmAllocBytes, (long long)gCntVmDealloc, (long long)gCntVmDeallocBytes,
                 (long long)(gCntVmParked - gCntVmUnparked - gCntVmParkReleased));
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    ALOG("NVVidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void NVVidMemory::deallocPhysical()
{
    if (fScanout) {
        ALOG("NVVidMemory::deallocPhysical(%p): SCANOUT -- FB owns backing; keeping import identity", this);
        // The FB owns the backing for the entire boot. Keep the import
        // identity across unwire/rewire; clearing fScanout would make the
        // next allocPhysical allocate unrelated surface memory instead.
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
    if (!fRmHandle) return;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    // The family frees a surface's VRAM after copying it back for the CPU, and backs the surface with new VRAM when the
    // GPU needs it again. The Metal plugin imported the old VRAM once and keeps drawing into it, so from then on the
    // display and every copy-back read new, never-written VRAM: black frames, and windows black until a drag made
    // WindowServer import the surface again. Parked instead, this VRAM is what the next allocPhysical hands back, so the
    // surface keeps the memory the plugin draws into. It is released when the resource goes away or outgrows it.
    if (gVmSticky) {
        releaseParked();
        fParkHandle = fRmHandle; fParkKva = fKva; fParkPhys = fPhys; fParkBytes = fBytes; fParkCookie = fAllocationCookie;
        OSAddAtomic64(1, &gCntVmParked);
    } else {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
    }
    fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
}

void NVVidMemory::releaseVram(void *handle, void *kva, unsigned long long bytes, unsigned long long cookie)
{
    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.handle = handle; r.kva = kva; r.actualSize = bytes; r.allocationCookie = cookie;
    if (NVAccel::gAccel) NVAccel::gAccel->askFramebufferForVram(&r, false);
}

void NVVidMemory::releaseParked()
{
    if (!fParkHandle) return;
    releaseVram(fParkHandle, fParkKva, fParkBytes, fParkCookie);
    fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
    OSAddAtomic64(1, &gCntVmParkReleased);
}

void NVVidMemory::free()
{
    if (fRmHandle && !fScanout) {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
        fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    }
    releaseParked();
    IOAccelVidMemory::free();
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

static int    gNvIop = 0;
static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopOkHead[kNvMaxFB];
static SInt64 gCntIopSurfRepeat, gCntIopSameRefresh, gCntIopCopyOverrun;
static unsigned int gIopLastSurf;          /* last surface id presented */
static unsigned int gIopLastFrameAtCopy;   /* display frame counter when the last copy started */
static SInt64 gCntIopCopyCross, gCntIopSrcChanged;
static SInt64 gIopMapLastUs, gIopMapMaxUs, gIopMapSumUs;

// the display-pipe copy path's per-frame cost: the copy itself, the whole transaction, and the gap between
// transactions, all in us.  The transaction runs on WindowServer's display thread, so txn is WindowServer's frame time.
static SInt64 gIopCopyLastUs, gIopCopyMaxUs, gIopCopySumUs;
static SInt64 gIopTxnLastUs, gIopTxnMaxUs, gIopTxnSumUs;
static SInt64 gIopGapLastUs, gIopGapMaxUs, gIopGapSumUs;
static uint64_t gIopLastTxnEnd;
static int gIopMemcpy = 1;

// vblank sync: the copy goes straight into the panel's framebuffer, so a copy that starts in the middle of a
// scan shows its top half from this frame and its bottom half from the previous one (tearing).  The panel's
// frame counter (BAR0 0x70040) goes up at every vblank; waiting for it makes the copy start right after the
// panel begins scanning from the top, and the copy (2-4 ms against a 16.1 ms frame) stays ahead of the scan.
static int gIopVblank = 1;
static SInt64 gCntIopVblankWaits, gCntIopVblankSkips, gIopVblankLastUs, gIopVblankMaxUs;
#define NV_DISP_FRAME_COUNTER 0x70040u

// --- Intel zero-copy flip: the compositor's frame is already in system memory.  Instead of copying it into the
// boot framebuffer, reserved GGTT slots (XeGfx owns the PTEs via xgFlipInit/xgFlipMap) are pointed at the frame's
// own physical pages and PLANE_SURF_1_A is written once.  The plane latches the new offset at the next vblank, and
// the front before last stays pinned until a later flip proves its slot is no longer scanned.
static int gIopXeFlip = 0;
static IOService *gXeFlipSvc;
static unsigned long long gXeFlipGgtt[8];
// What each flip slot currently maps.  Rewriting 6370 PTEs and flushing the GPU TLB every frame cost
// about 20 ms at 3120x2080 -- more than the CPU copy this path replaced -- and CoreAnimation only cycles
// a few surfaces, so remember the mapping and reuse it while it still describes the same pages.
static uint32_t gXeSlotSid[8];		/* surface id each slot maps (0 = none) */
static unsigned int gXeSlotPages[8];	/* page count that mapping covers */
static unsigned long long gXeSlotFirst[8];	/* its first page, so a recycled id cannot be mistaken for it */
static SInt64 gXeMapHits, gXeMapMisses;
static unsigned int gXeFlipSlots, gXeFlipNext;
static unsigned long long gXeFlipBytes;
static unsigned int gXeFlipLive = ~0u;
static IOMemoryDescriptor *gXeFlipFrontMd, *gXeFlipPrevMd;
static IOSurface *gXeFlipFront, *gXeFlipPrev;
static IOSurface *gXeFlipUncertain; // pinned after a latch timeout until home is confirmed
static SInt64 gCntXeFlip, gCntXeFlipMiss, gCntXeFlipFail;
#define NV_PLANE_SURF_A 0x7019cu
#define NV_PLANE_STRIDE_A 0x70188u		/* PLANE_STRIDE_1_A: row bytes / 64, 11 bits */
// 4224 pages is 16.5 MB -- the 2560x1600 panel this was written for.  A 3120x2080 surface at the
// compositor's 12544 row pitch is 24.9 MB, i.e. 6370 pages, so the mapping below would refuse it
// even with the pitch gate open.  8192 pages is 32 MB.
#define NV_XE_FLIP_MAX_PAGES 8192

// --- K9: deferred Intel zero-copy flip (plan 2, kernel-level ordering).  The no-wait compositor hands WindowServer's
// frame to the display pipe as soon as its GPU work is submitted, so arming the plane here would scan out a frame that
// is still being written: the symptoms are half-presented frames, tearing, the cursor jumping back and the input-
// method box drawn wrong.  Instead the display thread only queues the frame, and a thread_call worker waits
// until the GPU has finished everything it was given ("xgFlipQuery": every engine idle) before it maps the frame's
// pages into a reserved GGTT slot and writes PLANE_SURF_1_A.  WindowServer itself never waits.  Frames are kept in a
// small FIFO and every submitted frame is armed and its display transaction signalled in order, so WindowServer always
// gets exactly one completion per frame it submitted.  While a frame is queued or in flight isTransactionComplete
// reports the pipe busy, so WindowServer cannot recycle the frame's surface before it is on the panel.
static int gIopXeDefer = 0;		// debug.xegfxaccel_iop_xe_defer: 1 = defer the plane write to the idle worker
static int gIopXeWaitIdle = 1;		// debug.xegfxaccel_iop_xe_wait_idle: 0 = arm without waiting for GT idle
static int gIopXeIdleMs = 40;		// debug.xegfxaccel_iop_xe_idle_ms: longest GT-idle wait, then arm anyway
static int gIopXeReportBusy = 1;	// debug.xegfxaccel_iop_xe_report_busy: 0 = always report the transaction complete
static SInt64 gCntXeDeferQueued, gCntXeDeferArmed, gCntXeDeferCoalesced, gCntXeDeferDropped;
static SInt64 gCntXeDeferSignals, gCntXeDeferForced;
static SInt64 gCntXeDeferIdleWait, gCntXeDeferIdleForced;
static SInt64 gXeDeferIdleLastUs, gXeDeferIdleMaxUs, gXeDeferArmLastUs, gXeDeferArmMaxUs;
static SInt64 gXeDeferQMaxDepth;
/* The queue holds one entry per display transaction WindowServer handed over, so every transaction the worker
   takes is also signalled - a transaction must never be left without a completion.  (The 2026-10-26 freeze was a
   transaction whose arm had finished but whose busy flag stayed set across the signal: WindowServer woke on the
   interrupt, asked isTransactionComplete before the worker cleared the flag, got "busy", and waited forever for a
   second wakeup that never came - the display then went "not ready" and the watchdog killed WindowServer.)  The
   busy flag is cleared *before* the signal now, and every queued frame gets its own signal.  An overflow (never
   expected with a serialised display link) completes that transaction immediately instead of leaving it hanging. */
#define NV_XE_FLIP_QUEUE 4
struct NVXeFlipPend { void *pipe; IOSurface *s; unsigned w, h, pitch; };
static NVXeFlipPend gXeFlipQ[NV_XE_FLIP_QUEUE];	// [head, tail) queue owns references in s and pipe
static unsigned int gXeFlipQHead, gXeFlipQTail;
static volatile SInt32 gXeFlipBusy;	// 1 while the worker is queued/running for at least one frame
static SInt64 gXeFlipBusySince;		// mach_absolute_time() when busy went 0 -> 1 (stuck-worker escape hatch)
static SInt64 gXeFlipGen;		// bumped when the display pipe goes away, so a stale worker does not arm
static IOLock *gXeFlipLock;
static struct thread_call *gXeFlipCall;
static const OSSymbol *gXeFlipQuerySym;
static int gXeFlipArmRun;		// consecutive deferred frames that did not reach the panel

#ifdef NMX_XEGFX
// --- M3d: adapter-initiated present (the XeGfxFramebuffer asks for the plane directly, see the ABI block at the top of
// this file).  The adapter owns the mode and the scanout memory; this accelerator still owns PLANE_SURF_1_A.  One
// present at a time (gXePresentLock), and gXePresentActive keeps the K9 defer worker from arming a compositor frame at
// the same time: whichever side gets there first wins, the other one coalesces (defer) or returns kIOReturnBusy.
static IOLock *gXePresentLock;
static volatile SInt32 gXePresentActive;	// 1 while nvXePresent() is programming the plane
static IOService *gXePresentFB;			// retained adapter framebuffer that armed the API
static int gXePresentPublished;
struct NVXePresentLast {
    unsigned long long phys, flipCount;
    unsigned int ggtt, pitch, width, height, format;
};
static NVXePresentLast gXePresentLast;
static SInt64 gCntPresentOk, gCntPresentFail, gCntPresentBusy, gCntPresentNotReady, gCntPresentBadArg;
static SInt64 gCntPresentGgttWrites, gCntPresentPhysWrites, gCntPresentLatchTimeouts;
static SInt64 gXePresentLastUs, gXePresentMaxUs;
static SInt64 gCntVblankReads, gCntVblankWraps;
static SInt64 gCntKmsScanoutOk, gCntKmsScanoutNotReady;
static SInt64 gCntCursorRequests;
static unsigned long long gXeVblankState;	// (generation << 32) | low 32 bits of BAR0 0x70040
static unsigned long long gXeVblankNs;		// mach time (ns) of the last observed frame-counter change
#endif

extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}

extern "C" void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
static inline SInt64 nvIopUsBetween(uint64_t t0, uint64_t t1)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    return (SInt64)(ns / 1000);
}
static inline void nvIopStat(SInt64 *last, SInt64 *max, SInt64 *sum, SInt64 v)
{
    *last = v;
    if (v > *max) *max = v;
    *sum += v;
}

// Waits for the panel's next vblank, then returns so the caller can copy while the scan is at the top of the
// frame.  Bounded at ~1.5 frames: a display that is off or asleep must not hold the transaction.
static void nvAccelIopWaitVblank(void)
{
    if (!gIopVblank || !gDispMmio) { OSAddAtomic64(1, &gCntIopVblankSkips); return; }
    const uint32_t before = gDispMmio[NV_DISP_FRAME_COUNTER / 4];
    const uint64_t t0 = mach_absolute_time();
    for (;;) {
        IOSleep(1);
        if (gDispMmio[NV_DISP_FRAME_COUNTER / 4] != before) break;
        if (nvIopUsBetween(t0, mach_absolute_time()) > 25000) { OSAddAtomic64(1, &gCntIopVblankSkips); return; }
    }
    const SInt64 us = nvIopUsBetween(t0, mach_absolute_time());
    OSAddAtomic64(1, &gCntIopVblankWaits);
    gIopVblankLastUs = us;
    if (us > gIopVblankMaxUs) gIopVblankMaxUs = us;
}
static IOService *nvAccelResident(void)
{
    if (!gXeFlipSvc) {
        OSDictionary *m = IOService::serviceMatching("XeGfxResident");
        if (m) { gXeFlipSvc = IOService::waitForMatchingService(m, 1000ULL * 1000 * 1000); m->release(); }
    }
    return (gXeFlipSvc);
}
static IOReturn nvAccelResidentCall(const char *fn, void *p1, void *p2, void *p3, void *p4)
{
    IOService *svc = nvAccelResident();
    if (!svc) return (kIOReturnNotAttached);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return (kIOReturnNoMemory);
    IOReturn r = svc->callPlatformFunction(sym, false, p1, p2, p3, p4);
    sym->release();
    return (r);
}
static void nvAccelXeFlipDrop(IOSurface **s)
{
    /* getMemoryDescriptor() returns an unretained descriptor: never release it.
       Only the IOSurface is retained (it keeps the frame memory alive while the
       display engine may still be scanning it). */
    if (*s) { ((OSObject *)*s)->release(); *s = nullptr; }
}
// 1 when the GPU finished everything it was given (the K9 ordering point), 0 on a timeout or with no i915 device.
// Polls core's "xgFlipQuery" in 1 ms steps; *waitUs gets the time it took.  Thread context, sleeps.
static int nvAccelXeFlipWaitIdle(SInt64 *waitUs)
{
    const uint64_t t0 = mach_absolute_time();
    int idle = -1;
    *waitUs = 0;
    if (!gIopXeWaitIdle) return (1);		// A/B switch: arm without ordering
    IOService *svc = nvAccelResident();
    if (!svc) return (0);
    if (!gXeFlipQuerySym) gXeFlipQuerySym = OSSymbol::withCStringNoCopy("xgFlipQuery");
    if (!gXeFlipQuerySym) return (0);
    for (;;) {
        idle = -1;
        svc->callPlatformFunction(gXeFlipQuerySym, false, &idle, nullptr, nullptr, nullptr);
        if (idle == 1 || idle < 0) break;
        if (nvIopUsBetween(t0, mach_absolute_time()) >= (SInt64)gIopXeIdleMs * 1000) break;
        IOSleep(1);
    }
    *waitUs = nvIopUsBetween(t0, mach_absolute_time());
    if (idle == 1) OSAddAtomic64(1, &gCntXeDeferIdleWait);
    return (idle == 1);
}
// Arm one frame: point a free GGTT slot at the frame's pages, wait for GT idle when asked to (the K9 order), write
// PLANE_SURF_1_A, and wait for the panel to latch it at a vblank (the frame before keeps scanning until then).  On
// success the frame is on the panel and takes over the front/previous references; on failure the caller still owns it.
// ---- A-53 stage 1: a scanout of our own, laid out at the compositor's row pitch --------------------
// CoreAnimation aligns IOSurface rows to 256 bytes, so a 3120-wide panel asks for 12544 while the firmware
// console scans 12480 (PLANE_STRIDE_1_A = 0xc3).  The deferred flip in performTransaction() only arms when
// q.pitch == P, so on this panel it never runs and every frame is copied by the CPU.  The boot aperture
// cannot simply be widened (25,958,400 B; 12544 x 2080 needs 26,091,520), but XeGfx already reserves GGTT
// slots of exactly the surface size -- take one for a scanout that is 12544 wide and the equality holds
// without touching that check.  Stage 1 allocates and maps only: no plane register is written here.

static bool nvScanoutAlloc(void)
{
    if (gScanout.live) return true;
    const unsigned pitch = 12544u, h = 2080u;
    const unsigned long long bytes = (unsigned long long)pitch * h;
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionOutIn | kIOMemoryKernelUserShared, (vm_size_t)bytes, 4096);
    if (!md) { ALOG("scanout: %llu B allocation failed", bytes); gScanoutState = 3; return false; }
    unsigned long long ggtt[8] = { 0 }; unsigned n = 0;
    if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)4, ggtt, &n)
            != kIOReturnSuccess || n == 0) {
        ALOG("scanout: xgFlipInit refused (%u slots)", n); md->release(); gScanoutState = 3; return false;
    }
    ALOG("scanout: xgFlipInit -> %u slot(s), first 0x%llx (asked %llu B)", n, ggtt[0], bytes);
    if (gXeFlipSlots && bytes > gXeFlipBytes) {
        ALOG("scanout: reserved slot is %llu B, need %llu -- refusing", gXeFlipBytes, bytes);
        md->release(); gScanoutState = 3; return false;
    }
    const unsigned int need = (unsigned int)((bytes + 4095ull) >> 12);
    if (need > NV_XE_FLIP_MAX_PAGES) {
        ALOG("scanout: %llu B needs %u pages, cap is %u -- refusing", bytes, need, (unsigned)NV_XE_FLIP_MAX_PAGES);
        md->release(); gScanoutState = 3; return false; }

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

static bool nvAccelXeFlipArm(IOSurface *s, unsigned w, unsigned h, unsigned pitch, bool waitIdle)
{
    if (gXeFlipUncertain) return false;
    if (!gDispMmio || !s) return (false);
    const unsigned long long bytes = (unsigned long long)pitch * (unsigned long long)h;
    if (bytes == 0 || bytes > 64ull * 1024 * 1024) return (false);
    // The reserved GGTT slots are a fixed window each (xg_flip_init sized them); mapping a larger surface into one
    // would write the PTE range of the next slot.  The adapter can reserve them first with its own size, so check.
    if (gXeFlipSlots && bytes > gXeFlipBytes) { OSAddAtomic64(1, &gCntXeFlipFail); return (false); }
    if (!gXeFlipSlots) {
        unsigned int n = 0;
        if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)3,
                gXeFlipGgtt, &n) != kIOReturnSuccess || n == 0) {
            OSAddAtomic64(1, &gCntXeFlipFail);
            return (false);
        }
        gXeFlipSlots = n; gXeFlipBytes = bytes; gXeFlipNext = 0;
        ALOG("iop: zero-copy flip: %u GGTT slots x %llu bytes (first 0x%llx)", n, bytes, gXeFlipGgtt[0]);
    }
    unsigned int slot = gXeFlipNext % gXeFlipSlots;
    for (unsigned int i = 0; i < gXeFlipSlots; i++) {
        slot = (gXeFlipNext + i) % gXeFlipSlots;
        if (slot != gXeFlipLive) break;
    }
    if (slot == gXeFlipLive) { OSAddAtomic64(1, &gCntXeFlipMiss); return (false); }
    // A-54: a slot that already maps this very surface needs no work -- its PTEs still point at the same
    // pages, and the surface is retained while we are about to arm it.  Verified by the first page, so an
    // id that got recycled for a different surface cannot be mistaken for the old one.
    const unsigned int need = (unsigned int)((bytes + 4095ull) >> 12);
    const uint32_t sid = (uint32_t)s->getSurfaceID();
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
    }
    if (waitIdle) {
        SInt64 idleUs = 0;
        if (!nvAccelXeFlipWaitIdle(&idleUs)) {
            OSAddAtomic64(1, &gCntXeDeferIdleForced);
            OSAddAtomic64(1, &gCntXeFlipFail);
            return false; // Mapping alone never permits scanning unfinished GPU work.
        }
        gXeDeferIdleLastUs = idleUs; if (idleUs > gXeDeferIdleMaxUs) gXeDeferIdleMaxUs = idleUs;
    }
    /*
     * The plane has its own stride and it has to agree with the surface we arm.
     * CoreAnimation allocates its IOSurface rows 256-byte aligned, so a 3120-wide
     * panel gets pitch 12544 while the firmware console scans 12480 -- and the
     * deferred flip in performTransaction() refuses to run unless q.pitch == P.
     * Programming the stride with the surface makes the two equal, which is what
     * this panel needs; on the 2560-wide panels upstream tested the values already
     * matched and this write is a no-op.  Restored with the surf on every exit.
     */
    const uint32_t strideReg = (pitch % 64u == 0u && (pitch / 64u) <= 0x7FFu) ? (pitch / 64u) : 0u;
    const uint32_t f0 = gDispMmio[NV_DISP_FRAME_COUNTER / 4];
    const uint32_t previousSurf = gDispMmio[NV_PLANE_SURF_A / 4];
    const uint32_t previousStride = gDispMmio[NV_PLANE_STRIDE_A / 4];
    if (strideReg) gDispMmio[NV_PLANE_STRIDE_A / 4] = strideReg;
    gDispMmio[NV_PLANE_SURF_A / 4] = (uint32_t)gXeFlipGgtt[slot];	/* the arming write */
    const uint64_t t0 = mach_absolute_time();
    bool latched = false;
    while (nvIopUsBetween(t0, mach_absolute_time()) < 25000) {
        if (gDispMmio[NV_DISP_FRAME_COUNTER / 4] != f0) { latched = true; break; }
        IOSleep(1);
    }
    if (!latched) {
        // The display may still be scanning the previous front. Preserve its
        // reference chain and restore the arming register before returning.
        if (strideReg) gDispMmio[NV_PLANE_STRIDE_A / 4] = previousStride;
        gDispMmio[NV_PLANE_SURF_A / 4] = previousSurf;
        (void)gDispMmio[NV_PLANE_SURF_A / 4];
        gXeFlipUncertain = s; ((OSObject *)s)->retain();
        gIopXeFlip = 0; gIopXeDefer = 0;
        OSAddAtomic64(1, &gCntXeFlipFail);
        return false;
    }
    nvAccelXeFlipDrop(&gXeFlipPrev);
    gXeFlipPrev = gXeFlipFront;
    gXeFlipFront = s; ((OSObject *)s)->retain();
    gXeFlipLive = slot; gXeFlipNext = slot + 1;
    const SInt64 armUs = nvIopUsBetween(t0, mach_absolute_time());
    gXeDeferArmLastUs = armUs; if (armUs > gXeDeferArmMaxUs) gXeDeferArmMaxUs = armUs;
    if (latched) OSAddAtomic64(1, &gCntXeFlip); else OSAddAtomic64(1, &gCntXeFlipFail);
    return (latched);
}
// K8 (synchronous): the old zero-copy flip, no idle wait - kept for A/B against the deferred path (defer=0).
static bool nvAccelXeFlip(IOService *fb, unsigned head, IOSurface *s, unsigned w, unsigned h, unsigned pitch)
{
#ifdef NMX_XEGFX
    // M3d: two writers of PLANE_SURF_1_A must never arm at the same time - the adapter presents into the same plane
    // on its own thread.  The deferred worker coalesces (a present makes it drop the queued frame), this is the A/B
    // synchronous path only.  False just routes the frame through the CPU copy path below.
    if (__atomic_load_n(&gXePresentActive, __ATOMIC_ACQUIRE) != 0) return (false);
#endif
    if (!gIopXeFlip || !gDispMmio || !s || head != 4) return (false);
#ifdef NMX_XEGFX
    if (nvXeIsKmsFB(fb)) {
        if (!gXePresentLock) return false;
        IOLockLock(gXePresentLock);
        if (gXeFlipLock) IOLockLock(gXeFlipLock);
        const bool busy = gXeFlipQHead != gXeFlipQTail ||
            __atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) != 0;
        if (!busy) __atomic_store_n(&gXePresentActive, 1, __ATOMIC_RELEASE);
        if (gXeFlipLock) IOLockUnlock(gXeFlipLock);
        bool ok = !busy && nvAccelXeFlipArm(s, w, h, pitch, false);
        if (!busy) __atomic_store_n(&gXePresentActive, 0, __ATOMIC_RELEASE);
        IOLockUnlock(gXePresentLock);
        return ok;
    }
#endif
    return (nvAccelXeFlipArm(s, w, h, pitch, false));
}
// True while a deferred frame is queued or in flight: WindowServer must not recycle the frame's surface yet.
// If the worker stops making progress the display link must not hang on it (that is what got WindowServer killed by
// the watchdog on 2026-10-26): after 250 ms the busy state is dropped, defer is switched off and the transaction is
// reported complete, so the desktop stays alive and the next frames go through the synchronous path.
static bool nvAccelXeFlipBusy(void)
{
    if (!gIopXeDefer || !gIopXeReportBusy) return (false);
    if (__atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) == 0) return (false);
    const SInt64 since = __atomic_load_n(&gXeFlipBusySince, __ATOMIC_ACQUIRE);
    if (since) {
        const SInt64 age = nvIopUsBetween((uint64_t)since, mach_absolute_time());
        if (age > 250000) {
            OSAddAtomic64(1, &gCntXeDeferForced);
            gIopXeDefer = 0;
            // Only the worker clears gXeFlipBusy. Its frame/pipe references
            // and plane-write exclusion remain live until it actually exits.
            ALOG("iop: deferred flip stuck for %lld us (queued %u armed %lld signals %lld) -- debug.xegfxaccel_iop_xe_defer=0",
                (long long)age, (unsigned)(gXeFlipQTail - gXeFlipQHead),
                (long long)gCntXeDeferArmed, (long long)gCntXeDeferSignals);
            return (false);
        }
    }
    return (true);
}
// Deferred flip worker: pops one queued frame at a time, maps its pages and waits for GT idle in this thread (not
// WindowServer's), arms the plane, clears the busy flag and only then signals the display-pipe transaction interrupt.
// The order matters: WindowServer wakes on the interrupt and calls isTransactionComplete again, and it must already
// see the transaction as complete at that point.  One signal per frame WindowServer handed over.
static void nvAccelXeFlipDeferRun(void *, void *)
{
    for (;;) {
        if (!gXeFlipLock) return;
        NVXeFlipPend p; p.pipe = nullptr; p.s = nullptr; p.w = p.h = p.pitch = 0;
        IOLockLock(gXeFlipLock);
        if (gXeFlipQHead == gXeFlipQTail) {	// nothing queued: idle out and let the display thread start us again
            __atomic_store_n(&gXeFlipBusy, 0, __ATOMIC_RELEASE);
            gXeFlipBusySince = 0;
            IOLockWakeup(gXeFlipLock, (event_t)&gXeFlipBusy, false);
            IOLockUnlock(gXeFlipLock);
            return;
        }
        p = gXeFlipQ[gXeFlipQHead % NV_XE_FLIP_QUEUE];
        gXeFlipQ[gXeFlipQHead % NV_XE_FLIP_QUEUE].s = nullptr;
        gXeFlipQHead++;
        const SInt64 gen = gXeFlipGen;
        IOLockUnlock(gXeFlipLock);

        if (gen != __atomic_load_n(&gXeFlipGen, __ATOMIC_ACQUIRE)) {	// the pipe went away while this frame was queued
            OSAddAtomic64(1, &gCntXeDeferDropped);
            ((OSObject *)p.s)->release();
            ((OSObject *)p.pipe)->release();
            continue;
        }
        const bool ok = nvAccelXeFlipArm(p.s, p.w, p.h, p.pitch, true);
        ((OSObject *)p.s)->release(); // Drop queue ref on both outcomes.
        if (ok) {
            OSAddAtomic64(1, &gCntXeDeferArmed);
            gXeFlipArmRun = 0;
        } else {
            OSAddAtomic64(1, &gCntXeDeferDropped);
            if (++gXeFlipArmRun >= 6 && gIopXeDefer) {
                gIopXeDefer = 0;
                ALOG("iop: 6 deferred frames in a row did not reach the panel -- debug.xegfxaccel_iop_xe_defer=0 (synchronous flip)");
            }
        }
        if (gen != __atomic_load_n(&gXeFlipGen, __ATOMIC_ACQUIRE)) {
            ((OSObject *)p.pipe)->release();
            continue; // Drop the queued pipe reference without signaling it.
        }
        bool last = false;
        IOLockLock(gXeFlipLock);
        last = (gXeFlipQHead == gXeFlipQTail);
        if (last) {		// clear busy *before* the signal, so the waiter that wakes on it sees the completion
            __atomic_store_n(&gXeFlipBusy, 0, __ATOMIC_RELEASE);
            gXeFlipBusySince = 0;
        }
        IOLockUnlock(gXeFlipLock);
        OSAddAtomic64(1, &gCntXeDeferSignals);
        _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(p.pipe, nullptr);
        ((OSObject *)p.pipe)->release();
    }
}
// Display thread: hand the frame to the worker.  One queue entry per display transaction, so the worker signals each
// one; the caller keeps its own reference, this takes one more.  An overflow completes that transaction right away
// (with a device-side partial frame as the price) instead of leaving WindowServer waiting for a wakeup forever.
static void nvAccelXeFlipDeferQueue(void *pipe, IOSurface *s, unsigned w, unsigned h, unsigned pitch)
{
    if (!gXeFlipLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gXeFlipLock)) IOLockFree(l);
    }
    if (!gXeFlipCall) gXeFlipCall = thread_call_allocate(nvAccelXeFlipDeferRun, nullptr);
    if (!gXeFlipLock || !gXeFlipCall) { OSAddAtomic64(1, &gCntXeDeferDropped); return; }
    ((OSObject *)s)->retain();
    ((OSObject *)pipe)->retain();
    bool start = false, overflow = false;
    IOLockLock(gXeFlipLock);
#ifdef NMX_XEGFX
    if (__atomic_load_n(&gXePresentActive, __ATOMIC_ACQUIRE) != 0) {
        // M3d: the adapter is programming the plane itself right now.  Do not queue a compositor frame that would arm
        // the same plane behind its back -- complete this transaction immediately instead (same price as an overflow:
        // the panel may show a device-side partial frame, but WindowServer is never left waiting for a wakeup).
        overflow = true;
    } else
#endif
    if (gXeFlipQTail - gXeFlipQHead >= NV_XE_FLIP_QUEUE) {
        overflow = true;
    } else {
        NVXeFlipPend *q = &gXeFlipQ[gXeFlipQTail % NV_XE_FLIP_QUEUE];
        q->pipe = pipe; q->s = s; q->w = w; q->h = h; q->pitch = pitch;
        gXeFlipQTail++;
        OSAddAtomic64(1, &gCntXeDeferQueued);
        const SInt64 depth = (SInt64)(gXeFlipQTail - gXeFlipQHead);
        if (depth > gXeDeferQMaxDepth) gXeDeferQMaxDepth = depth;
        if (__atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) == 0) {
            __atomic_store_n(&gXeFlipBusy, 1, __ATOMIC_RELEASE);
            gXeFlipBusySince = (SInt64)mach_absolute_time();
            start = true;
        }
    }
    IOLockUnlock(gXeFlipLock);
    if (overflow) {
        OSAddAtomic64(1, &gCntXeDeferDropped);
        ((OSObject *)s)->release();
        OSAddAtomic64(1, &gCntXeDeferSignals);
        _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr);
        ((OSObject *)pipe)->release();
        return;
    }
    if (start) thread_call_enter(gXeFlipCall);
}
#ifdef NMX_XEGFX
// --- M3d implementation: the adapter <-> accelerator present contract (structs/ABI at the top of the file) ---------

// 64-bit extension of the display frame counter (BAR0 0x70040).  The register is 32 bits (it wraps after ~2 years at
// 60 Hz; a polled read can also miss changes), so the state word keeps (generation, last low word) in ONE 64-bit
// atomic: a reader can never observe a torn pair.  The timestamp is the mach time (ns) of the change that produced the
// current generation.  Lock free and safe in any context (the adapter polls this from a timer).
static unsigned long long nvXeVblankNow(uint64_t *nsOut)
{
    if (nsOut) *nsOut = 0;
    if (!gDispMmio) return 0;
    const unsigned int raw = gDispMmio[NV_DISP_FRAME_COUNTER / 4];
    for (int attempt = 0; attempt < 8; attempt++) {
        const unsigned long long st = __atomic_load_n(&gXeVblankState, __ATOMIC_ACQUIRE);
        const unsigned int last = (unsigned int)st;
        const unsigned int gen = (unsigned int)(st >> 32);
        if (raw == last) {
            if (nsOut) *nsOut = __atomic_load_n(&gXeVblankNs, __ATOMIC_ACQUIRE);
            return ((unsigned long long)gen << 32) | raw;
        }
        const unsigned int newGen = gen + (raw < last ? 1u : 0u);
        const unsigned long long nst = ((unsigned long long)newGen << 32) | raw;
        const uint64_t now = mach_absolute_time();
        uint64_t absNs = 0;
        absolutetime_to_nanoseconds(now, &absNs);
        if (__sync_bool_compare_and_swap(&gXeVblankState, st, nst)) {
            OSAddAtomic64(1, &gCntVblankReads);
            if (raw < last) OSAddAtomic64(1, &gCntVblankWraps);
            __atomic_store_n(&gXeVblankNs, (unsigned long long)absNs, __ATOMIC_RELEASE);
            if (nsOut) *nsOut = absNs;
            return nst;
        }
    }
    if (nsOut) *nsOut = __atomic_load_n(&gXeVblankNs, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&gXeVblankState, __ATOMIC_ACQUIRE);
}

// "May this accelerator write PLANE_SURF_1_A for the adapter right now?"  Deliberately strict: the adapter's own
// XeGfxFbReady is the contract point (docs/20-M3D-ACCEL.md).  A present before it is not a failure to log loudly --
// it is simply too early: the adapter retries for ~2 s after enableController.
static bool nvXePresentReady(IOService **fbOut, const char **why)
{
    if (!gXePresent) { if (why) *why = "disabled (debug.xegfxaccel_present=0)"; return false; }
    IOService *fb = gXePresentFB;
    if (!fb) { if (why) *why = "no XeGfxFramebuffer has published itself yet"; return false; }
    if (!gDispMmio) { if (why) *why = "BAR0 display window is not mapped"; return false; }
    if (!nvXeFBReady(fb)) { if (why) *why = "the adapter FB has not published XeGfxFbReady yet"; return false; }
    if (fbOut) *fbOut = fb;
    return true;
}

// B: xgFlipPresent.  Point PLANE_SURF_1_A at the adapter's frame: direct when the adapter knows the GGTT offset of its
// scanout (no PTE writes at all), otherwise map the frame's physical pages into one of the reserved zero-copy GGTT slots
// and point the plane at that slot (exactly like the compositor flip path, except the pages come from a phys address
// instead of an IOSurface).  The frame is never copied.  Returns:
//   kIOReturnSuccess       the plane was programmed (with flags bit DONE; LATCHED when a vblank was seen)
//   kIOReturnNotReady      the adapter FB is not ready / API not armed: no register was touched
//   kIOReturnBusy          the K9b deferred flip worker owns the plane: no register write, retry
//   kIOReturnBadArgument   the frame does not fit the contract
//   kIOReturnTimeout       written but no vblank inside 25 ms: the previous frame was put back
//   kIOReturnNoDevice      the read-back did not match: the previous frame was put back
static IOReturn nvXePresent(struct XeGfxPresent *p)
{
    if (!p) { OSAddAtomic64(1, &gCntPresentBadArg); return kIOReturnBadArgument; }
    // The adapter header ships this struct as "version 1" (XEGFX_M3D_PRESENT_VERSION 1u) while its layout is
    // byte-for-byte the version-2 ABI, so both spellings are accepted: 1 = the adapter's legacy spelling, 2 = the
    // contract spelling now sent by the adapter (verified 2026-10-28 against XeGfxDisplay.h:241-259 + static_asserts;
    // the adapter's own m3dPresent() accepts 1|2 the same way).  Size is the real layout check; version is 1 or 2.
    if ((p->version != NVXE_ABI_VERSION && p->version != 1u) ||
        p->size != (unsigned int)sizeof(struct XeGfxPresent)) {
        OSAddAtomic64(1, &gCntPresentBadArg);
        if (gCntPresentBadArg <= 8)
            ALOG("present: ABI mismatch (v%u size %u, expected v%u|1 size %u) -- ignored", p->version, p->size,
                 NVXE_ABI_VERSION, (unsigned int)sizeof(struct XeGfxPresent));
        return kIOReturnBadArgument;
    }
    IOService *fb = nullptr;
    const char *why = "?";
    if (!nvXePresentReady(&fb, &why)) {
        OSAddAtomic64(1, &gCntPresentNotReady);
        if (gCntPresentNotReady <= 8)
            ALOG("present: not ready (%s), flip %llu -- previous frame kept; the adapter retries", why,
                 (unsigned long long)p->flipCount);
        return kIOReturnNotReady;
    }
    const unsigned int w = p->width, h = p->height, pitch = p->pitch;
    const unsigned long long bytes = (unsigned long long)pitch * (unsigned long long)h;
    const bool fullRegion = (p->regionW == 0) ||
        (p->regionX == 0 && p->regionY == 0 && p->regionW == w && p->regionH == h);
    // head==0 is also accepted as head 4: the adapter header documents "4 (0 => 4)".
    if ((p->head != 4 && p->head != 0) || w == 0 || h == 0 || w > 16384 || h > 16384 || pitch < 4ull * w || (pitch & 3u) ||
        bytes == 0 || bytes > 64ull * 1024 * 1024 || !fullRegion ||
        (p->format != 0 && p->format != NVXE_FORMAT_XRGB8888) || (!p->ggtt && !p->phys)) {
        OSAddAtomic64(1, &gCntPresentBadArg);
        if (gCntPresentBadArg <= 8)
            ALOG("present: bad frame (head %u %ux%u pitch %u fmt 0x%x phys 0x%llx ggtt 0x%x region %u,%u %ux%u)"
                 " -- ignored, previous frame kept", p->head, w, h, pitch, p->format, p->phys, p->ggtt,
                 p->regionX, p->regionY, p->regionW, p->regionH);
        return kIOReturnBadArgument;
    }
    // NO_WAIT spelling: v2 callers use bit 1; the adapter's v1 header defines NO_WAIT as bit 0 (XEGFX_M3D_PRESENT_NO_WAIT
    // 1u) while bit 0 means WAIT_LATCH in the v2 spelling, so bit 0 is honoured as NO_WAIT only for version-1 requests.
    const bool noWait = ((p->flags & NVXE_PRESENT_NO_WAIT) != 0) ||
                        (p->version == 1u && (p->flags & NVXE_PRESENT_WAIT_LATCH) != 0);
    if (!gXePresentLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gXePresentLock)) IOLockFree(l);
    }
    if (!gXePresentLock) { OSAddAtomic64(1, &gCntPresentFail); return kIOReturnNoMemory; }
    IOLockLock(gXePresentLock);
    if (gXeFlipLock) IOLockLock(gXeFlipLock);
    const bool deferBusy = (gXeFlipQHead != gXeFlipQTail) ||
                           (__atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) != 0);
    if (!deferBusy) __atomic_store_n(&gXePresentActive, 1, __ATOMIC_RELEASE);
    if (gXeFlipLock) IOLockUnlock(gXeFlipLock);
    if (deferBusy) {
        IOLockUnlock(gXePresentLock);
        OSAddAtomic64(1, &gCntPresentBusy);
        if (gCntPresentBusy <= 8)
            ALOG("present: the K9b deferred flip worker owns the plane -- kIOReturnBusy, retry");
        return kIOReturnBusy;
    }
    IOReturn rc = kIOReturnSuccess;
    const uint64_t tStart = mach_absolute_time();
    const uint32_t prevSurf = gDispMmio[NV_PLANE_SURF_A / 4];
    const uint32_t f0 = gDispMmio[NV_DISP_FRAME_COUNTER / 4];
    unsigned int wroteGgtt = 0;
    unsigned int presentSlot = ~0u;	/* the reserved slot this present mapped, if any */
    bool latched = false;
    static unsigned long long pages[NV_XE_FLIP_MAX_PAGES];
    do {
        if (p->ggtt) {
            if (p->ggtt & 0xffu) { rc = kIOReturnBadArgument; break; }
            wroteGgtt = p->ggtt;
            gDispMmio[NV_PLANE_SURF_A / 4] = wroteGgtt;
            OSAddAtomic64(1, &gCntPresentGgttWrites);
        } else if (p->phys) {
            if (p->phys >= 0x100000000ull || (p->phys & 0xfffull)) { rc = kIOReturnBadArgument; break; }
            if (!gXeFlipSlots) {
                unsigned int n = 0;
                if (nvAccelResidentCall("xgFlipInit", (void *)(uintptr_t)bytes, (void *)(uintptr_t)3,
                        gXeFlipGgtt, &n) != kIOReturnSuccess || n == 0) {
                    rc = kIOReturnNotReady;
                    break;
                }
                gXeFlipSlots = n; gXeFlipBytes = bytes; gXeFlipNext = 0;
                ALOG("present: reserved %u zero-copy GGTT slots x %llu bytes for adapter frames", n, bytes);
            } else if (bytes > gXeFlipBytes) {
                rc = kIOReturnBadArgument;
                break;
            }
            unsigned int slot = gXeFlipNext % gXeFlipSlots;
            for (unsigned int i = 0; i < gXeFlipSlots; i++) {
                slot = (gXeFlipNext + i) % gXeFlipSlots;
                if (slot != gXeFlipLive) break;
            }
            if (slot == gXeFlipLive) { rc = kIOReturnBusy; break; }
            const unsigned int npages = (unsigned int)((bytes + 4095ull) >> 12);
            if (npages == 0 || npages > NV_XE_FLIP_MAX_PAGES) { rc = kIOReturnBadArgument; break; }
            const unsigned long long base = p->phys & ~0xfffull;
            for (unsigned int i = 0; i < npages; i++) pages[i] = base + ((unsigned long long)i << 12);
            const IOReturn mapRc = nvAccelResidentCall("xgFlipMap", (void *)(uintptr_t)slot, pages,
                                                       (void *)(uintptr_t)npages, (void *)(uintptr_t)bytes);
            if (mapRc != kIOReturnSuccess) { rc = mapRc; break; }
            wroteGgtt = (unsigned int)gXeFlipGgtt[slot];
            gDispMmio[NV_PLANE_SURF_A / 4] = wroteGgtt;
            presentSlot = slot;
            OSAddAtomic64(1, &gCntPresentPhysWrites);
        } else {
            rc = kIOReturnBadArgument;
            break;
        }
        if (gDispMmio[NV_PLANE_SURF_A / 4] != wroteGgtt) {
            const uint32_t got = gDispMmio[NV_PLANE_SURF_A / 4];
            if (prevSurf) gDispMmio[NV_PLANE_SURF_A / 4] = prevSurf;
            rc = kIOReturnNoDevice;
            ALOG("present: PLANE_SURF_1_A read back 0x%x, expected 0x%x -- previous frame put back", got, wroteGgtt);
            break;
        }
        if (!noWait) {
            while (nvIopUsBetween((uint64_t)tStart, mach_absolute_time()) < 25000) {
                if (gDispMmio[NV_DISP_FRAME_COUNTER / 4] != f0) { latched = true; break; }
                IOSleep(1);
            }
            if (!latched) {
                if (prevSurf) gDispMmio[NV_PLANE_SURF_A / 4] = prevSurf;
                OSAddAtomic64(1, &gCntPresentLatchTimeouts);
                rc = kIOReturnTimeout;
                if (gCntPresentLatchTimeouts <= 8)
                    ALOG("present: no vblank inside 25 ms (flip %llu) -- previous frame put back",
                         (unsigned long long)p->flipCount);
                break;
            }
        }
        // Only now does the plane really scan out this frame: the reserved slot becomes the live one, so the
        // compositor flip path will pick a different slot.  Before this point a failure restored the previous frame.
        if (presentSlot != ~0u) { gXeFlipLive = presentSlot; gXeFlipNext = presentSlot + 1; }
        p->flags |= NVXE_PRESENT_DONE;
        if (latched) p->flags |= NVXE_PRESENT_LATCHED;
        gXePresentLast.phys = p->phys;
        gXePresentLast.ggtt = wroteGgtt;
        gXePresentLast.pitch = pitch;
        gXePresentLast.width = w;
        gXePresentLast.height = h;
        gXePresentLast.format = p->format;
        gXePresentLast.flipCount = p->flipCount;
        OSAddAtomic64(1, &gCntPresentOk);
        const SInt64 us = nvIopUsBetween((uint64_t)tStart, mach_absolute_time());
        gXePresentLastUs = us;
        if (us > gXePresentMaxUs) gXePresentMaxUs = us;
        if (gCntPresentOk == 1)
            ALOG("present: first adapter frame on head 4: %ux%u pitch %u -> ggtt 0x%x (flip %llu, %lld us, latched %d)",
                 w, h, pitch, wroteGgtt, (unsigned long long)p->flipCount, (long long)us, latched ? 1 : 0);
    } while (0);
    if (gXeFlipLock) IOLockLock(gXeFlipLock);
    __atomic_store_n(&gXePresentActive, 0, __ATOMIC_RELEASE);
    if (gXeFlipLock) IOLockUnlock(gXeFlipLock);
    IOLockUnlock(gXePresentLock);
    if (rc != kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntPresentFail);
        if (gCntPresentFail <= 8)
            ALOG("present: failed rc 0x%x (flip %llu phys 0x%llx ggtt 0x%x) -- previous frame still on the plane",
                 rc, (unsigned long long)p->flipCount, p->phys, p->ggtt);
    }
    return rc;
}

// The adapter's vblank source.  Both calls read the same pair; only the argument order differs (ABI block at the top):
// xgVblankCount(p1 = uint64 *count, p2 = uint64 *ns), xgVblankTime(p1 = uint64 *ns, p2 = uint64 *count).
static IOReturn nvXeVblankCount(void *p1, void *p2)
{
    uint64_t ns = 0;
    const unsigned long long count = nvXeVblankNow(&ns);
    if (p1) *(unsigned long long *)p1 = count;
    if (p2) *(unsigned long long *)p2 = ns;
    return kIOReturnSuccess;
}
static IOReturn nvXeVblankTime(void *p1, void *p2)
{
    uint64_t ns = 0;
    const unsigned long long count = nvXeVblankNow(&ns);
    if (p1) *(unsigned long long *)p1 = ns;
    if (p2) *(unsigned long long *)p2 = count;
    return kIOReturnSuccess;
}

// B: xgGetPresentInfo (diagnostics: last programmed frame, live PLANE_SURF_1_A, gates).
static IOReturn nvXeGetPresentInfo(struct XeGfxPresentInfo *pi)
{
    if (!pi) return kIOReturnBadArgument;
    bzero(pi, sizeof(struct XeGfxPresentInfo));
    pi->version = NVXE_ABI_VERSION;
    pi->size = (unsigned int)sizeof(struct XeGfxPresentInfo);
    pi->head = 4;
    if (gXePresentLock) IOLockLock(gXePresentLock);
    pi->phys = gXePresentLast.phys;
    pi->flipCount = gXePresentLast.flipCount;
    pi->pitch = gXePresentLast.pitch;
    pi->width = gXePresentLast.width;
    pi->height = gXePresentLast.height;
    pi->format = gXePresentLast.format;
    if (gXePresentLock) IOLockUnlock(gXePresentLock);
    uint64_t ns = 0;
    pi->vblankCount = nvXeVblankNow(&ns);
    pi->vblankNs = ns;
    IOService *fb = nullptr;
    const char *why = "?";
    if (nvXePresentReady(&fb, &why)) pi->flags |= NVXE_PRESENT_INFO_READY;
    if (gXeFlipQHead != gXeFlipQTail || __atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) != 0)
        pi->flags |= NVXE_PRESENT_INFO_BUSY;
    pi->planeSurfGgtt = gDispMmio ? gDispMmio[NV_PLANE_SURF_A / 4] : 0;
    return kIOReturnSuccess;
}

// B: xgCursorImage skeleton (M4 fills it in).  Validate so the adapter side can integrate against the real ABI, log
// the first few requests and touch no register: a cursor plane programmed before the adapter's mode is up is exactly
// the kind of early register write M3d must not do.
static IOReturn nvXeCursor(struct XeGfxCursorImage *ci)
{
    OSAddAtomic64(1, &gCntCursorRequests);
    if (!ci) return kIOReturnBadArgument;
    if (ci->version != NVXE_ABI_VERSION || ci->size != (unsigned int)sizeof(struct XeGfxCursorImage) ||
        ci->width == 0 || ci->height == 0 || ci->width > 256 || ci->height > 256 || !(ci->ggtt || ci->phys))
        return kIOReturnBadArgument;
    if (gCntCursorRequests <= 8)
        ALOG("cursor: %ux%u fmt 0x%x phys 0x%llx ggtt 0x%x hotspot %u,%u -- validated, not programmed (M4)",
             ci->width, ci->height, ci->format, ci->phys, ci->ggtt, ci->hotspotX, ci->hotspotY);
    return kIOReturnUnsupported;
}

// A: xgGetScanoutInfo on the adapter FB (80-byte v2).  Called from askFramebufferForScanout() for the display-pipe
// resource: the frame becomes the pipe's vidmem under the SAME physical address, and the zero-copy flip path then arms
// PLANE_SURF at it.  NotReady means the adapter has not allocated/presented its scanout yet; the pipe must then get no
// resource at all (NOT the boot aperture -- that fallback belongs to the placeholder path only, docs/20).
static IOReturn nvXeKmsScanoutInfo(IOService *fb, struct NVRMVramRequest *r)
{
    if (!fb || !r) return kIOReturnBadArgument;
    struct XeGfxScanoutInfo si;
    bzero(&si, sizeof(si));
    si.version = NVXE_ABI_VERSION;
    si.size = (unsigned int)sizeof(si);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("xgGetScanoutInfo");
    if (!sym) return kIOReturnNoMemory;
    const IOReturn rc = fb->callPlatformFunction(sym, false, &si, nullptr, nullptr, nullptr);
    sym->release();
    if (rc != kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntKmsScanoutNotReady);
        if (gCntKmsScanoutNotReady <= 8)
            ALOG("scanout: %s not ready (rc 0x%x) -- no display-pipe resource yet", fb->getName(), rc);
        return rc;
    }
    if (si.version != NVXE_ABI_VERSION || si.size != (unsigned int)sizeof(si) ||
        !(si.flags & NVXE_SCANOUT_READY) || si.phys == 0 || si.pitch == 0 || si.width == 0 || si.height == 0 ||
        si.pitch < 4ull * si.width) {
        OSAddAtomic64(1, &gCntKmsScanoutNotReady);
        if (gCntKmsScanoutNotReady <= 8)
            ALOG("scanout: %s descriptor not ready (v%u size %u flags 0x%x phys 0x%llx %ux%u pitch %u)",
                 fb->getName(), si.version, si.size, si.flags, si.phys, si.width, si.height, si.pitch);
        return kIOReturnNotReady;
    }
    bzero(r, sizeof(*r));
    r->version = NVRM_VRAM_ABI_VERSION;
    r->phys = si.phys;
    r->kva = (void *)si.kva;
    r->width = si.width;
    r->height = si.height;
    r->pitch = si.pitch;
    r->size = (unsigned long long)si.pitch * si.height;
    r->actualSize = r->size;
    OSAddAtomic64(1, &gCntKmsScanoutOk);
    if (gCntKmsScanoutOk == 1)
        ALOG("scanout: display-pipe resource over the adapter scanout: %ux%u pitch %u at 0x%llx (%s)",
             si.width, si.height, si.pitch, si.phys, (si.flags & NVXE_SCANOUT_GGTT) ? "ggtt" : "phys");
    return kIOReturnSuccess;
}

// A completed CPU copy must become the displayed buffer again if an earlier
// zero-copy frame moved the plane away. Reuse the adapter present lock/latch
// and rollback path; a busy worker leaves the previous frame on screen.
static IOReturn nvXeRestoreCopyTarget(IOService *fb)
{
    if (!nvXeIsKmsFB(fb)) return kIOReturnSuccess;
    XeGfxScanoutInfo si = {};
    si.version = NVXE_ABI_VERSION; si.size = sizeof(si);
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("xgGetScanoutInfo");
    if (!sym) return kIOReturnNoMemory;
    const IOReturn rc = fb->callPlatformFunction(sym, false, &si, nullptr, nullptr, nullptr);
    sym->release();
    if (rc != kIOReturnSuccess) return rc;
    if (si.version != NVXE_ABI_VERSION || si.size != sizeof(si) ||
        !(si.flags & NVXE_SCANOUT_READY) || !si.phys || !si.kva || !si.ggtt ||
        !si.width || !si.height || si.pitch < 4ull * si.width) return kIOReturnNotReady;
    if (!gXeFlipUncertain && gDispMmio && gDispMmio[NV_PLANE_SURF_A / 4] == si.ggtt) return kIOReturnSuccess;
    XeGfxPresent frame = {};
    frame.version = NVXE_ABI_VERSION; frame.size = sizeof(frame);
    frame.phys = si.phys; frame.kva = si.kva; frame.ggtt = si.ggtt;
    frame.width = si.width; frame.height = si.height; frame.pitch = si.pitch;
    frame.regionW = si.width; frame.regionH = si.height;
    frame.format = si.format; frame.head = 4;
    // Zero flags select the bounded latch wait; NO_WAIT is deliberately clear.
    const IOReturn presentRc = nvXePresent(&frame);
    if (presentRc == kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntIopFlipHome);
        // The adapter buffer is now latched: retired compositor pages can
        // safely leave the display's front/previous/uncertain reference set.
        nvAccelXeFlipDrop(&gXeFlipUncertain);
        nvAccelXeFlipDrop(&gXeFlipPrev);
        nvAccelXeFlipDrop(&gXeFlipFront);
        gXeFlipLive = ~0u;
    }
    return presentRc;
}

// B discovery: the adapter property-matches XeGfxAccelPresent on the IOService plane (and the nub).  Published as soon
// as the adapter FB is parked or its head-4 pipe binds -- before XeGfxFbReady on purpose, so the adapter's very first
// xgFlipPresent can reach us; the register-write gate itself is XeGfxFbReady inside nvXePresent().
static bool nvXePublishPresentAPI(IOService *fb, const char *why, IOService *accel)
{
    if (!fb) return false;
    if (gXePresentFB != fb) {
        if (gXePresentFB) ((OSObject *)gXePresentFB)->release();
        gXePresentFB = fb;
        ((OSObject *)fb)->retain();
        gXePresentPublished = 0;
    }
    OSNumber *head = OSNumber::withNumber((unsigned long long)4, 32);
    IOService *targets[2];
    targets[0] = accel;
    targets[1] = fb->getProvider();
    for (int i = 0; i < 2; i++) {
        IOService *s = targets[i];
        if (!s) continue;
        s->setProperty("XeGfxAccelPresent", kOSBooleanTrue);
        if (head) s->setProperty("XeGfxAccelPresentHead", head);
    }
    if (head) head->release();
    if (!gXePresentPublished) {
        gXePresentPublished = 1;
        ALOG("present: API armed on %s (%s) -- the adapter may now call xgFlipPresent", fb->getName(), why);
    }
    return true;
}

// PerformTransaction gate: the placeholder path keeps exactly the code it always ran; the adapter FB may only have the
// plane armed for it when it is the framebuffer that published the API and its ready property is up.  A compositor
// frame must never be armed onto a plane the adapter has not claimed.
static bool nvXePlaneWriteAllowed(IOService *fb)
{
    if (!nvXeIsKmsFB(fb)) return true;
    if (!gXePresentFB || fb != gXePresentFB) return false;
    return nvXeFBReady(fb);
}
#else	/* !NMX_XEGFX */
// M3d exists only in the XeGfx build: the other products keep the display-pipe gate exactly as it was.
static bool nvXePlaneWriteAllowed(IOService *fb) { (void)fb; return (true); }
#endif
#define NVIOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define NVIOPLOG(fmt, ...) do { if (gIopLogs < NVIOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define NVIOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); NVIOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

void NVDisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        NVIopSrc *e = &fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void NVDisplayPipe::free()
{
    iopDropSources();
    NVXeFlipPend retired[NV_XE_FLIP_QUEUE] = {};
    unsigned retiredCount = 0;
    if (gXeFlipLock) {
        IOLockLock(gXeFlipLock);
        __atomic_add_fetch(&gXeFlipGen, 1, __ATOMIC_ACQ_REL);
        while (gXeFlipQHead != gXeFlipQTail && retiredCount < NV_XE_FLIP_QUEUE) {
            NVXeFlipPend *q = &gXeFlipQ[gXeFlipQHead % NV_XE_FLIP_QUEUE];
            retired[retiredCount++] = *q;
            q->s = nullptr; q->pipe = nullptr;
            gXeFlipQHead++;
        }
        gXeFlipQHead = gXeFlipQTail = 0;
        IOLockUnlock(gXeFlipLock);
        // Releasing another pipe can enter its free(); never do it under
        // the shared queue lock. An in-flight worker owns its own references.
        for (unsigned i = 0; i < retiredCount; ++i) {
            if (retired[i].s) ((OSObject *)retired[i].s)->release();
            if (retired[i].pipe) ((OSObject *)retired[i].pipe)->release();
        }
    }
    if (__atomic_load_n(&gXeFlipBusy, __ATOMIC_ACQUIRE) == 0 && !gXeFlipUncertain) {
        nvAccelXeFlipDrop(&gXeFlipPrev);
        nvAccelXeFlipDrop(&gXeFlipFront);
    } else {
        ALOG("iop: live worker retains the front chain during pipe teardown");
    }
    IOAccelLegacyDisplayPipe::free();
}

const void *NVDisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < kIopSrcSlots; i++)
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map)
            return (fIopSrc[i].map->getLength() >= need) ? (const void *)fIopSrc[i].map->getVirtualAddress() : nullptr;
    if (md->getLength() < need) return nullptr;
    NVIopSrc *e = &fIopSrc[fIopNext % kIopSrcSlots]; fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    NVIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (fIopNext - 1) % kIopSrcSlots);
    return (const void *)m->getVirtualAddress();
}

#ifdef NMX_XEGFX
extern SInt64 gCntPipeInit, gCntPipeValidate, gCntPipeComplete, gCntPipePerform;
#define NVPIPE_CNT(v) OSIncrementAtomic64(&(v))
#else
#define NVPIPE_CNT(v) do { } while (0)
#endif
IOReturn NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    NVPIPE_CNT(gCntPipeValidate);
    return kIOReturnSuccess;
}

static bool nvAccelIopAsyncBusy(const void *pipe);
static bool nvAccelXeFlipBusy(void);
bool NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    NVPIPE_CNT(gCntPipeComplete);
    return gNvIop != 0 && !nvAccelIopAsyncBusy(this) && !nvAccelXeFlipBusy();
}

static void nvAccelSurfScanoutSync(IOSurface *s);
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head);
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static bool nvSurfWaitReady(unsigned int id);
static void nvAccelIopHome(IOService *fb, unsigned head);
IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    NVPIPE_CNT(gCntPipePerform);
    const uint64_t txnStart = mach_absolute_time();
    if (gIopLastTxnEnd) nvIopStat(&gIopGapLastUs, &gIopGapMaxUs, &gIopGapSumUs, nvIopUsBetween(gIopLastTxnEnd, txnStart));
    if (!gNvIop) return (IOReturn)0xe00002c7;
    if (!txn) NVIOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
    NVAccel *a = NVAccel::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    if (!fb) NVIOP_FAIL(kIOReturnNotAttached, "no accelerator, or pipe %p drives a framebuffer that is none of ours", this);

    IOSurface *s  = txn->getPlaneIOSurface(0, 0);
    OSObject  *ro = txn->getPlaneResource(0, 0);
    if (!s && !ro) { OSAddAtomic64(1, &gCntIopEmpty); return kIOReturnSuccess; }

    // K9: with the no-wait compositor this frame may still be under the GPU.  Hand it to the flip worker instead of
    // waiting here: WindowServer's display thread stays free, and the worker writes PLANE_SURF only after the GPU
    // reports idle.  The frame's geometry has to be the scanout's, as in the synchronous flip below.
    if (gIopXeDefer && gIopXeFlip && s && head == 4 && nvXePlaneWriteAllowed(fb)) {
        const unsigned W = (unsigned)s->getWidth(), H = (unsigned)s->getHeight(), P = (unsigned)s->getBytesPerRow();
        if (W && H && W <= 16384 && H <= 16384 && P >= 4 * W && !(P & 3) && (unsigned long long)P * H <= 64ull * 1024 * 1024) {
            struct NVRMVramRequest q;
            for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
            q.version = NVRM_VRAM_ABI_VERSION;
            const IOReturn qr = a->askFramebufferForScanout(&q, fb);
            if (qr == kIOReturnSuccess && q.kva && q.pitch == P && q.height &&
                W == q.width && H == q.height && s->getPixelFormat() == 0x42475241u) {
                nvAccelXeFlipDeferQueue(this, s, W, H, P);
                nvIopStat(&gIopTxnLastUs, &gIopTxnMaxUs, &gIopTxnSumUs, nvIopUsBetween(txnStart, mach_absolute_time()));
                gIopLastTxnEnd = mach_absolute_time();
                return kIOReturnSuccess;
            }
        }
    }

    const volatile unsigned int *src = nullptr; unsigned w = 0, h = 0, pitch = 0; bool fromRes = false;
    NVResource *res = OSDynamicCast(NVResource, ro);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    static SInt32 gIopK6Logs;
    if (gIopK6Logs < 6) {
        OSIncrementAtomic(&gIopK6Logs);
        const unsigned char *kr = (const unsigned char *)res;
        ALOG("iop: K6 plane 0: surface %s id 0x%x %ux%u row %u fmt 0x%08x | resource %s w %u h %u +0xb8 %u +0xc0 %u vidmem %llu bytes kva %d scanout %d",
             s ? "yes" : "NO", s ? (unsigned)s->getSurfaceID() : 0u, s ? (unsigned)s->getWidth() : 0u, s ? (unsigned)s->getHeight() : 0u,
             s ? (unsigned)s->getBytesPerRow() : 0u, s ? (unsigned)s->getPixelFormat() : 0u,
             ro ? ro->getMetaClass()->getClassName() : "(none)",
             res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb0) : 0u, res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb2) : 0u,
             res ? *(const volatile unsigned int *)(kr + 0xb8) : 0u, res ? *(const volatile unsigned int *)(kr + 0xc0) : 0u,
             vm ? (unsigned long long)vm->fBytes : 0ull, (vm && vm->fKva) ? 1 : 0, vm ? (int)vm->fScanout : -1);
    }
    if (!s && vm && vm->fKva) {
        const unsigned char *rr = (const unsigned char *)res;
        w     = *(const volatile unsigned short *)(rr + 0xb0);
        h     = *(const volatile unsigned short *)(rr + 0xb2);
        pitch = *(const volatile unsigned int   *)(rr + 0xb8);
        if ((unsigned long long)pitch < 4ull * w) NVIOP_FAIL(kIOReturnBadArgument, "resource %ux%u: +0xb8 reads %u, which is not a row pitch (need >= %u) — not a surface buffer", w, h, pitch, 4u * w);
        if ((unsigned long long)h * pitch > vm->fBytes) NVIOP_FAIL(kIOReturnOverrun, "resource %ux%u pitch %u exceeds its %llu bytes", w, h, pitch, (unsigned long long)vm->fBytes);
        src = (const volatile unsigned int *)vm->fKva; fromRes = true;
    } else if (s && nvAccelIopFlip(fb, head, s, this)) {
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        return kIOReturnSuccess;
    } else if (s) {
        // A copied frame must come from the surface's newest pixels. Copying while they were still only in VRAM put a
        // stale, one-colour buffer on the panel every other frame: full-screen flicker, 5 of 40 flicker recordings
        // (1.13-1.14.1), nvaccel_iop_blank about half of every copied frame, never on machines whose frames all flip.
        // Such a frame now keeps the previous picture on the panel instead.
        if (!nvSurfWaitReady(s->getSurfaceID())) {
            OSAddAtomic64(1, &gCntIopLate);
            return kIOReturnSuccess; // Keep prior pixels; timeout is not GPU completion.
        }
        if (!nvAccelSurfCopyReady(s, head)) return kIOReturnSuccess;
        const unsigned fmt = s->getPixelFormat();
        if (fmt != 0x42475241u) NVIOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
        const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
        if (!W || !H || P < 4 || W > 16384 || H > 16384 || P > 16384ull * 8) NVIOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
        if (P < 4 * W || (P & 3)) NVIOP_FAIL(kIOReturnBadArgument, "surface %llux%llu: row pitch %llu is under 4*W or not word-aligned", W, H, P);
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!md) NVIOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
        const uint64_t mapStart = mach_absolute_time();
        src = (const volatile unsigned int *)iopMapSurface(s->getSurfaceID(), md, H * P);
        nvIopStat(&gIopMapLastUs, &gIopMapMaxUs, &gIopMapSumUs, nvIopUsBetween(mapStart, mach_absolute_time()));
        if (!src) NVIOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
        w = (unsigned)W; h = (unsigned)H; pitch = (unsigned)P;
    } else {
        NVIOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an NVResource with a CPU mapping, and there is no IOSurface", ro);
    }
    if (!w || !h || pitch < 4) NVIOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || q.pitch < 4 || !q.height) NVIOP_FAIL(kIOReturnNotReady, "framebuffer would not describe its scanout (0x%x kva %p pitch %u)", (unsigned)ret, q.kva, q.pitch);
    if (w != q.width || h != q.height) NVIOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, q.width, q.height);

    if (!gIopXeDefer && gIopXeFlip && s && pitch == q.pitch && nvXePlaneWriteAllowed(fb) && nvAccelXeFlip(fb, head, s, w, h, pitch)) {
        nvAccelIopHome(fb, head);
        nvIopStat(&gIopTxnLastUs, &gIopTxnMaxUs, &gIopTxnSumUs, nvIopUsBetween(txnStart, mach_absolute_time()));
        gIopLastTxnEnd = mach_absolute_time();
        return kIOReturnSuccess;
    }

    nvAccelIopHome(fb, head);
    nvAccelIopWaitVblank();
    const unsigned int surfID = s ? (unsigned)s->getSurfaceID() : 0u;
    const unsigned int frameNow = gDispMmio ? gDispMmio[NV_DISP_FRAME_COUNTER / 4] : 0u;
    if (gIopLastSurf && surfID == gIopLastSurf) OSAddAtomic64(1, &gCntIopSurfRepeat);
    if (gIopLastFrameAtCopy && frameNow == gIopLastFrameAtCopy) OSAddAtomic64(1, &gCntIopSameRefresh);
    gIopLastSurf = surfID;
    gIopLastFrameAtCopy = frameNow;
    const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    unsigned preSample[4] = {};
    if (src) {
        preSample[0] = src[0];
        preSample[1] = src[(unsigned long)(h / 2) * spr + (spr / 2)];
        preSample[2] = src[(unsigned long)(h - 1) * spr + (spr - 1)];
        preSample[3] = src[(unsigned long)(h / 4) * spr];
    }
    const uint64_t copyStart = mach_absolute_time();
    if (gIopMemcpy) {
        if (q.pitch == pitch) {
            memcpy((void *)q.kva, (const void *)src, (size_t)h * (size_t)pitch);
        } else {
            for (unsigned y = 0; y < h; y++)
                memcpy((char *)q.kva + (unsigned long)y * q.pitch,
                       (const char *)src + (unsigned long)y * pitch, (size_t)words * 4);
        }
    } else {
        volatile unsigned int *d = (volatile unsigned int *)q.kva;
        for (unsigned y = 0; y < h; y++) {
            const volatile unsigned int *sp = src + (unsigned long)y * spr;
            volatile unsigned int *dp = d + (unsigned long)y * dpr;
            for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
        }
    }
#ifdef NMX_XEGFX
    if (nvXeIsKmsFB(fb) && nvXeRestoreCopyTarget(fb) != kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntIopLate);
        return kIOReturnSuccess; // Keep the prior frame, without claiming a displayed copy.
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
    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));
    if (gIopCopyLastUs > 12000) OSAddAtomic64(1, &gCntIopCopyOverrun);
    {
        const unsigned int frameEnd = gDispMmio ? gDispMmio[NV_DISP_FRAME_COUNTER / 4] : 0u;
        if (frameNow && frameEnd && frameEnd != frameNow) OSAddAtomic64(1, &gCntIopCopyCross);
        if (src) {
            unsigned d = 0;
            if (src[0] != preSample[0]) d++;
            if (src[(unsigned long)(h / 2) * spr + (spr / 2)] != preSample[1]) d++;
            if (src[(unsigned long)(h - 1) * spr + (spr - 1)] != preSample[2]) d++;
            if (src[(unsigned long)(h / 4) * spr] != preSample[3]) d++;
            if (d) OSAddAtomic64(1, &gCntIopSrcChanged);
        }
    }

    { static SInt32 traceN; if ((traceN++ & 0x7f) == 0)
        ALOG("iop: trace surf 0x%x frame %u copy %lld vblank %lld txn %lld",
             surfID, frameNow, (long long)gIopCopyLastUs, (long long)gIopVblankLastUs, (long long)gIopTxnLastUs); }
    OSAddAtomic64(1, fromRes ? &gCntIopSrcRes : &gCntIopSrcSurf);
    const SInt64 n = OSAddAtomic64(1, &gCntIopOk);
    const SInt64 nh = OSAddAtomic64(1, &gCntIopOkHead[head]);
    unsigned nz = 0, varied = 0; const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        const unsigned v = src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu;
        if (v) nz++;
        if (v != first) varied++;
    }
    if (!varied) OSAddAtomic64(1, &gCntIopBlank);
    if ((n <= 3 || n == 600 || nh == 0) && gIopK6Logs < 12) {
        OSIncrementAtomic(&gIopK6Logs);
        ALOG("iop: K6 transaction #%lld (head %u #%lld) on the panel: %ux%u pitch %u from %s — 64 samples: nonzero %u varied %u", (long long)n, head, (long long)nh, w, h, pitch,
             fromRes ? "our VRAM resource" : "the IOSurface's memory", nz, varied);
    }
    nvIopStat(&gIopTxnLastUs, &gIopTxnMaxUs, &gIopTxnSumUs, nvIopUsBetween(txnStart, mach_absolute_time()));
    gIopLastTxnEnd = mach_absolute_time();
    return kIOReturnSuccess;
}

// The body of debug.xegfxaccel_iop, shared with the boot-armed sequence so both take exactly the same actions.
static void nvaccelSetIop(int v)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) { ALOG("iop: OSDictionary::withCapacity failed -- capabilities NOT published"); return; }
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gNvIop = 1;
        ALOG("iop: debug.xegfxaccel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gNvIop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.xegfxaccel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
}
static int nvacceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvIop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    if (!NVAccel::gAccel) return ENXIO;
    nvaccelSetIop(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int nvaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvAccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I",
            "register the NVIDIA framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define NVACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
NVACCEL_CNT(nvaccel_vm_alloc,         gCntVmAlloc,        "NVVidMemory::allocPhysical successes");
NVACCEL_CNT(nvaccel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to NVVidMemory");
NVACCEL_CNT(nvaccel_vm_dealloc,       gCntVmDealloc,      "NVVidMemory::deallocPhysical calls with a handle");
NVACCEL_CNT(nvaccel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by NVVidMemory");
NVACCEL_CNT(nvaccel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_vm_sticky, CTLFLAG_RW | CTLFLAG_LOCKED, &gVmSticky, 0, "1 = a surface keeps its VRAM across the family's dealloc/alloc (the memory the plugin draws into)");
NVACCEL_CNT(nvaccel_vm_parked,        gCntVmParked,       "VRAM kept when the family freed a surface's VRAM");
NVACCEL_CNT(nvaccel_vm_unparked,      gCntVmUnparked,     "allocations answered with the surface's kept VRAM");
NVACCEL_CNT(nvaccel_vm_park_released, gCntVmParkReleased, "kept VRAM released (resource freed or grown)");
NVACCEL_CNT(nvaccel_res_new,          gCntResNew,         "NVResource objects created");
NVACCEL_CNT(nvaccel_res_free,         gCntResFree,        "NVResource objects freed");
NVACCEL_CNT(nvaccel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
NVACCEL_CNT(nvaccel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
NVACCEL_CNT(nvaccel_map_free,         gCntMapFree,        "GPU VA ranges freed (nvAccelGpuVaFree)");
NVACCEL_CNT(nvaccel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
NVACCEL_CNT(nvaccel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
NVACCEL_CNT(nvaccel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
NVACCEL_CNT(nvaccel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
NVACCEL_CNT(nvaccel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
NVACCEL_CNT(nvaccel_iop_ok_h4,        gCntIopOkHead[4],   "frames copied to head 4 (an Intel built-in panel, NMIntelFB)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kNvMaxFB == 5, "kNvMaxFB changed: add debug.xegfxaccel_iop_ok_h<i> for every new head");
NVACCEL_CNT(nvaccel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nvaccel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
NVACCEL_CNT(nvaccel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
NVACCEL_CNT(nvaccel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
NVACCEL_CNT(nvaccel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
static SInt64 gCntIopCopySkip;
NVACCEL_CNT(nvaccel_iop_copy_skip,    gCntIopCopySkip,    "frames kept on the panel because the surface's newest pixels were not yet in system memory");
NVACCEL_CNT(nvaccel_iop_surf_repeat,  gCntIopSurfRepeat,  "K7: frames whose surface id repeats the previous frame (stale frame candidate)");
NVACCEL_CNT(nvaccel_iop_same_refresh, gCntIopSameRefresh, "K7: frames whose copy started in the same display refresh as the previous frame");
NVACCEL_CNT(nvaccel_iop_copy_overrun, gCntIopCopyOverrun, "K7: frame copies that took longer than 12 ms (tearing risk)");
NVACCEL_CNT(nvaccel_iop_copy_cross,   gCntIopCopyCross,   "K7: copies that crossed a display refresh (tearing candidate)");
NVACCEL_CNT(nvaccel_iop_src_changed,  gCntIopSrcChanged,  "K7: copies whose source pixels changed during the copy (concurrent render)");
NVACCEL_CNT(nvaccel_iop_map_last_us,  gIopMapLastUs,      "K7: last source surface map time (us)");
NVACCEL_CNT(nvaccel_iop_map_max_us,   gIopMapMaxUs,       "K7: worst source surface map time (us)");
NVACCEL_CNT(nvaccel_iop_xe_flips,     gCntXeFlip,         "K8: frames flipped through reserved GGTT slots (zero copy, Intel panel)");
NVACCEL_CNT(nvaccel_iop_xe_flip_miss, gCntXeFlipMiss,     "K8: flips skipped because the only free slot was the live one");
NVACCEL_CNT(nvaccel_iop_xe_flip_fail, gCntXeFlipFail,     "K8: zero-copy flips that fell back to the CPU copy");
static int    gIopFlip = 0;
static SInt64 gCntIopFlip, gCntIopFlipNo[8];
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopFlip, 0, "1 = scan the composited IOSurface out of its own VRAM (no copy)");
NVACCEL_CNT(nvaccel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
NVACCEL_CNT(nvaccel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
NVACCEL_CNT(nvaccel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
NVACCEL_CNT(nvaccel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
NVACCEL_CNT(nvaccel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
NVACCEL_CNT(nvaccel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopUnwritten, gCntIopCopyUnwritten;
NVACCEL_CNT(nvaccel_iop_flip_unwritten, gCntIopUnwritten,     "copied because the family marked VRAM newest on a copy the plugin never wrote");
NVACCEL_CNT(nvaccel_iop_copy_unwritten, gCntIopCopyUnwritten, "copied frames that skipped the page-off because the plugin never wrote the VRAM copy");
// The two decisions the display pipe makes from the family's validity bits, kept as pure functions so they can be
// exercised outside the kernel (tools/test_iop_flip_gate.py). vramNewest is the family's bit (valid[6] & plane);
// sysmemCurrent is valid[0] & plane; pluginWrote is nvAccelPluginWrote(): the resource's recorded write cookie equals the
// allocation cookie of the VRAM it holds now.
// A zero-copy flip scans VRAM out directly, so it needs a VRAM copy that holds a frame: the family's bit alone is not
// enough, the plugin must have written it (its dirty escape sets both).
static inline bool nvAccelIopVramFlippable(bool vramNewest, bool pluginWrote) { return vramNewest && pluginWrote; }
// A copied frame is read from system memory. When the family says that copy is stale, the resource is paged off VRAM
// first; a VRAM copy the plugin never wrote holds nothing worth paging, and paging it would overwrite the pixels the
// plugin copied out. Such a surface is read as it is.
static inline bool nvAccelIopCopyNeedsPageoff(bool sysmemCurrent, bool pluginWrote) { return !sysmemCurrent && pluginWrote; }
static inline bool nvAccelPluginWrote(unsigned long long wroteCookie, unsigned long long allocCookie) { return wroteCookie != 0 && wroteCookie == allocCookie; }
static bool nvAccelResPluginWrote(NVResource *res) { NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr; return vm && nvAccelPluginWrote(res->fPluginWroteCookie, vm->fAllocationCookie); }
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
NVACCEL_CNT(nvaccel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
NVACCEL_CNT(nvaccel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;

/*
 * Frame completion off WindowServer's thread. NVIDIA's display path never makes the compositor wait for the GPU: a flip
 * completes on the display engine's flip event, and a flip ordered behind a fence waits for it in a worker
 * (nvidia-drm-modeset.c:636/692). Here WindowServer waited for every composite to finish on the GPU before swapping
 * (issue #196: ~1.3 ms of each 6.9 ms frame at 144 Hz, ~95-105 presents/s). Now the plugin marks the surfaces a
 * WindowServer command buffer renders as pending when it commits and ready when the GPU has finished them, and the swap
 * waits for that here: on the async flip thread for a zero-copy flip, before reading for a copied frame. A mark answers
 * 0 while this is on; the plugin keeps WindowServer's own wait when it answers anything else.
 */
static int gSurfGpuOn = 1;
static SInt64 gCntSurfMarks, gCntSurfWaits, gCntSurfTimeouts;
static int gSurfWaitMaxUs;
static const unsigned kSurfGpuWaitMs = 100;   // a GPU that has not finished a composite in 100 ms is hung; flip anyway
struct NVSurfPending { unsigned int id; unsigned int seq; };
static NVSurfPending gSurfPend[32];
static unsigned int gSurfPendSeq;
static IOLock *gSurfPendLock;
static IOLock *nvSurfLock()
{
    if (!gSurfPendLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSurfPendLock)) IOLockFree(l); }
    return gSurfPendLock;
}
static void nvSurfMark(unsigned int id, bool pending)
{
    IOLock *l = nvSurfLock();
    if (!l || !id) return;
    IOLockLock(l);
    unsigned slot = 32, oldest = 0;
    for (unsigned i = 0; i < 32; i++) {
        if (gSurfPend[i].id == id) { slot = i; break; }
        if (slot == 32 && !gSurfPend[i].id) slot = i;
        if (gSurfPend[i].seq < gSurfPend[oldest].seq) oldest = i;
    }
    if (pending) {
        if (slot == 32) slot = oldest;   // 32 surfaces in flight at once is not a compositor; the oldest mark goes
        gSurfPend[slot].id = id; gSurfPend[slot].seq = ++gSurfPendSeq;
    } else if (slot < 32 && gSurfPend[slot].id == id) {
        gSurfPend[slot].id = 0;
        IOLockWakeup(l, gSurfPend, false);
    }
    IOLockUnlock(l);
    OSAddAtomic64(1, &gCntSurfMarks);
}
static bool nvSurfWaitReady(unsigned int id)
{
    IOLock *l = gSurfPendLock;
    if (!l || !id || !gSurfGpuOn) return true;   // switched off: marks left from before never hold a swap
    uint64_t start = mach_absolute_time(), deadline = 0;
    clock_interval_to_deadline(kSurfGpuWaitMs, kMillisecondScale, &deadline);
    bool waited = false, ready = true;
    IOLockLock(l);
    for (;;) {
        unsigned i = 0;
        while (i < 32 && gSurfPend[i].id != id) i++;
        if (i == 32) break;
        if (mach_absolute_time() >= deadline) { gSurfPend[i].id = 0; ready = false; break; }
        waited = true;
        IOLockSleepDeadline(l, gSurfPend, deadline, THREAD_UNINT);
    }
    IOLockUnlock(l);
    if (waited) {
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
        if ((int)(ns / 1000) > gSurfWaitMaxUs) gSurfWaitMaxUs = (int)(ns / 1000);
        OSAddAtomic64(1, ready ? &gCntSurfWaits : &gCntSurfTimeouts);
    }
    return ready;
}
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "1 = WindowServer swaps wait for GPU completion here (same value as nvaccel_surface_gpu_on)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_on, CTLFLAG_RW | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "0 = WindowServer waits for its own composites again (the plugin re-asks every second)");
NVACCEL_CNT(nvaccel_surface_gpu_marks,    gCntSurfMarks,    "pending/ready marks from WindowServer");
NVACCEL_CNT(nvaccel_surface_gpu_waits,    gCntSurfWaits,    "swaps that waited here for their composite to finish");
NVACCEL_CNT(nvaccel_surface_gpu_timeouts, gCntSurfTimeouts, "swaps whose composite did not finish within 100 ms");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_wait_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfWaitMaxUs, 0, "longest swap wait for a composite, us");
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
NVACCEL_CNT(nvaccel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
NVACCEL_CNT(nvaccel_iop_async_refused, gCntIopAsyncRefused, "async swaps NVKMS refused (that frame stayed off the panel)");
NVACCEL_CNT(nvaccel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight past a refresh period");
NVACCEL_CNT(nvaccel_iop_late,          gCntIopLate,         "frames kept on the panel because the previous flip had not reached it yet (framebuffer answered busy)");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.xegfxaccel_crc reads");
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvaccel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head nvaccel_crc_head is showing (0 = unavailable)");
#ifdef NMX_XEGFX
// ---- M4a: boot-armed auto-configuration (docs/19-M4A-ACCEL.md) -----------------------------------------------
// A boot that asks for it -- xegfxaccelboot=1 in the boot arguments, or "XeGfxBootArm" published on IOResources or
// on this accelerator's provider -- gets the accelerated desktop armed by this kext itself: the same actions
// userspace drives by hand (igpu=1, fb=1, fb=3, iop=1, flip=1) run in a kernel thread, before loginwindow and
// WindowServer ask for a Metal device, so no manual sysctl and no WindowServer restart is needed.  Every wait is
// bounded and a failure is logged and published, never a panic: the display then stays on whichever framebuffer
// owns the panel (IntelFB / the placeholder / the KMS adapter) and the manual path is still there.
static uint64_t nvBootMsSince(uint64_t t0)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    return ns / 1000000ULL;
}
static bool nvBootTruthy(OSObject *o)
{
    if (!o || o == kOSBooleanFalse) return false;
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) return n->unsigned32BitValue() != 0;
    return true;   // a plain published marker counts as on (publishResource("XeGfxArm") publishes "IOService")
}
bool NVAccel::bootArmRequested(const char **why, int *source)
{
    int v = 0;
    if (PE_parse_boot_argn("xegfxaccelboot", &v, sizeof v) && v) {
        *why = "boot argument xegfxaccelboot=1"; *source = 1; return true;
    }
    char flag = 0;
    if (PE_parse_boot_argn("-xegfxaccelboot", &flag, sizeof flag)) {
        *why = "boot argument -xegfxaccelboot"; *source = 1; return true;
    }
    if (IOService *res = IOService::getResourceService()) {
        if (nvBootTruthy(res->getProperty("XeGfxBootArm"))) {
            *why = "IOResources XeGfxBootArm"; *source = 2; return true;
        }
    }
    if (IOService *prov = getProvider()) {
        if (nvBootTruthy(prov->getProperty("XeGfxBootArm"))) {
            *why = "provider XeGfxBootArm"; *source = 3; return true;
        }
    }
    return false;
}
bool NVAccel::bootHeadReady(const char **cls, const char **gate)
{
    IOService *cand = nullptr;
    if (fFBs[4] && xgXeGfxFBGate(fFBs[4])) cand = fFBs[4];        // the placeholder and the adapter both want head 4
    else if (fKmsFB && xgXeGfxFBGate(fKmsFB)) cand = fKmsFB;       // the adapter parked by fbPublished
    else if (fFB && xgXeGfxFBGate(fFB)) cand = fFB;                // whichever became primary
    if (!cand) return false;
    if (cls) *cls = xgXeGfxFBClass(cand);
    if (gate) *gate = xgXeGfxFBGate(cand);
    return true;
}
void NVAccel::bootArmStart(const char *why, int source)
{
    if (fBootArmed == 1) { ALOG("boot-armed: already running -- ignoring %s", why); return; }
    if (gBootArmState == 2) { ALOG("boot-armed: already READY -- ignoring %s", why); return; }
    fBootArmed = 1; gBootArmState = 1; gBootArmReason = source; gBootArmSteps = 0;
    setProperty("XeGfxAccelBootArm", why);
    setProperty("XeGfxAccelBootReady", kOSBooleanFalse);
    setProperty("XeGfxAccelBootAgdc", kOSBooleanFalse);
    if (fPCI) fPCI->setProperty("XeGfxAccelBootArm", kOSBooleanTrue);
    retain();   // the thread outlives this call and releases this reference at its end
    thread_t th = nullptr;
    if (kernel_thread_start(&NVAccel::bootArmThread, this, &th) != KERN_SUCCESS || !th) {
        fBootArmed = 2; gBootArmState = 3; release();
        setProperty("XeGfxAccelBootFailure", "kernel_thread_start failed");
        if (fPCI) fPCI->setProperty("XeGfxAccelBootReady", kOSBooleanFalse);
        ALOG("boot-armed: kernel_thread_start FAILED -- this boot stays on the manual path "
             "(debug.xegfxaccelfb=1, then debug.xegfxaccelfb=3, then debug.xegfxaccel_iop=1)");
        return;
    }
    thread_deallocate(th);
    ALOG("boot-armed: ARMED by %s (source %d) -- kernel thread %p; sequence in %d ms, head wait %d ms, budget %d s",
         why, source, th, gBootArmDelayMs, gBootArmHeadWaitMs, gBootArmBudgetMs / 1000);
}
void NVAccel::bootArmThread(void *p0, int resume)
{
    (void)resume;
    NVAccel *me = (NVAccel *)p0;
    if (me && me == gAccel)
        me->bootArmRun();
    else if (me) {
        gBootArmState = 3;
        me->setProperty("XeGfxAccelBootReady", kOSBooleanFalse);
        me->setProperty("XeGfxAccelBootFailure", "the accelerator stopped before the boot sequence ran");
        ALOG("boot-armed: this accelerator is no longer the live one -- sequence skipped");
    }
    if (me) me->release();
}
void NVAccel::bootArmFinish(int ok, const char *msg)
{
    fBootArmed = 2;
    gBootArmState = ok ? 2 : 3;
    setProperty("XeGfxAccelBootSteps", gBootArmSteps, 32);
    setProperty("XeGfxAccelBootReady", ok ? kOSBooleanTrue : kOSBooleanFalse);
    if (fPCI) fPCI->setProperty("XeGfxAccelBootReady", ok ? kOSBooleanTrue : kOSBooleanFalse);
    if (ok) return;
    setProperty("XeGfxAccelBootFailure", msg ? msg : "unspecified");
    ALOG("boot-armed: FAILED -- %s (steps 0x%x, published 0x%x, registered 0x%x, piped 0x%x, igpu %d, fb %d, iop %d, "
         "flip %d/defer %d). Nothing was torn down and the accelerator stays inert, so WindowServer keeps whichever "
         "framebuffer owns the panel: IntelFB, the placeholder or XeGfxFramebuffer is still the safe display. Retry by "
         "hand: debug.xegfxaccelfb=1, debug.xegfxaccelfb=3, debug.xegfxaccel_iop=1",
         msg ? msg : "unspecified", (unsigned)gBootArmSteps, gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped,
         gNvIgpu, gNvAccelArmed, gNvIop, gIopXeFlip, gIopXeDefer);
}
void NVAccel::bootArmRun(void)
{
    char msg[224];
    const uint64_t t0 = mach_absolute_time();
    const int budget = gBootArmBudgetMs;
    ALOG("boot-armed: sequence starts (delay %d ms, budget %d s); the manual sysctls keep working", gBootArmDelayMs,
         budget / 1000);
    IOSleep(gBootArmDelayMs);   // AGDC and the display machine finish their own early MMIO work first
    if (!fRegLock || !fDM) { bootArmFinish(0, "no registration lock or no display machine"); return; }

    // 1/6: a head must be published AND through its start gate.  Registering one earlier is what crashed
    // IOFramebuffer::getAttributeExt, so this wait is the price of the whole path.
    const char *cls = nullptr, *gate = nullptr;
    unsigned waited = 0;
    while (!bootHeadReady(&cls, &gate)) {
        if (waited >= (unsigned)gBootArmHeadWaitMs || nvBootMsSince(t0) >= (uint64_t)budget) {
            snprintf(msg, sizeof msg, "no XeGfx framebuffer with an open start gate after %u ms (published 0x%x): the "
                     "display stack never came up this boot", waited, gNvHeadsPublished);
            bootArmFinish(0, msg); return;
        }
        IOSleep(250); waited += 250;
    }
    ALOG("boot-armed: 1/6 head gate open after %u ms (%s, %s); published 0x%x registered 0x%x piped 0x%x", waited, cls,
         gate, gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);

    // 2/6: igpu=1 -- head 4 is the Intel panel (already the XeGfx default; kept explicit so the log says so)
    gNvIgpu = 1; gBootArmSteps |= 0x01;
    ALOG("boot-armed: 2/6 debug.xegfxaccel_igpu=1 (head 4 = the Intel panel)");

    // 3/6: fb=1 -- register the published head that has no pipe yet
    if (!gAccel) { bootArmFinish(0, "the accelerator went away before registration"); return; }
    gAccel->triggerFromSysctl(1);
    waited = 0;
    while (!(gNvHeadsRegistered & 0x10u) && waited < (unsigned)gBootArmHeadWaitMs && nvBootMsSince(t0) < (uint64_t)budget) {
        IOSleep(200); waited += 200;
    }
    if (!(gNvHeadsRegistered & 0x10u)) {
        snprintf(msg, sizeof msg, "fb=1 did not register head 4 within %u ms (published 0x%x registered 0x%x)",
                 gBootArmHeadWaitMs, gNvHeadsPublished, gNvHeadsRegistered);
        bootArmFinish(0, msg); return;
    }
    gBootArmSteps |= 0x02;
    ALOG("boot-armed: 3/6 debug.xegfxaccelfb=1 -> head 4 registered (0x%x) after %u ms", gNvHeadsRegistered, waited);

    // 4/6: fb=3 -- scan-out backed pipe, plus the family's mode-change delivery
    gAccel->triggerFromSysctl(3);
    waited = 0;
    while (!(gNvHeadsPiped & 0x10u) && waited < (unsigned)gBootArmPipeWaitMs && nvBootMsSince(t0) < (uint64_t)budget) {
        IOSleep(250); waited += 250;
    }
    if (!(gNvHeadsPiped & 0x10u)) {
        snprintf(msg, sizeof msg, "fb=3 gave head 4 no display pipe within %u ms (registered 0x%x piped 0x%x)",
                 gBootArmPipeWaitMs, gNvHeadsRegistered, gNvHeadsPiped);
        bootArmFinish(0, msg); return;
    }
    if (!gNvAccelArmed) {
        bootArmFinish(0, "head registered but mode-change delivery did not arm Metal"); return;
    }
    gBootArmSteps |= 0x08;
    ALOG("boot-armed: 4/6 debug.xegfxaccelfb=3 -> head 4 has a display pipe (piped 0x%x) after %u ms", gNvHeadsPiped, waited);

    // 5/6: iop=1 -- publish IOAccelDisplayPipeCapabilities so WindowServer may use the pipe
    nvaccelSetIop(1);
    if (!gNvIop) { bootArmFinish(0, "debug.xegfxaccel_iop=1 did not take"); return; }
    gBootArmSteps |= 0x10;
    ALOG("boot-armed: 5/6 debug.xegfxaccel_iop=1 -> display-pipe capabilities published, transaction hooks OPEN");

    // 6/6: the K9b deferred zero-copy flip (what the working manual config arms: flip + defer + idle wait)
    gIopXeFlip = 1; gIopXeDefer = 1; gIopXeWaitIdle = 1; gIopXeReportBusy = 1; gIopXeIdleMs = 40;
    gBootArmSteps |= 0x20;
    ALOG("boot-armed: 6/6 debug.xegfxaccel_iop_xe_flip=1 + debug.xegfxaccel_iop_xe_defer=1 (wait_idle 1, idle_ms 40, "
         "report_busy 1)");

    // A-53 stage 1: take a 12544-wide scanout out of the GGTT slots (allocate + map only; the plane is
    // untouched here).  If it cannot be had, the copy path stays exactly as it is.
    nvScanoutAlloc();

    // AGDC (debug.xegfx_agdc=1 + debug.xegfx_agdc_k5=1) belongs to org.watchthelight.XeGfxAGDC: a kext cannot write
    // another kext's sysctls, so the request is published on the GPU node instead and the fact is recorded as its own
    // property rather than as a failure.  M4b should make the AGDC kext read this request in its start().
    if (fPCI) fPCI->setProperty("XeGfxAccelAgdcRequest", kOSBooleanTrue);
    setProperty("XeGfxAccelBootNote", "AGDC arming (debug.xegfx_agdc=1, debug.xegfx_agdc_k5=1) belongs to "
                "org.watchthelight.XeGfxAGDC and cannot be written from this kext; XeGfxAccelAgdcRequest marks it");
    gBootArmSteps |= 0x40;
    ALOG("boot-armed: AGDC request left on the GPU node (XeGfxAccelAgdcRequest=1); debug.xegfx_agdc/_k5 still need "
         "org.watchthelight.XeGfxAGDC or a boot helper");

    setProperty("XeGfxAccelBootTookMs", (int)nvBootMsSince(t0), 32);
    bootArmFinish(1, nullptr);
    ALOG("boot-armed: READY in %llu ms -- head 4 registered and piped, scanout %d aperture %d, iop/flip/defer armed; "
         "awaiting WindowServer desktop validation", (unsigned long long)nvBootMsSince(t0), gNvScanout, gNvAperture);
}
static int nvaccelboot_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gBootArmState;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    if (v != 1) return EINVAL;
    if (!NVAccel::gAccel) return ENXIO;
    NVAccel::gAccel->bootArmStart("debug.xegfxaccel_boot=1", 4);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_boot, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelboot_sysctl, "I",
            "1 = run the boot-armed sequence now (the actions the boot helper does by hand); reads 0 idle, 1 running, "
            "2 ready, 3 failed");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_boot_steps, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gBootArmSteps, 0,
           "boot-armed progress: 1 igpu, 2 fb=1, 4 head registered, 8 fb=3, 16 pipe, 32 iop, 64 flip, 128 AGDC request");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_boot_delay_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gBootArmDelayMs, 0,
           "boot-armed: how long the sequence waits before step 1 so AGDC and the display machine come up first");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_boot_head_wait_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gBootArmHeadWaitMs, 0,
           "boot-armed: how long step 1 waits for a XeGfx framebuffer with an open start gate");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_boot_pipe_wait_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gBootArmPipeWaitMs, 0,
           "boot-armed: how long step 4 waits for the display pipe after fb=3");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_boot_budget_ms, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gBootArmBudgetMs, 0,
           "boot-armed: hard budget for the whole sequence (default 90000)");
#endif
static bool gSysctlInstalled = false;
static void nvaccelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
#if defined(NMX_ACCEL) && !defined(NMX_XEGFX)
    // only this one: the NVIDIA accelerator may be loaded too, and owns the debug.xegfxaccel* names
    sysctl_register_oid(&sysctl__debug_nmxaccel_headless);
    gSysctlInstalled = true;
#else
#ifdef NMX_XEGFX
    // over XeGfx (BIOS Hybrid): the NVIDIA GPU is off the bus, so NVAccel never starts and the names are free
    sysctl_register_oid(&sysctl__debug_nmxaccel_headless);
#endif
    sysctl_register_oid(&sysctl__debug_nvaccelfb);
#ifndef NMX_ACCEL
    sysctl_register_oid(&sysctl__debug_nvaccel_headless);
#endif
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc);       sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_sticky);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_parked);      sysctl_register_oid(&sysctl__debug_nvaccel_vm_unparked);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_park_released);
    sysctl_register_oid(&sysctl__debug_nvaccel_res_new);        sysctl_register_oid(&sysctl__debug_nvaccel_res_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_commit);     sysctl_register_oid(&sysctl__debug_nvaccel_map_release);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok);         sysctl_register_oid(&sysctl__debug_nvaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_empty);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_res);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_surf);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_blank);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_skip);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_unwritten); sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_unwritten);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_crc_head);       sysctl_register_oid(&sysctl__debug_nvaccel_crc);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_flips);
    { extern struct sysctl_oid sysctl__debug_nvaccel_svdiag; sysctl_register_oid(&sysctl__debug_nvaccel_svdiag); }
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu);    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_on);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_marks); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_waits);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_timeouts); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_wait_max_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_refused); sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_late);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h3);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h4);      sysctl_register_oid(&sysctl__debug_nvaccel_igpu);
#ifdef NMX_XEGFX
    { extern struct sysctl_oid sysctl__debug_nvaccel_scanout_copies; sysctl_register_oid(&sysctl__debug_nvaccel_scanout_copies); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_scanout_last_us, sysctl__debug_nvaccel_scanout_max_us,
          sysctl__debug_nvaccel_scanout_sum_us, sysctl__debug_nvaccel_scanout_self;
      sysctl_register_oid(&sysctl__debug_nvaccel_scanout_last_us); sysctl_register_oid(&sysctl__debug_nvaccel_scanout_max_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_scanout_sum_us);  sysctl_register_oid(&sysctl__debug_nvaccel_scanout_self); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_pipe_init, sysctl__debug_nvaccel_pipe_validate,
          sysctl__debug_nvaccel_pipe_complete, sysctl__debug_nvaccel_pipe_perform;
      sysctl_register_oid(&sysctl__debug_nvaccel_pipe_init);     sysctl_register_oid(&sysctl__debug_nvaccel_pipe_validate);
      sysctl_register_oid(&sysctl__debug_nvaccel_pipe_complete); sysctl_register_oid(&sysctl__debug_nvaccel_pipe_perform); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_copy_last_us, sysctl__debug_nvaccel_iop_copy_max_us,
          sysctl__debug_nvaccel_iop_copy_sum_us, sysctl__debug_nvaccel_iop_txn_last_us, sysctl__debug_nvaccel_iop_txn_max_us,
          sysctl__debug_nvaccel_iop_txn_sum_us, sysctl__debug_nvaccel_iop_gap_last_us, sysctl__debug_nvaccel_iop_gap_max_us,
          sysctl__debug_nvaccel_iop_gap_sum_us, sysctl__debug_nvaccel_iop_memcpy;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_last_us);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_max_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_sum_us);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_txn_last_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_txn_max_us);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_txn_sum_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_gap_last_us);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_gap_max_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_gap_sum_us);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_memcpy); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_vblank, sysctl__debug_nvaccel_iop_vblank_waits,
          sysctl__debug_nvaccel_iop_vblank_skips, sysctl__debug_nvaccel_iop_vblank_last_us,
          sysctl__debug_nvaccel_iop_vblank_max_us;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_vblank);          sysctl_register_oid(&sysctl__debug_nvaccel_iop_vblank_waits);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_vblank_skips);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_vblank_last_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_vblank_max_us); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_xe_flip, sysctl__debug_nvaccel_iop_xe_flips, sysctl__debug_nvaccel_iop_xe_flip_miss,
          sysctl__debug_nvaccel_iop_xe_flip_fail;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_flips);   // Intel successes; generic iop_flips counts NVIDIA flips
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_flip);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_flip_miss);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_flip_fail); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_xe_defer, sysctl__debug_nvaccel_iop_xe_wait_idle,
          sysctl__debug_nvaccel_iop_xe_idle_ms, sysctl__debug_nvaccel_iop_xe_report_busy;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_wait_idle);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_idle_ms);     sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_report_busy); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_xe_defer_queued, sysctl__debug_nvaccel_iop_xe_defer_armed,
          sysctl__debug_nvaccel_iop_xe_defer_coalesced, sysctl__debug_nvaccel_iop_xe_defer_dropped,
          sysctl__debug_nvaccel_iop_xe_defer_signals, sysctl__debug_nvaccel_iop_xe_defer_forced,
          sysctl__debug_nvaccel_iop_xe_defer_max_depth,
          sysctl__debug_nvaccel_iop_xe_idle_waits, sysctl__debug_nvaccel_iop_xe_idle_forced,
          sysctl__debug_nvaccel_iop_xe_idle_last_us, sysctl__debug_nvaccel_iop_xe_idle_max_us,
          sysctl__debug_nvaccel_iop_xe_arm_last_us, sysctl__debug_nvaccel_iop_xe_arm_max_us;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_queued);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_armed);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_coalesced); sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_dropped);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_signals);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_forced);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_defer_max_depth);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_idle_waits);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_idle_forced);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_idle_last_us);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_idle_max_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_arm_last_us);     sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_arm_max_us); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_surf_repeat, sysctl__debug_nvaccel_iop_same_refresh,
          sysctl__debug_nvaccel_iop_copy_overrun, sysctl__debug_nvaccel_iop_copy_cross,
          sysctl__debug_nvaccel_iop_src_changed, sysctl__debug_nvaccel_iop_map_last_us,
          sysctl__debug_nvaccel_iop_map_max_us;
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_surf_repeat);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_same_refresh);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_overrun); sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_cross);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_changed);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_map_last_us);
      sysctl_register_oid(&sysctl__debug_nvaccel_iop_map_max_us); }
    nvXeTraceStart();
#endif
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_published); sysctl_register_oid(&sysctl__debug_nvaccel_heads_registered);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_piped);
#ifdef NMX_XEGFX
    // M4a: the boot-armed sequence (docs/19-M4A-ACCEL.md)
    sysctl_register_oid(&sysctl__debug_nvaccel_boot);
    sysctl_register_oid(&sysctl__debug_nvaccel_boot_steps);
    sysctl_register_oid(&sysctl__debug_nvaccel_boot_delay_ms);    sysctl_register_oid(&sysctl__debug_nvaccel_boot_head_wait_ms);
    sysctl_register_oid(&sysctl__debug_nvaccel_boot_pipe_wait_ms); sysctl_register_oid(&sysctl__debug_nvaccel_boot_budget_ms);
    // M3d: the adapter present path (docs/20-M3D-ACCEL.md)
    { extern struct sysctl_oid sysctl__debug_nvaccel_present;
      sysctl_register_oid(&sysctl__debug_nvaccel_present); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_present_ok, sysctl__debug_nvaccel_present_fail,
          sysctl__debug_nvaccel_present_busy, sysctl__debug_nvaccel_present_not_ready,
          sysctl__debug_nvaccel_present_badarg, sysctl__debug_nvaccel_present_latch_timeouts,
          sysctl__debug_nvaccel_present_ggtt_writes, sysctl__debug_nvaccel_present_phys_writes,
          sysctl__debug_nvaccel_present_last_us, sysctl__debug_nvaccel_present_max_us;
      sysctl_register_oid(&sysctl__debug_nvaccel_present_ok);           sysctl_register_oid(&sysctl__debug_nvaccel_present_fail);
      sysctl_register_oid(&sysctl__debug_nvaccel_present_busy);         sysctl_register_oid(&sysctl__debug_nvaccel_present_not_ready);
      sysctl_register_oid(&sysctl__debug_nvaccel_present_badarg);       sysctl_register_oid(&sysctl__debug_nvaccel_present_latch_timeouts);
      sysctl_register_oid(&sysctl__debug_nvaccel_present_ggtt_writes);  sysctl_register_oid(&sysctl__debug_nvaccel_present_phys_writes);
      sysctl_register_oid(&sysctl__debug_nvaccel_present_last_us);      sysctl_register_oid(&sysctl__debug_nvaccel_present_max_us); }
    { extern struct sysctl_oid sysctl__debug_nvaccel_vblank_reads, sysctl__debug_nvaccel_vblank_wraps,
          sysctl__debug_nvaccel_scanout_kms_ok, sysctl__debug_nvaccel_scanout_kms_not_ready,
          sysctl__debug_nvaccel_cursor_requests;
      sysctl_register_oid(&sysctl__debug_nvaccel_vblank_reads);         sysctl_register_oid(&sysctl__debug_nvaccel_vblank_wraps);
      sysctl_register_oid(&sysctl__debug_nvaccel_scanout_kms_ok);       sysctl_register_oid(&sysctl__debug_nvaccel_scanout_kms_not_ready);
      sysctl_register_oid(&sysctl__debug_nvaccel_cursor_requests); }
#endif
    gSysctlInstalled = true;
    ALOG("sysctl debug.xegfxaccelfb installed -- `sudo sysctl -w debug.xegfxaccelfb=1` registers the framebuffer");
#endif
}

static inline void nvAccelZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static struct NVRMRmHandles gRmHandles;
static bool                 gRmHandlesValid;

static unsigned nvRmHandlesLogged;

static bool nvAccelRmHandles(struct NVRMRmHandles *out)
{
    if (gRmHandlesValid) { *out = gRmHandles; return true; }
    NVAccel *a = NVAccel::gAccel;
    if (!a) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no NVAccel instance yet -- too early"); }
        return false;
    }
    if (!a->fFB) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no framebuffer has published yet -- will retry"); }
        return false;
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
    if (!sym) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: OSSymbol allocation failed"); }
        return false;
    }
    struct NVRMRmHandles h;
    nvAccelZero(&h, sizeof h);
    h.version = NVRM_GPUVA_ABI_VERSION;
    IOReturn r = a->fFB->callPlatformFunction(sym, false, &h, nullptr, nullptr, nullptr);
    sym->release();
    if (r != kIOReturnSuccess || !h.kapiDevice) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: framebuffer gave no kapi device (ret 0x%x, kapiDevice %p)",
                 (unsigned)r, h.kapiDevice); }
        return false;
    }
    gRmHandles = h; gRmHandlesValid = true;
    ALOG("RM handles: kapiDevice %p (hClient/hDevice resolved inside NVRM by name) "
         "*** the accelerator can now build GPU page tables ***", h.kapiDevice);
    *out = h;
    return true;
}

static IOService *nvAccelNVRM()
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nullptr;
    if (!a->fNVRM) {
        OSDictionary *m = IOService::serviceMatching("NVRM");
        if (m) a->fNVRM = IOService::waitForMatchingService(m, 100ULL * 1000 * 1000);
    }
    return a->fNVRM;
}

static IOReturn nvAccelGpuVaCall(const char *fn, struct NVRMGpuVaRequest *g)
{
    IOService *nvrm = nvAccelNVRM();
    if (!nvrm) return kIOReturnNotAttached;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = nvrm->callPlatformFunction(sym, false, g, nullptr, nullptr, nullptr);
    sym->release();
    return r;
}

#define NVRM_SS_KERNEL_ABI 1
#include "../nvrm_surfshare_abi.h"
extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool nvFamLock(NVAccel *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void nvFamUnlock(NVAccel *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
static unsigned gNVSVLockMiss;
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static NVResource *gNVSVPendResource[kNVSVPend];
static IOSimpleLock *gNVSVPendLock;
static bool nvSVPend(unsigned id, int op, NVResource *resource = nullptr)
{
    if (!id) return op == 1 ? false : false;
    if (!gNVSVPendLock) return op == 1 ? false : false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) {
            r = true;
            if (op == 0) { gNVSVPendId[i] = 0; gNVSVPendResource[i] = nullptr; }
            else if (op == 1) gNVSVPendResource[i] = resource;
            break;
        }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r) { if (freeSlot >= 0) { gNVSVPendId[freeSlot] = id; gNVSVPendResource[freeSlot] = resource; r = true; } }
    else if (op == 1) r = true;
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
// A pending mark belongs to the resource that last dirtied the surface. Never
// dereference a device cache during destruction, or clear a newer owner's mark.
static void nvSVForgetResource(NVResource *resource)
{
    if (!resource || !gNVSVPendLock) return;
    IOSimpleLockLock(gNVSVPendLock);
    for (unsigned i = 0; i < kNVSVPend; i++) if (gNVSVPendResource[i] == resource) {
        gNVSVPendId[i] = 0;
        gNVSVPendResource[i] = nullptr;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void nvSVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
// sysctl debug.xegfxaccel_svdiag=N logs the next N surface dirty marks and page-offs (surface, result, path); 0 = off
static int gSVDiag;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_svdiag, CTLFLAG_RW | CTLFLAG_LOCKED, &gSVDiag, 0, "log the next N surface dirty marks and page-offs");
static bool nvSVDiag() { return gSVDiag > 0 && OSDecrementAtomic(&gSVDiag) > 0; }
static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return NVRM_SS_NO_CACHE;
    if (!gNVSVPendLock) gNVSVPendLock = IOSimpleLockAlloc();
    if (!nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return NVRM_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    { NVVidMemory *vm = nvres_vidmem(res); res->fPluginWroteCookie = vm ? vm->fAllocationCookie : 0; }
    return 0;
}
// The caller holds the accelerator family lock. The plugin can still write a
// retained allocation after the family unwires it; make that same backing active
// before marking the GPU copy current. Never allocate fresh memory for this case.
static bool nvAccelRestoreParkedForDirty(NVResource *res, NVVidMemory *vm, unsigned plane)
{
    if (!res || !vm || vm->fRmHandle || !vm->fParkHandle) return true;
    const volatile unsigned char *r = (const volatile unsigned char *)res;
    const unsigned flags = *(const volatile unsigned *)(r + 0xc);
    const volatile unsigned short *valid = *(unsigned short *volatile const *)(r + 0x30);
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)vm + 0x40);
    if (vm->fScanout || r[0x14] != 0xc0 || (flags & 0x8000) || !valid || plane > 15 ||
        !want || vm->fParkBytes < want || !vm->fParkPhys || !vm->fParkKva)
        return true;  // Preserve the original refusal and CPU-copy fallback.
    void *const handle = vm->fParkHandle;
    void *const mapping = vm->fParkKva;
    const unsigned long long bytes = vm->fParkBytes, phys = vm->fParkPhys, cookie = vm->fParkCookie;
    // IOAccelVidMemory::wire calls allocPhysical and updates family accounting;
    // it does not page CPU pixels in. The size check above selects allocPhysical's
    // retained-allocation branch, which moves the existing backing without copying.
    _ZN16IOAccelVidMemory4wireEv(vm);
    return vm->fRmHandle == handle && vm->fKva == mapping && vm->fPhys == phys &&
        vm->fBytes == bytes && vm->fAllocationCookie == cookie && !vm->fParkHandle;
}
// The marks arrive through NVK's surface escape (NVRM_ESC_SURF_DIRTY with plane NVRM_SS_PLANE_GPU_MARK | pending): the
// WindowServer sandbox denies sysctl writes. Only WindowServer's own marks are taken, so no other process can hold a swap.
static unsigned nvSurfGpuEscape(const struct NVRMSurfShareEsc *p)
{
    if (!gSurfGpuOn) return NVRM_SS_OFF;
    char nm[32] = { 0 };
    proc_selfname(nm, sizeof nm);
    if (strcmp(nm, "WindowServer")) return NVRM_SS_OFF;
    if (p->surfaceID) nvSurfMark(p->surfaceID, (p->plane & 1u) != 0);
    return 0;
}
static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)
{
    if (p->version == NVRM_SURFSHARE_VERSION && cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_GPU_MARK)) {
        p->hSrcClient = p->hSrcMemory = 0;
        p->status = nvSurfGpuEscape(p);
        return kIOReturnSuccess;
    }
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != NVRM_SURFSHARE_VERSION) { p->status = NVRM_SS_BAD_VERSION; return kIOReturnSuccess; }
    // The plugin re-marks a surface after its last GPU write on the thread that releases the pending marks. A page-off
    // waiting for one of those marks holds the accelerator lock, so that re-mark must not wait for the lock: it is
    // dropped instead, and the mark taken at commit stands.
    const bool tryOnly = cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_TRYLOCK);
    p->plane &= ~(NVRM_SS_PLANE_TRYLOCK | NVRM_SS_PLANE_GPU_MARK);
    // plane is a full uint32 from the caller and reaches IOGraphicsAccelerator2::deviceCacheForIOSurface
    // below. The plane > 15 refusals further down run after that call, and only on the dirty path.
    if (p->plane > 15) { p->status = NVRM_SS_NO_CACHE; return kIOReturnSuccess; }
    NVAccel *a = NVAccel::gAccel;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (!task || !a || !nvAccelRmHandles(&h)) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = NVRM_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    if (!nvFamLock(a, tryOnly)) { ((OSObject *)surf)->release(); p->status = tryOnly ? NVRM_SS_BUSY : NVRM_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (cmd == NVRM_ESC_SURF_DIRTY) {
        const bool sameBacking = nvAccelRestoreParkedForDirty(res, vm, p->plane);
        p->status = (sameBacking && cache && res && vm && vm->fRmHandle)
            ? nvAccelSurfDirty(cache, res, p->plane) : NVRM_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (nvSVDiag()) ALOG("SVDIAG dirty surface %u plane %u -> %#x (cache %d res %d vm %d)", p->surfaceID, p->plane, p->status,
                             cache ? 1 : 0, res ? 1 : 0, (vm && vm->fRmHandle) ? 1 : 0);
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[0x14] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = NVRM_SS_NO_CACHE;
    else if (!vm || !vm->fRmHandle || !vm->fBytes) p->status = NVRM_SS_NO_VRAM;
    else if (p->size > vm->fBytes) p->status = NVRM_SS_TOO_SMALL;
    else {
        struct NVRMSurfShareRm g; nvSVZero(&g, sizeof g);
        g.version = NVRM_SS_RM_VERSION; g.op = NVRM_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->fRmHandle; g.target = p->hClient; g.status = NVRM_SS_OFF;
        IOService *nvrm = nvAccelNVRM();
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_RM);
        if (nvrm && sym && nvrm->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->fBytes;
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *nvSVStageLock(void)
{
    if (!gNVSVStageLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVStageLock)) IOLockFree(l);
    }
    return gNVSVStageLock;
}
static IOBufferMemoryDescriptor *nvSVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

void NVResource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + 0xe0);
    NVVidMemory *vm = nvres_vidmem(this);
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (nvSVDiag()) {
        volatile unsigned short *vd = *(unsigned short *volatile *)(r + 0x30);
        ALOG("SVDIAG pageoff surface %u type 0x%x vm %d rm %d scanout %d valid0 %#x valid6 %#x", surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u,
             (unsigned)r[0x14], vm ? 1 : 0, (vm && vm->fRmHandle) ? 1 : 0, vm ? (int)vm->fScanout : -1, vd ? vd[0] : 0xffff, vd ? vd[6] : 0xffff);
    }
    if (!surf || !vm || !vm->fRmHandle || vm->fScanout || !vm->fBytes || !nvAccelRmHandles(&h)) {
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    // a CPU lock that arrives while WindowServer's GPU is still writing this surface waits for that write first, as a
    // normal IOSurfaceLock does on Apple's drivers (the commit marked it pending; see nvmtl_ws_mark_pending)
    nvSurfWaitReady(_ZNK9IOSurface12getSurfaceIDEv(surf));
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (len > vm->fBytes) len = vm->fBytes;
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = NVRM_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = nvSVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = nvSVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = spa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec;
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = NVRM_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = pa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec; ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kNvMaxFB], *gIopFlipPrev[kNvMaxFB];
static bool gIopFlipped[kNvMaxFB];
static unsigned gIopFlipLogs;
static bool nvAccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct NVIopFlipCache { IOSurface *s; NVResource *res; NVVidMemory *vm; void *kmem; unsigned long long bytes; unsigned long long use; };
static NVIopFlipCache gIopFlipCache[8];
static IOSimpleLock *gIopFlipCacheLock;
static unsigned long long gIopFlipCacheTick;
static bool nvAccelIopCacheGet(IOSurface *s, NVResource **res, NVVidMemory **vm, void **kmem, unsigned long long *bytes)
{
    if (!gIopFlipCacheLock) return false;
    bool hit = false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < 8; i++) if (gIopFlipCache[i].s == s) {
        NVVidMemory *v = gIopFlipCache[i].vm;
        if (v && v->fRmHandle == gIopFlipCache[i].kmem && v->fBytes == gIopFlipCache[i].bytes) {
            *res = gIopFlipCache[i].res; *vm = v; *kmem = gIopFlipCache[i].kmem; *bytes = gIopFlipCache[i].bytes;
            gIopFlipCache[i].use = ++gIopFlipCacheTick; hit = true;
        }
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return hit;
}
static void nvAccelIopCachePut(IOSurface *s, NVResource *res, NVVidMemory *vm, void *kmem, unsigned long long bytes)
{
    if (!gIopFlipCacheLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopFlipCacheLock)) IOSimpleLockFree(l); }
    if (!gIopFlipCacheLock) return;
    IOSurface *drop = nullptr;
    ((OSObject *)s)->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (gIopFlipCache[i].s == s) { k = i; break; }
        if (!gIopFlipCache[i].s) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    drop = gIopFlipCache[k].s;
    gIopFlipCache[k].s = s; gIopFlipCache[k].res = res; gIopFlipCache[k].vm = vm; gIopFlipCache[k].kmem = kmem;
    gIopFlipCache[k].bytes = bytes; gIopFlipCache[k].use = ++gIopFlipCacheTick;
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (drop) ((OSObject *)drop)->release();
}
extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
// next/nextF: one frame WindowServer presented while the previous flip was still in flight past the bound, taken by
// the worker as soon as that flip lands (guarded by gIopAsLock, like busy's clearing).
struct NVIopAsync { void *pipe; IOService *fb; IOSurface *s; struct NVRMFlipRequest f; volatile SInt32 busy; struct thread_call *call;
                    IOSurface *next; struct NVRMFlipRequest nextF; };
static NVIopAsync gIopAs[kNvMaxFB];
static SInt32 gIopAsyncRefuseRun;
// WindowServer's next present waits here while the previous asynchronous flip is still on its way to the panel. It
// waited in IOSleep(1) steps, each overshooting by a millisecond or more - a large slice of a 6.9 ms frame at 144 Hz,
// where the desktop then ran near half the panel rate. The flip's worker wakes the waiter when it finishes; the busy
// flag is still re-read at least every 250 us. The bound is one refresh period and a quarter, published by the
// framebuffer from its mode (NVRMFlipWaitUs): a flip in flight lands at the next vblank, so a wait past that only
// holds WindowServer's thread; the frame is dropped instead and the next one follows the flip.
static IOLock *gIopAsLock;
static uint64_t nvAccelFlipWaitUs(IOService *fb)
{
    // A retained copy: a mode switch republishes the property, which releases the number getProperty would return.
    OSObject *o = fb ? fb->copyProperty("NVRMFlipWaitUs") : nullptr;
    OSNumber *n = OSDynamicCast(OSNumber, o);
    const uint64_t us = n ? n->unsigned64BitValue() : 0;
    if (o) o->release();
    return us ? us : 50000;   // before the framebuffer has a mode, the old 50 ms
}
// What the framebuffer's answer to a flip means for the frame: on the panel, kept (busy: the flip before it is still
// landing, the next present follows it), or refused (copied, and three in a row turn the asynchronous swap off).
enum { kNvFlipShown = 0, kNvFlipKept = 1, kNvFlipRefused = 2 };
static int nvAccelFlipOutcome(IOReturn rr)
{
    return rr == kIOReturnSuccess ? kNvFlipShown : rr == kIOReturnBusy ? kNvFlipKept : kNvFlipRefused;
}
static bool nvAccelIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gIopAs[h].pipe == pipe && __atomic_load_n(&gIopAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static void nvAccelIopAsyncRun(void *p0, void *)
{
    NVIopAsync *e = (NVIopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    IOSurface *s = e->s; e->s = nullptr;
  again:
    nvSurfWaitReady(s->getSurfaceID());   // WindowServer did not wait for this composite; the flip does, off its thread
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    const int outcome = nvAccelFlipOutcome(rr);
    if (outcome == kNvFlipShown) {
        gIopAsyncRefuseRun = 0;
        if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
        gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else if (outcome == kNvFlipKept) {
        OSAddAtomic64(1, &gCntIopLate);
        ((OSObject *)s)->release();
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.xegfxaccel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        ((OSObject *)s)->release();
    }
    // A frame queued behind this flip follows it at once; otherwise the worker is free and the waiter is woken.
    IOSurface *queued = nullptr;
    if (gIopAsLock) {
        IOLockLock(gIopAsLock);
        queued = e->next; e->next = nullptr;
        if (queued) { for (unsigned i = 0; i < sizeof e->f; i++) ((volatile unsigned char *)&e->f)[i] = ((const volatile unsigned char *)&e->nextF)[i]; }
        else { __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE); IOLockWakeup(gIopAsLock, (event_t)&e->busy, false); }
        IOLockUnlock(gIopAsLock);
    } else {
        __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    }
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
    if (queued) { s = queued; goto again; }
}
static bool nvAccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct NVRMFlipRequest *f)
{
    NVIopAsync *e = &gIopAs[head];
    if (!gIopAsLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopAsLock)) IOLockFree(l); }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        uint64_t end = 0; clock_interval_to_deadline((uint32_t)nvAccelFlipWaitUs(fb), kMicrosecondScale, &end);
        while (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && mach_absolute_time() < end) {
            if (!gIopAsLock) { IOSleep(1); continue; }
            uint64_t next = 0; clock_interval_to_deadline(250, kMicrosecondScale, &next);
            if (next > end) next = end;
            IOLockLock(gIopAsLock);
            if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE))
                IOLockSleepDeadline(gIopAsLock, (event_t)&e->busy, next, THREAD_UNINT);
            IOLockUnlock(gIopAsLock);
        }
    }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && gIopAsLock) {
        // Still in flight past the bound: this frame is queued for the worker instead of holding WindowServer any
        // longer, replacing an older queued frame (that one is the dropped frame the counter names).
        IOLockLock(gIopAsLock);
        if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && e->pipe == pipe && e->fb == fb) {
            IOSurface *old = e->next;
            ((OSObject *)s)->retain();
            e->next = s;
            for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->nextF)[i] = ((const unsigned char *)f)[i];
            IOLockUnlock(gIopAsLock);
            if (old) { OSAddAtomic64(1, &gCntIopAsyncDrop); ((OSObject *)old)->release(); }
            return true;
        }
        IOLockUnlock(gIopAsLock);
    }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        return true;
    }
    if (!e->call) e->call = thread_call_allocate(nvAccelIopAsyncRun, e);
    if (!e->call) return false;
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    return true;
}
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kNvMaxFB) return false;
    // zero-copy scan-out is an NVIDIA display-engine flip; another vendor's panel (NMIntelFB) always takes the copy
    if (!fb->metaCast("NVRMNVDAFramebuffer")) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nvAccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return nvAccelIopFlipNo(2);
    NVResource *res = nullptr; NVVidMemory *vm = nullptr; void *kmem = nullptr; unsigned long long bytes = 0;
    if (nvAccelIopCacheGet(s, &res, &vm, &kmem, &bytes)) {
        OSAddAtomic64(1, &gCntIopFlipHit);
    } else {
        if (!nvFamLock(a, true)) return nvAccelIopFlipNo(3);
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
        res = OSDynamicCast(NVResource, ctx);
        vm = res ? nvres_vidmem(res) : nullptr;
        kmem = (vm && vm->fRmHandle && vm->fBytes && !vm->fScanout) ? vm->fRmHandle : nullptr;
        bytes = vm ? vm->fBytes : 0;
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        OSAddAtomic64(1, &gCntIopFlipMiss);
        if (kmem) nvAccelIopCachePut(s, res, vm, kmem, bytes);
    }
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    const bool vramNewest = valid && (valid[6] & 1u);
    if (!kmem) return nvAccelIopFlipNo(4);
    if (!vramNewest) return nvAccelIopFlipNo(5);
    // Checked against the allocation this flip submits (vm, resolved above and cached), not re-resolved from the
    // resource: this runs outside the family lock, and the record must describe the VRAM that is about to be scanned.
    if (!nvAccelIopVramFlippable(vramNewest, nvAccelPluginWrote(res->fPluginWroteCookie, vm->fAllocationCookie))) {
        const SInt64 n = OSAddAtomic64(1, &gCntIopUnwritten);
        if (n < 4 || (n & 0xfff) == 0)
            ALOG("iop: surface 0x%x: VRAM marked newest but the plugin never wrote it -- copying its system memory instead of flipping (%lld so far)",
                 s->getSurfaceID(), (long long)n + 1);
        return false;
    }
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return nvAccelIopFlipNo(6);
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    if (gIopAsync && pipe) return nvAccelIopAsyncStart(pipe, fb, head, s, &f);
    nvSurfWaitReady(s->getSurfaceID());
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (nvAccelFlipOutcome(rr) == kNvFlipKept) { OSAddAtomic64(1, &gCntIopLate); return true; }   // the frame on the panel stays
    if (rr != kIOReturnSuccess) {
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        return nvAccelIopFlipNo(7);
    }
    ((OSObject *)s)->retain();
    if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
    gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
    const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
    if (n < 4 || n == 600 || (n & 0xffff) == 0)
        ALOG("iop: zero-copy swap #%lld head %u: surface 0x%x %llux%llu pitch %llu scanned out of its own VRAM (flipResult %d)",
             (long long)n + 1, head, s->getSurfaceID(), W, H, P, f.flipResult);
    return true;
}
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    NVAccel *a = NVAccel::gAccel;
    IOService *fb = a ? a->fFB : nullptr;
    IOService *nub = fb ? fb->getProvider() : nullptr;
    OSNumber *n = nub ? OSDynamicCast(OSNumber, nub->getProperty("nvkms-kapi")) : nullptr;
    struct NvKmsKapiFunctionsTable *k = n ? (struct NvKmsKapiFunctionsTable *)(uintptr_t)n->unsigned64BitValue() : nullptr;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (k && k->getCRC32 && gCrcHead >= 0 && gCrcHead < 8 && nvAccelRmHandles(&h) && h.kapiDevice) {
        struct NvKmsKapiCrcs c; nvSVZero(&c, sizeof c);
        if (k->getCRC32((struct NvKmsKapiDevice *)h.kapiDevice, (NvU32)gCrcHead, &c))
            v = c.compositorCrc32.supported ? c.compositorCrc32.value : (c.outputCrc32.supported ? c.outputCrc32.value : 0);
    }
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static void nvAccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kNvMaxFB || !gIopFlipped[head] || !fb->metaCast("NVRMNVDAFramebuffer")) return;
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    // Busy: the flip before HOME is still landing and HOME was not submitted, so the panel is not home yet and the
    // next copied frame tries again. Clearing the flag here left every later copy offscreen behind that flip.
    if (rr == kIOReturnBusy) return;
    if (rr != kIOReturnSuccess && gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head); }
    gIopFlipped[head] = false;
}
static void nvAccelSurfScanoutSync(IOSurface *s)
{
    if (!s || !NVAccel::gAccel || !nvSVPend(s->getSurfaceID(), 2)) return;
    NVAccel *a = NVAccel::gAccel;
    if (!nvFamLock(a, true)) {
        if (++gNVSVLockMiss <= 8 || (gNVSVLockMiss & 1023) == 0)
            ALOG("surfvram scanout: accelerator lock busy, surface %u stays pending (misses %u)",
                 s->getSurfaceID(), gNVSVLockMiss);
        return;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    bool out = false;
    if (res) res->pageoff(nullptr, true, &out, 0);
    if (!out) nvSVPend(s->getSurfaceID(), 0);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
}
// True when the surface's system memory holds its newest pixels, after moving them there if the GPU drew them into VRAM.
// Unlike nvAccelSurfScanoutSync it waits a few ms for the accelerator lock and does not clear the pending mark when the
// page-off fails. At most 4 frames in a row are held back per head: a surface that never syncs is copied as before
// rather than freezing the panel.
static unsigned gIopCopyHeld[kNvMaxFB];
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head)
{
    NVAccel *a = NVAccel::gAccel;
    if (!s || !a) return true;
    const unsigned id = s->getSurfaceID();
    bool ready = !nvSVPend(id, 2);
    if (!ready) {
        bool locked = false;
        for (unsigned i = 0; i < 8 && !(locked = nvFamLock(a, true)); i++) IOSleep(1);
        if (locked) {
            void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
            OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
            NVResource *res = OSDynamicCast(NVResource, ctx);
            volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
            if (!res || !valid || (valid[0] & 1u)) ready = true;
            else if (!nvAccelIopCopyNeedsPageoff(false, nvAccelResPluginWrote(res))) { ready = true; OSAddAtomic64(1, &gCntIopCopyUnwritten); }
            else { bool out = false; res->pageoff(nullptr, true, &out, 0); ready = out || (valid[0] & 1u); }
            if (ready) nvSVPend(id, 0);
            if (cache) ((OSObject *)cache)->release();
            nvFamUnlock(a);
        }
    }
    if (head >= kNvMaxFB) return true;
    if (ready) { gIopCopyHeld[head] = 0; return true; }
    if (++gIopCopyHeld[head] > 4) {
#ifdef NMX_XEGFX
        if (head == 4 && nvXeIsKmsFB(a->fFB)) {
            gIopCopyHeld[head] = 4;
            OSAddAtomic64(1, &gCntIopCopySkip);
            return false; // Repeated failure does not make stale CPU backing ready.
        }
#endif
        gIopCopyHeld[head] = 0; return true;
    }
    OSAddAtomic64(1, &gCntIopCopySkip);
    return false;
}

static unsigned nvVaAllocLogged, nvVaMapLogged;

static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                            unsigned long long len, unsigned long long align)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !len) return 0;
    if (mm->fGpuVa) return mm->fGpuVa;
#ifdef NMX_XEGFX
    {
        if (!gXeVaLock) gXeVaLock = IOLockAlloc();
        if (!gXeVaLock) return 0;
        IOLockLock(gXeVaLock);
        if (!gXeVa) gXeVa = IORangeAllocator::withRange(0x10000000000ull, 0, 0, IORangeAllocator::kLocking);
        IORangeScalar va = 0;
        const unsigned long long a = align > 4096 ? align : 4096, l = (len + 4095) & ~4095ull;
        bool ok = gXeVa && gXeVa->allocate(l, &va, a);
        IOLockUnlock(gXeVaLock);
        if (!ok) return 0;
        mm->fHVirt = 1; mm->fGpuVa = 0x100000000ull + va; mm->fVaLen = l;
        return mm->fGpuVa;
    }
#endif

    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return 0;

    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.size = len; g.align = align;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_ALLOC, &g) != kIOReturnSuccess || !g.gpuva) {
        if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
            ALOG("gpuVaAlloc: RM refused %llu bytes (align %llu) -> status 0x%x", len, align, g.status); }
        return 0;
    }
    mm->fHVirt = g.hVirt; mm->fGpuVa = g.gpuva; mm->fVaLen = len;
    if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
        ALOG("gpuVaAlloc: %llu bytes -> gpuVA 0x%llx (hVirt 0x%x)", len, g.gpuva, g.hVirt); }
    return g.gpuva;
}

static void nvAccelGpuVaFree(IOAccelMemoryMap *map)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !mm->fHVirt) return;
#ifdef NMX_XEGFX
    if (gXeVa && mm->fGpuVa) gXeVa->deallocate(mm->fGpuVa - 0x100000000ull, mm->fVaLen);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
    return;
#endif
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = mm->fHVirt; g.hMemory = mm->fMapped ? mm->fHRmMemory : 0; g.size = mm->fVaLen;
    g.gpuva = mm->fGpuVa;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
}

bool NVMemoryMap::commitIntoGPUPageTable()
{
    if (fMapped) return true;
#ifdef NMX_XEGFX
    // no GPU page table to fill: ANV renders through its own GEM objects, these addresses are bookkeeping only
    fMapped = true; OSAddAtomic64(1, &gCntMapCommit);
    return true;
#endif
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, fMem);
    void *kapiMem = vm ? vm->fRmHandle : nullptr;
    unsigned long long len = fVaLen ? fVaLen : getLength();
    if (!fHVirt || !kapiMem || !len) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: NOT MAPPED -- hVirt 0x%x kapiMem %p len %llu (mem %p%s)",
                 fHVirt, kapiMem, len, fMem, vm ? "" : ", not an NVVidMemory"); }
        return false;
    }
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return false;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice; g.kapiMemory = kapiMem;
    g.hVirt = fHVirt; g.size = len;
    g.isVidmem = 1;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_MAP, &g) != kIOReturnSuccess) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: MAP_MEMORY_DMA refused gpuVA 0x%llx len %llu -> status 0x%x",
                 fGpuVa, len, g.status); }
        return false;
    }
    fHRmMemory = g.hMemory; fMapped = true;
    OSAddAtomic64(1, &gCntMapCommit);
    if (g.gpuva) fGpuVa = g.gpuva;
    if (fCommits < 8) { fCommits++;
        ALOG("MM commit: MAPPED gpuVA 0x%llx len %llu <- hMem 0x%x  *** the GPU can reach these bytes ***",
             fGpuVa, len, g.hMemory); }
    return true;
}

void NVMemoryMap::releaseFromGPUPageTable()
{
    if (!fMapped || !fHVirt) return;
#ifdef NMX_XEGFX
    fMapped = false; OSAddAtomic64(1, &gCntMapRelease);
    return;
#endif
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = fHVirt; g.hMemory = fHRmMemory; g.size = fVaLen;
    g.gpuva = fGpuVa;
    g.flags = NVRM_GPUVA_FLAG_UNMAP_ONLY;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    fMapped = false;
    OSAddAtomic64(1, &gCntMapRelease);
}

static void nvAccelWriteStamp(int index, unsigned int value)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned nvFrontSamples, nvFrontFlips;
static unsigned nvFrontLastSum;
static void nvAccelSampleFront(NVVidMemory *vm, unsigned w, unsigned h, unsigned pitch)
{
    if (!vm || !vm->fKva || !pitch || !w || !h) return;
#define NVFRONT_EVERY 10u
    if (++nvFrontFlips % NVFRONT_EVERY) return;
    if (nvFrontSamples >= 40) return;
    nvFrontSamples++;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    const unsigned p1 = (w > 100  && h > 100)  ? px[100  * ppr + 100]  : 0u;
    const unsigned p2 = (w > 960  && h > 540)  ? px[540  * ppr + 960]  : 0u;
    const unsigned p3 = (w > 1800 && h > 1000) ? px[1000 * ppr + 1800] : 0u;
    const bool moved = (nvFrontSamples > 1) && (sum != nvFrontLastSum);
    ALOG("FRONT SURFACE sample %u: sum 0x%08x %s nonzero %u/%u "
         "px(100,100)=0x%08x px(960,540)=0x%08x px(1800,1000)=0x%08x %ux%u pitch %u",
         nvFrontSamples, sum,
         moved ? "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
               : (nvFrontSamples > 1 ? "(UNCHANGED since last sample -- frozen, not proof of a picture)"
                                     : "(first sample -- motion is the proof, wait for the next)"),
         nonzero, n, p1, p2, p3, w, h, pitch);
    nvFrontLastSum = sum;
}

static unsigned nvSwapProbes;
static void nvAccelResStats(IOAccelResource2 *res, const char *tag)
{
    if (!res) { ALOG("  swap %s: (null)", tag); return; }
    NVVidMemory *vm = nvres_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!vm || !vm->fKva || !pitch || !w || !h) {
        ALOG("  swap %s: %ux%u pitch %u vm %p kva %p scanout %d -- NOT SAMPLEABLE",
             tag, w, h, pitch, vm, vm ? vm->fKva : nullptr, vm ? (int)vm->fScanout : -1);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    ALOG("  swap %s: %ux%u pitch %u scanout %d  sum 0x%08x  nonzero %u/%u  %s",
         tag, w, h, pitch, (int)vm->fScanout, sum, nonzero, n,
         nonzero ? "*** HAS CONTENT -- THIS IS THE SOURCE ***" : "(empty)");
}
bool nvAccelResHasContent(IOAccelResource2 *res)
{
    if (!res) return false;
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) return false;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4) return false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 4; iy++)
        for (unsigned ix = 1; ix <= 4; ix++) {
            const unsigned x = (w * ix) / 5, y = (h * iy) / 5;
            if (x < w && y < h && px[y * ppr + x]) return true;
        }
    return false;
}

void nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nvSwapProbes >= 6) return;
    nvSwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", nvSwapProbes);
    nvAccelResStats(a1, "a1");
    nvAccelResStats(a2, "a2");
}

static unsigned nvFlipWhy = 0;
#define NVFLIPWHY(...) do { if (nvFlipWhy < 20) { nvFlipWhy++; ALOG(__VA_ARGS__); } } while (0)

#define NVSCANOUT_COPY_MAX 120
static unsigned nvScanoutCopies = 0;
#ifdef NMX_XEGFX
// over XeGfx this copy is how WindowServer's frames reach the panel (N-254), not a bring-up probe: no cap, counted
static SInt64 gCntScanoutCopies;
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_copies, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntScanoutCopies,
            "composited frames copied onto the boot framebuffer by the swap path");
// the copy runs on ws_main_thread, so its cost is WindowServer's frame time: last, max, total in us, and self-copies skipped
static SInt64 gScanoutLastUs, gScanoutMaxUs, gScanoutSumUs, gCntScanoutSelf;
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutLastUs, "last scanout copy, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutMaxUs, "slowest scanout copy, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_sum_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gScanoutSumUs, "all scanout copies, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_self, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntScanoutSelf,
            "swaps whose source was the scanout itself (copy skipped)");
// the display-pipe copy path (performTransaction): copy/transaction/gap times in us, and the copy implementation switch
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_copy_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopCopyLastUs, "last display-pipe copy, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_copy_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopCopyMaxUs, "slowest display-pipe copy, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_copy_sum_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopCopySumUs, "all display-pipe copies, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_txn_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopTxnLastUs, "last display-pipe transaction, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_txn_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopTxnMaxUs, "slowest display-pipe transaction, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_txn_sum_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopTxnSumUs, "all display-pipe transactions, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_gap_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopGapLastUs, "gap since the previous display-pipe transaction, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_gap_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopGapMaxUs, "longest gap between display-pipe transactions, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_gap_sum_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopGapSumUs, "all gaps between display-pipe transactions, us");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_memcpy, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopMemcpy, 0,
           "1 = copy the frame with memcpy (default), 0 = the word-by-word loop");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_vblank, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopVblank, 0,
           "1 = copy each frame right after the panel's vblank (default), 0 = copy as soon as the transaction arrives");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_flip, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeFlip, 0,
    "1 = flip the compositor's own pages through reserved GGTT slots (zero copy), 0 = CPU copy (default)");
// K9: deferred zero-copy flip (plan 2): the display thread only queues the frame, a worker waits for GT idle
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_defer, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeDefer, 0,
    "1 = defer the Intel zero-copy plane write to a worker that waits for the GPU to be idle, 0 = synchronous flip (default)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_wait_idle, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeWaitIdle, 0,
    "1 = the deferred flip waits for the GPU to be idle before the plane write (default), 0 = arm without ordering");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_idle_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeIdleMs, 0,
    "longest wait for the GPU to be idle in the deferred flip, ms (on a timeout the frame is armed anyway)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_report_busy, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeReportBusy, 0,
    "1 = isTransactionComplete is false while a deferred flip is queued/in flight (default), 0 = always complete");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_queued, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferQueued, "frames handed to the deferred flip worker");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_armed, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferArmed, "deferred frames that reached the panel");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_coalesced, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferCoalesced, "queued deferred frames replaced by a newer one");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_dropped, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferDropped, "deferred frames that never reached the panel");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_signals, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferSignals, "display-pipe transaction interrupts signalled by the deferred flip worker");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_forced, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferForced, "deferred flips whose busy state was dropped by the stuck-worker escape hatch");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_defer_max_depth, CTLFLAG_RD | CTLFLAG_LOCKED, &gXeDeferQMaxDepth, "longest deferred flip queue, frames");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_idle_waits, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferIdleWait, "deferred flips that waited for GT idle successfully");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_idle_forced, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeDeferIdleForced, "deferred flips armed without a GT-idle confirmation (timeout)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_idle_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXeDeferIdleLastUs, "last GT-idle wait, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_idle_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXeDeferIdleMaxUs, "longest GT-idle wait, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_arm_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXeDeferArmLastUs, "last deferred arm+latch, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_arm_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXeDeferArmMaxUs, "longest deferred arm+latch, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_vblank_waits, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntIopVblankWaits, "frames copied after a vblank wait");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_vblank_skips, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntIopVblankSkips, "frames copied without a vblank wait (no MMIO, gate off, or timeout)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_vblank_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopVblankLastUs, "wait for the last vblank, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_vblank_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gIopVblankMaxUs, "longest vblank wait, us");
// M3d: the adapter present path (docs/20-M3D-ACCEL.md).  `debug.xegfxaccel_present` is the one switch to turn the
// adapter's register writes off without touching anything else; the counters say what the adapter asked for.
SYSCTL_INT(_debug, OID_AUTO, nvaccel_present, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gXePresent, 0,
           "M3d: accept adapter xgFlipPresent calls (1) or leave the plane untouched (0)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_ok, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentOk, "adapter frames programmed on head 4");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_fail, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentFail, "adapter presents that failed (previous frame kept)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_busy, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentBusy, "adapter presents refused because the K9b flip worker owns the plane");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_not_ready, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentNotReady, "adapter presents before XeGfxFbReady (no register write)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_badarg, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentBadArg, "adapter presents that did not fit the contract");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_latch_timeouts, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentLatchTimeouts, "adapter presents written but not latched inside 25 ms (previous frame put back)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_ggtt_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentGgttWrites, "adapter presents programmed by a direct GGTT offset");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_phys_writes, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPresentPhysWrites, "adapter presents programmed through a reserved GGTT slot");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_last_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXePresentLastUs, "last adapter present, us (map + plane write + latch)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_present_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gXePresentMaxUs, "slowest adapter present, us");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_vblank_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntVblankReads, "xgVblankCount/xgVblankTime reads that observed a new frame");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_vblank_wraps, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntVblankWraps, "frame-counter 32-bit wraps the accelerator extended");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_kms_ok, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntKmsScanoutOk, "xgGetScanoutInfo imports of the adapter scanout");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_scanout_kms_not_ready, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntKmsScanoutNotReady, "xgGetScanoutInfo answers that were not ready");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_cursor_requests, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntCursorRequests, "xgCursorImage requests (skeleton: validated, not programmed)");
// how far CoreDisplay gets with the display pipe (thanhnndev: it submits transactions only after AGDC 0x2001 is accepted)
SInt64 gCntPipeInit, gCntPipeValidate, gCntPipeComplete, gCntPipePerform;
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_pipe_init, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPipeInit, "NVDisplayPipe::initFramebufferResource calls");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_pipe_validate, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPipeValidate, "validateTransaction calls");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_pipe_complete, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPipeComplete, "isTransactionComplete calls");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_pipe_perform, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntPipePerform, "performTransaction calls");
#endif
#ifdef NMX_XEGFX
// Every 5 s the counters go to XeGfx's NVRAM trace (xegfx-accel) when they changed: N-258 and N-260 froze with the
// unified log lost, so the last line in NVRAM dates the freeze against the frames WindowServer had sent
static struct thread_call *gXeTraceCall;
static IOService *gXeResident;
static void nvXeTraceFire(void *, void *)
{
    static char last[96];
    char b[96];
    snprintf(b, sizeof(b), "copies %lld pipe %lld/%lld/%lld/%lld iop4 %lld vm %lld", (long long)gCntScanoutCopies,
             (long long)gCntPipeInit, (long long)gCntPipeValidate, (long long)gCntPipeComplete, (long long)gCntPipePerform,
             (long long)gCntIopOkHead[4], (long long)gCntVmAlloc);
    if (strcmp(b, last) != 0) {
        if (!gXeResident)
            if (OSDictionary *m = IOService::serviceMatching("XeGfxResident")) {
                gXeResident = IOService::copyMatchingService(m);   // kept for the boot: the accelerator never unloads
                m->release();
            }
        if (gXeResident) {
            const OSSymbol *fn = OSSymbol::withCStringNoCopy("xgTrace");
            if (fn && gXeResident->callPlatformFunction(fn, false, b, nullptr, nullptr, nullptr) == kIOReturnSuccess)
                strlcpy(last, b, sizeof(last));
            if (fn) fn->release();
        }
    }
    uint64_t dl;
    clock_interval_to_deadline(5, 1000000000u, &dl);
    thread_call_enter_delayed(gXeTraceCall, dl);
}
static void nvXeTraceStart(void)
{
    if (gXeTraceCall) return;
    gXeTraceCall = thread_call_allocate(&nvXeTraceFire, nullptr);
    if (gXeTraceCall) nvXeTraceFire(nullptr, nullptr);
}
#endif

static bool nvAccelCopyToScanout(IOAccelResource2 *res)
{
#ifndef NMX_XEGFX
    if (nvScanoutCopies >= NVSCANOUT_COPY_MAX) return false;
#endif
    if (!res) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("copyToScanout: no accelerator/framebuffer"); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("copyToScanout: refused -- heads piped 0x%x, no way to tell whose frame this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) { NVFLIPWHY("copyToScanout: source has no kva"); return false; }

    const unsigned char *rr = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    if (!w || !h || pitch < 4) { NVFLIPWHY("copyToScanout: source geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q);
    if (ret != kIOReturnSuccess || !q.kva || !q.pitch) {
        NVFLIPWHY("copyToScanout: framebuffer would not describe its scanout (0x%x kva %p pitch %u)",
                  (unsigned)ret, q.kva, q.pitch);
        return false;
    }

#ifdef NMX_XEGFX
    if (nvXeIsKmsFB(a->fFB) &&
        (w != q.width || h != q.height || (pitch & 3u) || (q.pitch & 3u) || pitch < (uint64_t)w * 4 ||
         q.pitch < (uint64_t)q.width * 4 || vm->fBytes < (uint64_t)pitch * h ||
         q.actualSize < (uint64_t)q.pitch * q.height)) {
        NVFLIPWHY("copyToScanout: KMS geometry or backing length mismatch -- frame rejected");
        return false;
    }
#endif

#ifdef NMX_XEGFX
    // a swap with an empty a2 names the scanout itself (a1): copying it onto itself reads 16 MB of write-combined
    // memory, ~0.8 s on ws_main_thread every other swap (N-255..N-280), and changes nothing on the panel
    if (vm->fScanout || (const void *)vm->fKva == (const void *)q.kva) {
        OSAddAtomic64(1, &gCntScanoutSelf);
        return true;
    }
    const uint64_t t0 = mach_absolute_time();
#endif
    const unsigned rows = (h < q.height) ? h : q.height;
    const unsigned rowb = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned words = rowb / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    const volatile unsigned int *s = (const volatile unsigned int *)vm->fKva;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = s + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
    nvScanoutCopies++;
#ifdef NMX_XEGFX
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    const SInt64 us = (SInt64)(ns / 1000);
    gScanoutLastUs = us;
    if (us > gScanoutMaxUs) gScanoutMaxUs = us;
    OSAddAtomic64(us, &gScanoutSumUs);
    OSAddAtomic64(1, &gCntScanoutCopies);
    if (nvScanoutCopies <= 3)
#else
    if (nvScanoutCopies <= 3 || nvScanoutCopies == NVSCANOUT_COPY_MAX)
#endif
        ALOG("*** COPIED THE COMPOSITED FRAME ONTO THE PANEL *** #%u: %ux%u, %u rows x %u bytes, "
             "src kva %p pitch %u -> scanout kva %p phys 0x%llx pitch %u%s",
             nvScanoutCopies, w, h, rows, rowb, vm->fKva, pitch, q.kva,
             (unsigned long long)q.phys, q.pitch,
             nvScanoutCopies == NVSCANOUT_COPY_MAX ? "  [CAP REACHED -- stopping, panel keeps this frame]" : "");
    return true;
}

#ifdef NMX_XEGFX
bool nvAccelXeSwapCopy(IOAccelResource2 *source)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !nvXeIsKmsFB(a->fFB)) return false;
    // submitSwapCopy's second resource is the completed composite. A black
    // frame is valid; the old front buffer is never a substitute for it.
    if (!nvAccelCopyToScanout(source))
        NVFLIPWHY("KMS swap: no valid completed swap source; keeping the previous frame");
    return true; // Handled even on rejection: never fall back to the old front.
}
#endif

static bool nvAccelFlipTo(IOAccelResource2 *res)
{
    if (!res) { NVFLIPWHY("nvAccelFlipTo: no resource"); return false; }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("nvAccelFlipTo: accel %p fFB %p", a, a ? a->fFB : nullptr); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("nvAccelFlipTo: refused -- heads piped 0x%x, no way to tell whose surface this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fRmHandle) {
        NVFLIPWHY("nvAccelFlipTo: vm %p fRmHandle %p -- the composited surface has no RM memory to flip to",
                  vm, vm ? vm->fRmHandle : nullptr);
        return false;
    }
    if (vm->fScanout) { NVFLIPWHY("nvAccelFlipTo: already the scanout"); return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { NVFLIPWHY("nvAccelFlipTo: geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = NVRM_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_FLIP_FN);
    if (!sym) { NVFLIPWHY("nvAccelFlipTo: OSSymbol allocation failed"); return false; }
    IOReturn rr = a->fFB->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        NVFLIPWHY("nvAccelFlipTo: framebuffer refused the flip (0x%x, flipResult %d) for %ux%u pitch %u kapiMemory %p",
                  (unsigned)rr, f.flipResult, w, h, pitch, vm->fRmHandle);
        return false;
    }
    NVFLIPWHY("nvAccelFlipTo: *** FLIPPED *** to %ux%u pitch %u kapiMemory %p (flipResult %d)",
              w, h, pitch, vm->fRmHandle, f.flipResult);
    nvAccelSampleFront(vm, w, h, pitch);
    return true;
}

IOReturn NVAccel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(NVACCEL_SS_FN))
        return nvAccelSurfVram((struct NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
#ifdef NMX_XEGFX
    // M3d: the adapter -> accelerator direction (XeGfxDisplay.kext finds this object through the XeGfxAccelPresent
    // property).  These are the only entry points that can write PLANE_SURF_1_A on the adapter's behalf; each one is
    // gated by nvXePresentReady() (XeGfxFbReady), so a call before the adapter has claimed its mode and scanout
    // touches no register.  Contract and error codes: docs/20-M3D-ACCEL.md.  xgFlipPresent may block up to ~25 ms
    // (vblank latch wait) and must be called from thread context; flags bit1 makes it return immediately.
    if (fn) {
        if (fn->isEqualTo("xgFlipPresent"))     return nvXePresent((struct XeGfxPresent *)p1);
        if (fn->isEqualTo("xgVblankCount"))     return nvXeVblankCount(p1, p2);
        if (fn->isEqualTo("xgVblankTime"))      return nvXeVblankTime(p1, p2);
        if (fn->isEqualTo("xgGetPresentInfo"))  return nvXeGetPresentInfo((struct XeGfxPresentInfo *)p1);
        if (fn->isEqualTo("xgCursorImage"))     return nvXeCursor((struct XeGfxCursorImage *)p1);
    }
#endif
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

NM_TAHOE_FWD_DEFS
