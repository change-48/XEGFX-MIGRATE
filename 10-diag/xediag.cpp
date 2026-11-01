// XeDiag v2 - read-only MMIO diagnostic for Intel display 14 (Arrow Lake 8086:7D51).
// Attaches to IOResources (always matches), finds the IGPU IOPCIDevice itself,
// maps BAR0 and publishes raw register windows. It never writes a register.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <mach/kmod.h>

extern "C" {
static kern_return_t xediag_kmod_start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
static kern_return_t xediag_kmod_stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
KMOD_EXPLICIT_DECL(org.xediag.XeDiag, "0.3", xediag_kmod_start, xediag_kmod_stop);
}

#define WIN_BYTES  0x400
#define WIN_DWORDS (WIN_BYTES / 4)

struct win { UInt32 base; const char *name; };

static const struct win wins[] = {
    { 0x40000, "w40" }, { 0x44000, "w44" }, { 0x45000, "w45" }, { 0x50000, "w50" },
    { 0x60000, "w60" }, { 0x61000, "w61" }, { 0x62000, "w62" }, { 0x63000, "w63" },
    { 0x64000, "w64" }, { 0x65000, "w65" }, { 0x66000, "w66" }, { 0x67000, "w67" },
    { 0x68000, "w68" }, { 0x69000, "w69" },
    { 0x70000, "w70" }, { 0x71000, "w71" }, { 0x72000, "w72" }, { 0x73000, "w73" },
    { 0x108000, "misc" },
};
#define NWIN (sizeof(wins) / sizeof(wins[0]))

class XeDiag : public IOService {
    OSDeclareDefaultStructors(XeDiag)
public:
    virtual bool start(IOService *provider) override;
private:
    IOMemoryMap *fMap = nullptr;
    volatile UInt32 *fReg = nullptr;
    IOByteCount fLen = 0;
};

OSDefineMetaClassAndStructors(XeDiag, IOService)

static UInt32 propU32(IORegistryEntry *e, const char *key)
{
    if (!e) return 0xffffffffu;
    OSData *d = OSDynamicCast(OSData, e->getProperty(key));
    if (!d || d->getLength() < 4) return 0xffffffffu;
    return *(const UInt32 *)d->getBytesNoCopy();
}

bool XeDiag::start(IOService *provider)
{
    IOLog("XeDiag: start called\n");
    if (!IOService::start(provider)) return false;
    setProperty("diagState", "up");

    IOService *gpu = nullptr;
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) { setProperty("diagState", "no-match-dict"); return false; }
    OSIterator *it = IOService::getMatchingServices(match);
    if (it) {
        OSObject *o;
        while ((o = it->getNextObject())) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s) continue;
            UInt32 v = propU32(s, "vendor-id");
            UInt32 c = propU32(s, "class-code");
            if (v == 0x8086u && ((c >> 16) & 0xff) == 0x03) { gpu = s; break; }
        }
        it->release();
    }
    match->release();

    if (!gpu) { IOLog("XeDiag: no Intel VGA device\n"); setProperty("diagState", "no-gpu"); return false; }

    setProperty("diagGpuVendor", propU32(gpu, "vendor-id"), 32);
    setProperty("diagGpuDevice", propU32(gpu, "device-id"), 32);
    setProperty("diagGpuClass", propU32(gpu, "class-code"), 32);
    const char *nm = gpu->getName();
    if (nm) setProperty("diagGpuName", nm);

    IOItemCount n = gpu->getDeviceMemoryCount();
    setProperty("diagMemCount", (unsigned int)n, 32);

    IODeviceMemory *md = nullptr;
    for (IOItemCount i = 0; i < n; i++) {
        IODeviceMemory *m = gpu->getDeviceMemoryWithIndex((unsigned int)i);
        if (!m) continue;
        char key[20];
        snprintf(key, sizeof(key), "diagMemLen%u", (unsigned)i);
        setProperty(key, (unsigned long long)m->getLength(), 64);
        if (!md && m->getLength() >= 0x1000000ULL) md = m;
    }
    if (!md && n) md = gpu->getDeviceMemoryWithIndex(0);
    if (!md) { setProperty("diagState", "no-mem"); return false; }

    fLen = md->getLength();
    setProperty("diagBarBytes", (unsigned long long)fLen, 64);

    fMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, 0, fLen);
    if (!fMap) { setProperty("diagState", "map-failed"); return false; }
    fReg = (volatile UInt32 *)fMap->getVirtualAddress();
    setProperty("diagState", "mapped");

    for (unsigned i = 0; i < NWIN; i++) {
        UInt64 base = wins[i].base;
        if (base + WIN_BYTES > fLen) continue;
        UInt32 *buf = (UInt32 *)IOMalloc(WIN_BYTES);
        if (!buf) continue;
        for (unsigned d = 0; d < WIN_DWORDS; d++) buf[d] = fReg[(base / 4) + d];
        OSData *od = OSData::withBytes(buf, WIN_BYTES);
        if (od) { setProperty(wins[i].name, od); od->release(); }
        IOFree(buf, WIN_BYTES);
    }

    setProperty("diagState", "done");
    registerService();
    return true;
}
