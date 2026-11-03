#!/usr/bin/env python3
"""A-80 P2b: page-off on the plugin side (0026's page-off half, ported onto the A-79 tree).

Every edit asserts it applied exactly once, so a mismatched anchor stops the whole patch.
Usage: patch_a80.py [plugin_dir]
"""
import io
import os
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "/Users/change48/src/NvGfx-xe-2.6.2/plugin/"
if not P.endswith("/"):
    P += "/"
EDITS = []


def edit(path, old, new, count=1, tag=""):
    EDITS.append((path, old, new, count, tag))


# ------------------------------------------------------------------ nvmtl_vk.h
edit(P + "nvmtl_vk.h",
     "int nvmtl_vk_accel_surface_wait(void);",
     """/* A-80 §36.12 page-off: the accelerator's selectors 5-8 (current mark / wait for work / done / detach) */
int nvmtl_vk_po_on(void);
int nvmtl_vk_po_current(uint32_t surfaceID);
uint64_t nvmtl_vk_po_wait(uint32_t ms);
void nvmtl_vk_po_done(uint32_t surfaceID, uint32_t seq, int ok);
void nvmtl_vk_po_detach(uint32_t surfaceID);
int nvmtl_vk_accel_surface_wait(void);""",
     tag="h1 po prototypes")

# ------------------------------------------------------------------ nvmtl_vk.c
VK = P + "nvmtl_vk.c"

edit(VK,
     "    g_lib = dlopen(NVMTL_ICD_LIB, RTLD_NOW | RTLD_LOCAL);",
     """    /* A-80 §37.3 phase 0: /private/tmp/xemtl-noccs turns ANV's render compression (CCS) off for this process, to
       measure what the private copies' compression is worth.  Read once, before the driver loads (INTEL_DEBUG is
       parsed when it starts). */
    if (access("/private/tmp/xemtl-noccs", F_OK) == 0) {
        const char *was = getenv("INTEL_DEBUG"); char v[256];
        snprintf(v, sizeof v, "%s%snoccs", was && *was ? was : "", was && *was ? "," : "");
        setenv("INTEL_DEBUG", v, 1);
        nvlog("vk: render compression OFF (INTEL_DEBUG=%s, /private/tmp/xemtl-noccs)", v);
    }
    g_lib = dlopen(NVMTL_ICD_LIB, RTLD_NOW | RTLD_LOCAL);""",
     tag="vk noccs sentinel")

PO = r'''/* ===== A-80 §36.12 page-off: the accelerator's selectors 5-8 ================================================
   WindowServer's full-size surfaces are drawn into a private optimal-tiling copy (Tile4, and with CCS_E render
   compression on this GPU); their own pages stay linear and are brought up to date only when the CPU locks the
   surface or the plane scans it -- XeGfxAccel's device cache on the surface asks this process's page-off thread
   for it (A-79 shipped the accelerator halves: selectors 5-8, the CPU lock hook op 0 and the accounting, with no
   callers until now).
   Selector 5 (id) = the GPU copy is newer than the pages now, 0 probes.  6 (ms) = wait for a request, answers
   id << 32 | seq.  7 (id, seq, ok) = the copy is done.  8 (id) = our copy is gone.
   Off with /Library/GPUBundles/.xemtl-no-pageoff (across boots) or /private/tmp/xemtl-no-pageoff (until the next
   boot); XEMTL_NO_PAGEOFF keeps it off too.  A surface that already has a copy keeps it when the switch goes off. */
int nvmtl_vk_po_on(void)
{
    static int ws = -1, sel = 0; static _Atomic uint64_t checked; static _Atomic int on;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer") && !getenv("XEMTL_NO_PAGEOFF");
    if (!ws || !nvmtl_vk_surface_host_on()) return 0;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        atomic_store(&checked, now);
        const int want = access("/Library/GPUBundles/.xemtl-no-pageoff", F_OK) != 0
                      && access("/private/tmp/xemtl-no-pageoff", F_OK) != 0;
        if (want && sel == 0) {
            const uint64_t in = 0;
            sel = (nvmtl_mark_conn() && IOConnectCallScalarMethod(g_mark_conn, 5, &in, 1, NULL, NULL) == KERN_SUCCESS) ? 1 : -1;
            nvlog("page-off: the accelerator %s selector 5", sel > 0 ? "takes" : "does NOT take");
        }
        if (want != atomic_load(&on)) nvlog("page-off: new private surface copies %s", want && sel > 0 ? "ON" : "off");
        atomic_store(&on, want);
    }
    return atomic_load(&on) && sel > 0;
}
/* selector 5: this surface's private copy is newer than its pages (0 = probe whether the accelerator takes it) */
int nvmtl_vk_po_current(uint32_t id)
{
    const uint64_t in = id;
    return id && nvmtl_mark_conn() && IOConnectCallScalarMethod(g_mark_conn, 5, &in, 1, NULL, NULL) == KERN_SUCCESS;
}
/* selector 6: the page-off thread waits here for work; the answer is id << 32 | seq, 0 = timeout */
uint64_t nvmtl_vk_po_wait(uint32_t ms)
{
    const uint64_t in = ms; uint64_t out = 0; uint32_t n = 1;
    if (!nvmtl_mark_conn() || IOConnectCallScalarMethod(g_mark_conn, 6, &in, 1, &out, &n) != KERN_SUCCESS) { usleep(100000); return 0; }
    return out;
}
/* selector 7: the request is served (ok 1) or could not be (0); the waiting CPU lock is released either way */
void nvmtl_vk_po_done(uint32_t id, uint32_t seq, int ok)
{
    const uint64_t in[3] = { id, seq, ok ? 1u : 0u };
    if (nvmtl_mark_conn()) (void)IOConnectCallScalarMethod(g_mark_conn, 7, in, 3, NULL, NULL);
}
/* selector 8: this process has no private copy of the surface any more */
void nvmtl_vk_po_detach(uint32_t id)
{
    const uint64_t in = id;
    if (id && nvmtl_mark_conn()) (void)IOConnectCallScalarMethod(g_mark_conn, 8, &in, 1, NULL, NULL);
}
'''

edit(VK,
     "/* ===== end A-77 Tile4 =============================================================================== */",
     PO + "/* ===== end A-77 Tile4 / A-80 page-off =============================================================== */",
     tag="vk po selectors")

# ------------------------------------------------------------------ NVMTLObjects.h
edit(P + "NVMTLObjects.h",
     """  uint32_t _imgGen, _t4gen;   // A-78 §32.16: the root's image generation (bumped when it is swapped in place), and the one
                              // this texture's own views were made of
  BOOL _stex;""",
     """  uint32_t _imgGen, _t4gen;   // A-78 §32.16: the root's image generation (bumped when it is swapped in place), and the one
                              // this texture's own views were made of
  BOOL _po, _poWant, _poDemote;   // A-80 §36.12: the image is a private copy of WindowServer's surface, paged off on demand
  uint32_t _poCount; uint64_t _poWinNs;   // A-80 §36.12: page-offs in the current one-second window
  BOOL _poDrawn; uint64_t _poBornNs;      // A-80 §36.12: the GPU drew the copy, and when it was made
  BOOL _poCand; uint32_t _poDraws;        // A-80 §36.12: drawn over its pages until it proves long-lived (nvmtl_po_promote)
  BOOL _stex;""",
     tag="h2 po fields")

# ------------------------------------------------------------------ NVMTLObjects.m
OBJ = P + "NVMTLObjects.m"

PO_MACH = r'''
// <wtl> A-80 §36.12 page-off: WindowServer surfaces with a private copy, by surface ID (weak), and the thread that
// serves the accelerator's page-off requests.  It copies the private image into the surface's pages
// (nvmtl_surface_host_xfer: a GPU copy on the device queue, which runs after everything committed before) and reports
// back.  It takes no IOSurface lock and no surface sync: the request comes from inside some thread's IOSurfaceLock.
// A surface paged off more than 4 times in a second (a capture surface the CPU reads every frame) is marked for
// demotion to an image over its pages (nvmtl_po_demote, NVMTLEventSync.m).
static NSMapTable *g_po_roots;
static pthread_mutex_t g_po_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_po_served, g_po_failed;
NVMTLTexture *nvmtl_po_root(uint32_t sid)
{
    if (!sid || !g_po_roots) return nil;
    pthread_mutex_lock(&g_po_mu); NVMTLTexture *t = [g_po_roots objectForKey:@(sid)]; pthread_mutex_unlock(&g_po_mu);
    return t;
}
// §37.7 coherence: every root texture this process makes over a surface (weakly, keyed by the IOSurfaceRef, which each
// root retains).  A surface takes a private copy only while that copy is its ONE root: another root would read and write
// the pages under it.
static NSMapTable *g_surf_roots;
void nvmtl_roots_add(NVMTLTexture *t)
{
    if (!t || !t->_surf || t->_parent) return;
    pthread_mutex_lock(&g_po_mu);
    if (!g_surf_roots) g_surf_roots = [NSMapTable strongToStrongObjectsMapTable];
    NSValue *k = [NSValue valueWithPointer:t->_surf];
    NSHashTable *set = [g_surf_roots objectForKey:k];
    if (!set) { set = [NSHashTable weakObjectsHashTable]; [g_surf_roots setObject:set forKey:k]; }
    [set addObject:t];
    if (g_surf_roots.count > 4096) {
        NSMutableArray *dead = [NSMutableArray new];
        for (NSValue *v in g_surf_roots) if (![[g_surf_roots objectForKey:v] anyObject]) [dead addObject:v];
        for (NSValue *v in dead) [g_surf_roots removeObjectForKey:v];
    }
    pthread_mutex_unlock(&g_po_mu);
}
// §37.7: a root going away writes its private copy back inside its dealloc, under the surface's sync; a new root waits
// here for that write-back before it is used, or the write-back could land over what the new root draws.
void nvmtl_surface_barrier(NVMTLTexture *t)
{
    if (t && t->_surf) @synchronized (nvmtl_surface_sync(t)) { }
}
// live roots over s other than except
int nvmtl_roots_others(IOSurfaceRef s, NVMTLTexture *except)
{
    int n = 0;
    pthread_mutex_lock(&g_po_mu);
    NSHashTable *set = g_surf_roots ? [g_surf_roots objectForKey:[NSValue valueWithPointer:s]] : nil;
    for (NVMTLTexture *r in set) if (r != except) n++;
    pthread_mutex_unlock(&g_po_mu);
    return n;
}
static void *nvmtl_po_service(void *arg)
{
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) @autoreleasepool {
        const uint64_t req = nvmtl_vk_po_wait(1000);
        if (!req) continue;
        const uint32_t sid = (uint32_t)(req >> 32), seq = (uint32_t)req;
        NVMTLTexture *r = nvmtl_po_root(sid);
        int ok = 0; const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (r && r->_po && r->_surf && (uint32_t)IOSurfaceGetID(r->_surf) == sid)
            ok = nvmtl_surface_host_xfer(r, IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane), 0) == 0;
        nvmtl_vk_po_done(sid, seq, ok);
        const uint64_t n = atomic_fetch_add(ok ? &g_po_served : &g_po_failed, 1) + 1;
        if (r && ok) {
            const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (now - r->_poWinNs > 1000000000ull) { r->_poWinNs = now; r->_poCount = 0; }
            if (++r->_poCount > 4 && !r->_poDemote) { r->_poDemote = YES; nvlog("page-off: surface %u paged off %u times in a second -- back to its pages", sid, r->_poCount); }
        }
        if (n <= 16 || n % 1000 == 0)
            nvlog("page-off: surface %u %s in %.2f ms (%llu served, %llu failed)", sid, ok ? "copied to its pages" : "NOT copied (no copy of ours)",
                  (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6, (unsigned long long)atomic_load(&g_po_served), (unsigned long long)atomic_load(&g_po_failed));
    }
    return NULL;
}
// A surface whose private copy went within 2 s of being made, or was never drawn, is not given another one: WindowServer
// wraps some surfaces it only samples in short-lived render-target textures (each one would be a new 36 MB copy).
static struct { uint32_t sid; const void *surf; } g_po_deny[64];
static unsigned g_po_ndeny;
int nvmtl_po_denied(IOSurfaceRef s)
{
    const uint32_t sid = (uint32_t)IOSurfaceGetID(s); int hit = 0;
    pthread_mutex_lock(&g_po_mu);
    for (unsigned i = 0; i < 64 && !hit; i++) hit = g_po_deny[i].sid == sid && g_po_deny[i].surf == (const void *)s;
    pthread_mutex_unlock(&g_po_mu);
    return hit;
}
static void nvmtl_po_gone(NVMTLTexture *r)
{
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
    const double life = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - r->_poBornNs) / 1e9;
    int rc = 0;
    if (r->_poDrawn) {   // the kernel may still hold the copy current: write it to the pages now, while the image is here
        @synchronized (nvmtl_surface_sync(r)) { rc = nvmtl_surface_host_xfer(r, IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane), 0); }
        if (rc) nvlog("page-off: surface %u: copy to its pages FAILED as its copy went (rc %d)", sid, rc);
    }
    const int deny = !r->_poDrawn || life < 2.0;
    if (deny) { pthread_mutex_lock(&g_po_mu); g_po_deny[g_po_ndeny++ % 64].sid = sid; g_po_deny[(g_po_ndeny - 1) % 64].surf = (const void *)r->_surf; pthread_mutex_unlock(&g_po_mu); }
    static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 256)
        nvlog("page-off: surface %u: private copy gone after %.2f s, %s%s", sid, life, r->_poDrawn ? "drawn, copied to its pages" : "never drawn",
              deny ? " -- no more copies for it" : "");
}
// the surface's pages imported once, here, before the page-off thread can race to do it
int nvmtl_po_prepare(NVMTLTexture *t)
{
    if (t->_hostState == 0)
        t->_hostState = nvmtl_vk_surface_host_import(IOSurfaceGetBaseAddress(t->_surf), IOSurfaceGetAllocSize(t->_surf), &t->_hostBuf) ? -1 : 1;
    return t->_hostState > 0;
}
void nvmtl_po_register(NVMTLTexture *t)
{
    static dispatch_once_t once;
    pthread_mutex_lock(&g_po_mu);
    if (!g_po_roots) g_po_roots = [NSMapTable strongToWeakObjectsMapTable];
    [g_po_roots setObject:t forKey:@(IOSurfaceGetID(t->_surf))];
    t->_poBornNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const NSUInteger n = g_po_roots.count;
    pthread_mutex_unlock(&g_po_mu);
    dispatch_once(&once, ^{ pthread_t th; if (pthread_create(&th, NULL, nvmtl_po_service, NULL) == 0) pthread_detach(th); });
    nvlog("page-off: surface %u %ux%u drawn into a private copy (%lu surfaces)", (unsigned)IOSurfaceGetID(t->_surf), t->_i.w, t->_i.h, (unsigned long)n);
}
// </wtl>
'''

edit(OBJ,
     """    static unsigned n; if (n++ < 6 || n % 2000 == 0) nvlog("surface %s through host import #%u: %ux%u surface %u rc %d", to_image ? "in" : "out", n, r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf), rc);
    return rc;
}
// </wtl>""",
     """    static unsigned n; if (n++ < 6 || n % 2000 == 0) nvlog("surface %s through host import #%u: %ux%u surface %u rc %d", to_image ? "in" : "out", n, r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf), rc);
    return rc;
}
""" + PO_MACH,
     tag="obj po machinery")

edit(OBJ,
     """- (id<MTLHeap>)heap { return _heap ?: _subHeap; }
- (NSUInteger)heapOffset { return _heapOffset; }
- (void)dealloc {
    if (_sampledDepthView) nvmtl_vk_image_view_destroy(_sampledDepthView);
    nvmtl_tex_objs_gone++;
    if (_tbView || _tbAlias) { nvmtl_vk_texel_view_destroy(_tbAlias, _tbView); _tbView = _tbAlias = NULL; }
    if (_resid) nvmtl_resid_texture_gone(_resid);
    if (_ownView) nvmtl_vk_image_view_destroy(_ownView);
    if (_swzView) nvmtl_vk_image_view_destroy(_swzView);""",
     """- (id<MTLHeap>)heap { return _heap ?: _subHeap; }
- (NSUInteger)heapOffset { return _heapOffset; }
static void nvmtl_po_gone(NVMTLTexture *r);   // <wtl> A-80 §36.12
#include <objc/objc-sync.h>
static id nvmtl_surface_sync(NVMTLTexture *t);   // <wtl> A-80 §37.7
- (void)dealloc {
    // <wtl> A-80 §37.7: a private copy going away holds its surface's sync from here until its write-back is done, so a
    // new root (nvmtl_surface_barrier) cannot start drawing the pages before the old copy has landed on them
    id poSync = (!_parent && _po && _surf) ? nvmtl_surface_sync(self) : nil;
    if (poSync) objc_sync_enter(poSync);
    if (_sampledDepthView) nvmtl_vk_image_view_destroy(_sampledDepthView);
    nvmtl_tex_objs_gone++;
    if (_tbView || _tbAlias) { nvmtl_vk_texel_view_destroy(_tbAlias, _tbView); _tbView = _tbAlias = NULL; }
    if (_resid) nvmtl_resid_texture_gone(_resid);
    if (_ownView) nvmtl_vk_image_view_destroy(_ownView);
    if (_swzView) nvmtl_vk_image_view_destroy(_swzView);
    if (!_parent && _po && _surf) nvmtl_po_gone(self);   // <wtl> A-80 §36.12: the copy reaches the pages before the image goes
    if (poSync) objc_sync_exit(poSync);   // <wtl> A-80 §37.7""",
     tag="obj dealloc po")

edit(OBJ,
     """    if (!_parent && _hostState > 0) { nvmtl_vk_buffer_destroy(&_hostBuf); _hostState = 0; }
    if (_surfOwned && _surf) CFRelease(_surf);""",
     """    if (!_parent && _hostState > 0) { nvmtl_vk_buffer_destroy(&_hostBuf); _hostState = 0; }
    if (!_parent && _po && _surf) nvmtl_vk_po_detach((uint32_t)IOSurfaceGetID(_surf));   // <wtl> A-80 §36.12: our device cache goes
    if (_surfOwned && _surf) CFRelease(_surf);""",
     tag="obj dealloc detach")

edit(OBJ,
     """    if (r->_hostImg) { r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES; return; }   // CPU writes are already in the image
    @synchronized (nvmtl_surface_sync(r)) {""",
     """    if (r->_hostImg) { r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES; return; }   // CPU writes are already in the image
    // <wtl> A-80 §36.12: the private copy is behind only when the CPU wrote the pages since (a write unlock moves the seed)
    if (r->_po && r->_surfSynced && IOSurfaceGetSeed(r->_surf) == r->_surfSeed) return;
    // </wtl>
    @synchronized (nvmtl_surface_sync(r)) {""",
     tag="obj surface-in po skip")

edit(OBJ,
     """- (BOOL)nvmtlSurfaceOutDirtied:(BOOL)dirtied {
    NVMTLTexture *r = _parent ? _parent : self;
    if (!r->_surf) return YES;
    if (r->_hostImg) {""",
     """- (BOOL)nvmtlSurfaceOutDirtied:(BOOL)dirtied {
    NVMTLTexture *r = _parent ? _parent : self;
    if (!r->_surf) return YES;
    // <wtl> A-80 §36.12: no copy, no seed lock -- the accelerator's device cache is made current (its copy is newer) and
    // the pages are brought up to date when someone locks or scans them.  If the accelerator refuses, copy now as before.
    if (r->_po) {
        @synchronized (nvmtl_surface_sync(r)) {
            if (nvmtl_vk_po_current((uint32_t)IOSurfaceGetID(r->_surf))) {
                r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES; r->_poDrawn = YES;
                return YES;
            }
        }
        static unsigned nf; if (nf++ < 8) nvlog("page-off: surface %u: the accelerator took no current mark -- copied out now", (unsigned)IOSurfaceGetID(r->_surf));
    }
    // </wtl>
    if (r->_hostImg) {""",
     tag="obj surface-out po current")

# ------------------------------------------------------------------ NVMTLDevice.m
DEV = P + "NVMTLDevice.m"

edit(DEV,
     "    if (surf && plane == 0 && IOSurfaceGetPlaneCount(surf) <= 1 && nvmtl_vk_surface_host_on()) {\n        size_t row = IOSurfaceGetBytesPerRow(surf), sh = IOSurfaceGetHeight(surf);",
     "    if (surf && plane == 0 && IOSurfaceGetPlaneCount(surf) <= 1 && nvmtl_vk_surface_host_on() && !t->_poWant) {\n        size_t row = IOSurfaceGetBytesPerRow(surf), sh = IOSurfaceGetHeight(surf);",
     tag="dev image_create po want")

PO_WANT = r'''// <wtl> A-80 §36.12 page-off (0026's plugin half): a full-size BGRA8 surface WindowServer draws into, that is not a
// scanout surface, gets a private image (optimal tiling: Tile4 on this GPU) instead of an image over its pages.  The
// pages stay linear; the accelerator's device cache pages the copy off into them when the CPU locks the surface or the
// display scans it (nvmtl_po_*, NVMTLObjects.m).
// §37.10: /Library/GPUBundles/.xemtl-pageoff-lazy or /private/tmp/xemtl-pageoff-lazy makes a surface be promoted only
// after it has lived 3 s and been drawn by 100 commits (A-80's first half); without it the copy is made at once.
static int nvmtl_po_eager(void)
{
    static _Atomic uint64_t checked; static _Atomic int on = 1;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (now - atomic_load(&checked) > 1000000000ull) {
        atomic_store(&checked, now);
        const int v = access("/Library/GPUBundles/.xemtl-pageoff-lazy", F_OK) != 0 && access("/private/tmp/xemtl-pageoff-lazy", F_OK) != 0;
        if (v != atomic_exchange(&on, v)) nvlog("page-off: private copies %s", v ? "at once (eager)" : "after 3 s and 100 commits");
    }
    return atomic_load(&on);
}
static int nvmtl_po_want(IOSurfaceRef surf, NSUInteger plane, size_t w, size_t h, uint32_t vkf, NSUInteger usage)
{
    if (!surf || plane != 0 || IOSurfaceGetPlaneCount(surf) > 1 || !(usage & MTLTextureUsageRenderTarget)) return 0;
    if (vkf != VK_FORMAT_B8G8R8A8_UNORM && vkf != VK_FORMAT_B8G8R8A8_SRGB) return 0;
    if (w != IOSurfaceGetWidth(surf) || h != IOSurfaceGetHeight(surf) || IOSurfaceGetBytesPerRow(surf) < w * 4 || w < 1024 || h < 600) return 0;
    if (nvmtl_vk_is_scanout((uint32_t)IOSurfaceGetID(surf))) return 0;
    return nvmtl_vk_po_on() && !nvmtl_po_denied(surf);
}
void nvmtl_po_register(NVMTLTexture *t);
void nvmtl_roots_add(NVMTLTexture *t);
int nvmtl_roots_others(IOSurfaceRef s, NVMTLTexture *except);
void nvmtl_po_demote_now(NVMTLTexture *r);
void nvmtl_surface_barrier(NVMTLTexture *t);
int nvmtl_po_prepare(NVMTLTexture *t);
NVMTLTexture *nvmtl_po_root(uint32_t sid);
// §37.7 diagnosis: every texture WindowServer makes over a large surface, and what it became (first 600)
static void nvmtl_roots_log(IOSurfaceRef surf, size_t w, size_t h, MTLTextureDescriptor *d, const char *what)
{
    static int ws = -1; static _Atomic unsigned n;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!ws || !surf || w < 1024 || h < 600 || atomic_fetch_add(&n, 1) >= 600) return;
    nvlog("roots: surface %u %zux%zu fmt %lu usage 0x%lx seed %u roots %d: %s", (unsigned)IOSurfaceGetID(surf), w, h,
          (unsigned long)d.pixelFormat, (unsigned long)d.usage, IOSurfaceGetSeed(surf), nvmtl_roots_others(surf, nil), what);
}
'''

edit(DEV,
     "- (id<MTLTexture>)nvmtlNewSurfaceTexture:(MTLTextureDescriptor *)d surface:(IOSurfaceRef)surf plane:(NSUInteger)plane cache:(NSMapTable *)cache {",
     PO_WANT + "- (id<MTLTexture>)nvmtlNewSurfaceTexture:(MTLTextureDescriptor *)d surface:(IOSurfaceRef)surf plane:(NSUInteger)plane cache:(NSMapTable *)cache {",
     tag="dev po want block")

edit(DEV,
     """    NVMTLTexture *backing = key ? [cache objectForKey:key] : nil;
    // Failed BAR imports use a private image.""",
     """    NVMTLTexture *backing = key ? [cache objectForKey:key] : nil;
    // <wtl> A-80 §36.12: a surface with a private copy has ONE copy, whatever 4-byte format a wrapper asks for
    if (surf && !backing && bpp == 4) {
        NVMTLTexture *po = nvmtl_po_root((uint32_t)IOSurfaceGetID(surf));
        if (po && po->_po && po->_surf == surf && po->_plane == plane && po->_i.w == w && po->_i.h == h) backing = po;
    }
    const BOOL poWant = surf && !backing && nvmtl_po_want(surf, plane, w, h, vkf, d.usage) && !nvmtl_roots_others(surf, nil);   // §37.7: the one root
    // </wtl>
    // Failed BAR imports use a private image.""",
     tag="dev poWant")

edit(DEV,
     """            nvmtl_texture_desc_swizzle(alias, d);
            return alias;
        }
    }
    if (nvmtl_surface_image_create(t, surf, plane, (uint32_t)w, (uint32_t)h, vkf, bpp, a8, wantStex)) {""",
     """            nvmtl_texture_desc_swizzle(alias, d);
            nvmtl_roots_log(surf, w, h, d, backing->_po ? "view of its private copy" : "view of a root");
            return alias;
        }
    }
    // <wtl> A-80 §37.7: a new root over a surface whose private copy it cannot share: the copy goes back to the pages first
    const char *how = NULL;
    if (surf) {
        NVMTLTexture *po = nvmtl_po_root((uint32_t)IOSurfaceGetID(surf));
        if (po && po->_po && po->_surf == surf) { nvmtl_po_demote_now(po); how = "new root, its private copy demoted first"; }
    }
    // </wtl>
    t->_poWant = poWant && nvmtl_po_eager();   // <wtl> A-80 §36.12: over its pages first; a private copy only once it proves long-lived
    if (nvmtl_surface_image_create(t, surf, plane, (uint32_t)w, (uint32_t)h, vkf, bpp, a8, wantStex)) {""",
     tag="dev poWant apply")

edit(DEV,
     """    nvmtl_texture_desc_swizzle(t, d);
    if (key && !t->_vramOn) [cache setObject:t forKey:key];
    nvlog("newTextureWithDescriptor:iosurface:%p plane:%lu -> %p (%zux%zu fmt %lu)",""",
     """    nvmtl_texture_desc_swizzle(t, d);
    // <wtl> A-80 §36.12: eager -- a private copy now; lazy -- drawn over its pages until it has lived 3 s and been drawn 100 times
    if (t->_poWant && !t->_hostImg && !t->_vramOn && nvmtl_po_prepare(t)) { t->_po = YES; nvmtl_po_register(t); }
    else if (poWant && t->_hostImg && !t->_vramOn && !t->_tile4) { t->_poCand = YES; t->_poBornNs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
    t->_poWant = NO;
    // </wtl>
    if (key && !t->_vramOn) [cache setObject:t forKey:key];
    nvmtl_roots_add(t);   // <wtl> A-80 §37.7
    nvmtl_surface_barrier(t);   // §37.7: a write-back of a root going away finishes first
    nvmtl_roots_log(surf, w, h, d, how ? how : t->_po ? "new root, private copy" : t->_hostImg ? "new root over its pages" : "new root, other");
    nvlog("newTextureWithDescriptor:iosurface:%p plane:%lu -> %p (%zux%zu fmt %lu)",""",
     tag="dev po attach")

# ------------------------------------------------------------------ NVMTLEventSync.m
EV = P + "NVMTLEventSync.m"

PO_LAYOUT = r'''// <wtl> A-80 §36.12: a surface with a private copy that turned out to be scanned out (or that the CPU reads every
// frame) goes back to an image over its own pages: the copy is drawn into those pages (nvmtl_tile4_relayout waits for
// the earlier work), the root's image is swapped for the new one as nvmtl_tile4_inplace does, and our device cache is
// released.  From then on it is an ordinary surface again.
static void nvmtl_po_demote(NVMTLTexture *r)
{
    static _Atomic int fails;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
    r->_poDemote = NO;
    if (atomic_load(&fails) >= 3) return;
    NVMTLTexture *to = [NVMTLTexture new]; to->_tile4 = NO;
    if (nvmtl_surface_image_create(to, r->_surf, r->_plane, r->_i.w, r->_i.h, r->_i.fmt, r->_i.bpp, 0, r->_i.storage ? 1 : 0) || !to->_hostImg) {
        atomic_fetch_add(&fails, 1); nvlog("page-off: surface %u: no image over its pages -- keeps its private copy", sid); return; }
    to->_q = r->_q; to->_surf = r->_surf; CFRetain(to->_surf); to->_surfOwned = YES; to->_plane = r->_plane;
    to->_fmt = r->_fmt; to->_usage = r->_usage; to->_stex = to->_i.storage != 0;
    if (!nvmtl_tile4_relayout([r device], r, to)) { atomic_fetch_add(&fails, 1); nvlog("page-off: surface %u: copy to its pages failed -- keeps its private copy", sid); return; }
    @synchronized (nvmtl_surface_sync(r)) {
        @synchronized (r) {
            const nvk_image old = r->_i; r->_i = to->_i; to->_i = old;
            const nvk_buffer hb = r->_hostBuf; const int8_t hs = r->_hostState;
            r->_hostBuf = to->_hostBuf; r->_hostState = to->_hostState; to->_hostBuf = hb; to->_hostState = hs;
            r->_hostImg = YES; to->_hostImg = NO; r->_po = NO; r->_imgGen++;
            r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES;
        }
    }
    nvmtl_vk_po_detach(sid);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * (int64_t)NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                   ^{ (void)to; });	// holds the private image until then
    nvlog("page-off: surface %u %ux%u back to an image over its pages (%s)", sid, r->_i.w, r->_i.h,
          nvmtl_vk_is_scanout(sid) ? "it is scanned out" : r->_poCount > 4 ? "the CPU reads it often" : "another root needs its pages");
}
// A WindowServer surface that would take a private copy is first drawn over its own pages, and is promoted only after it
// has lived 3 s and been drawn by 100 commits (lazy): WindowServer's screenshot and capture surfaces live well under a
// second and are read by other processes without a lock that pages off.  The copy is made from the pages on the GPU and
// swapped in as nvmtl_po_demote does in reverse; the pages stay imported (r->_hostBuf) for the page-off copies.
static void nvmtl_po_promote(NVMTLTexture *r)
{
    static _Atomic int fails;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
    r->_poCand = NO;
    if (atomic_load(&fails) >= 3) return;
    NVMTLTexture *to = [NVMTLTexture new]; to->_tile4 = NO; to->_poWant = YES;   // a private image, not one over the pages
    if (nvmtl_surface_image_create(to, r->_surf, r->_plane, r->_i.w, r->_i.h, r->_i.fmt, r->_i.bpp, 0, r->_i.storage ? 1 : 0) || to->_hostImg || to->_vramOn) {
        atomic_fetch_add(&fails, 1); nvlog("page-off: surface %u: no private image -- stays over its pages", sid); return; }
    to->_poWant = NO;
    to->_q = r->_q; to->_surf = r->_surf; CFRetain(to->_surf); to->_surfOwned = YES; to->_plane = r->_plane;
    to->_fmt = r->_fmt; to->_usage = r->_usage; to->_stex = to->_i.storage != 0;
    if (!nvmtl_tile4_relayout([r device], r, to)) { atomic_fetch_add(&fails, 1); nvlog("page-off: surface %u: copy from its pages failed -- stays over them", sid); return; }
    @synchronized (nvmtl_surface_sync(r)) {
        @synchronized (r) {
            const nvk_image old = r->_i; r->_i = to->_i; to->_i = old;
            r->_hostImg = NO; r->_po = YES; r->_poDrawn = NO; r->_imgGen++;
            r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES;
        }
    }
    nvmtl_po_register(r);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * (int64_t)NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0),
                   ^{ (void)to; });	// holds the image over the pages until then
    nvlog("page-off: surface %u %ux%u promoted to a private copy after %u commits", sid, r->_i.w, r->_i.h, r->_poDraws);
}
// §37.7: a second root that cannot share the private copy is about to be made over the surface: the copy goes back to
// the pages first, so both roots then read and write the same pages
void nvmtl_po_demote_now(NVMTLTexture *r)
{
    if (r && r->_po && !r->_hostImg) { r->_poDemote = NO; nvmtl_po_demote(r); }
}
'''

edit(EV,
     """// the commit hook: after every WindowServer commit, any surface it drew whose layout is not the one it should have now
// is re-laid out here, on the committing thread, before the next frame is encoded.
static void nvmtl_tile4_after_commit(NVMTLCommandBuffer *cb)
{
    if (nvmtl_t4_in_relayout) return;
    static int ws = -1;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer") && !getenv("XEMTL_NO_TILE4")
                    && access("/private/tmp/xemtl-no-tile4", F_OK) != 0;
    if (!ws || nvmtl_t4_bisect() == 1) return;
    NSMutableArray *roots = nil, *wants = nil;
    for (id part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in ((NVMTLSubmissionPart *)part)->surfaces) {
            NVMTLTexture *r = t->_parent ? t->_parent : t;
            if (!r->_surf || !r->_hostImg || r->_vramOn || [roots containsObject:r]) continue;
            const int want = nvmtl_tile4_want(r);
            if (want == (r->_tile4 ? 1 : 0)) continue;
            if (!roots) { roots = [NSMutableArray new]; wants = [NSMutableArray new]; }
            [roots addObject:r]; [wants addObject:@(want)];
        }
    }
    for (NSUInteger i = 0; i < roots.count; i++) nvmtl_tile4_inplace(roots[i], [wants[i] intValue]);
}""",
     PO_LAYOUT + """// the commit hook: after every WindowServer commit, any surface it drew whose layout is not the one it should have now
// is re-laid out here, on the committing thread, before the next frame is encoded; and a surface with a private copy
// that turned out to be scanned out, or a candidate that has earned its copy, is changed here too (A-80 §36.12).
static void nvmtl_tile4_after_commit(NVMTLCommandBuffer *cb)
{
    if (nvmtl_t4_in_relayout) return;
    static int ws = -1;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer");
    if (!ws) return;
    // A-80: the Tile4 half keeps its own switches; the page-off half runs even with Tile4 off (a private copy is linear then)
    const int t4off = getenv("XEMTL_NO_TILE4") != NULL || access("/private/tmp/xemtl-no-tile4", F_OK) == 0 || nvmtl_t4_bisect() == 1;
    NSMutableArray *roots = nil, *wants = nil;
    for (id part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in ((NVMTLSubmissionPart *)part)->surfaces) {
            NVMTLTexture *r = t->_parent ? t->_parent : t;
            if (r->_po && r->_surf && !r->_hostImg && ![roots containsObject:r] &&
                (r->_poDemote || nvmtl_vk_is_scanout((uint32_t)IOSurfaceGetID(r->_surf)))) {   // <wtl> A-80 §36.12
                if (!roots) { roots = [NSMutableArray new]; wants = [NSMutableArray new]; }
                [roots addObject:r]; [wants addObject:@(-1)]; continue;
            }
            if (r->_poCand && r->_hostImg && !r->_tile4 && ![roots containsObject:r] && ++r->_poDraws >= 100 &&   // <wtl> A-80 §36.12 lazy
                clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - r->_poBornNs >= 3000000000ull) {
                if (!roots) { roots = [NSMutableArray new]; wants = [NSMutableArray new]; }
                [roots addObject:r]; [wants addObject:@(-2)]; continue;
            }
            if (t4off || !r->_surf || !r->_hostImg || r->_vramOn || [roots containsObject:r]) continue;
            const int want = nvmtl_tile4_want(r);
            if (want == (r->_tile4 ? 1 : 0)) continue;
            if (!roots) { roots = [NSMutableArray new]; wants = [NSMutableArray new]; }
            [roots addObject:r]; [wants addObject:@(want)];
        }
    }
    for (NSUInteger i = 0; i < roots.count; i++) {
        if ([wants[i] intValue] == -2) {   // <wtl> A-80 §36.12
            NVMTLTexture *r = roots[i]; const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
            if (nvmtl_vk_po_on() && !nvmtl_vk_is_scanout(sid) && !nvmtl_po_denied(r->_surf) && !nvmtl_roots_others(r->_surf, r)) nvmtl_po_promote(r);
            else r->_poCand = NO;
        } else if ([wants[i] intValue] < 0) nvmtl_po_demote(roots[i]);   // <wtl> A-80 §36.12
        else nvmtl_tile4_inplace(roots[i], [wants[i] intValue]);
    }
}""",
     tag="ev after_commit po")


def main():
    ok = True
    for path, old, new, count, tag in EDITS:
        if not os.path.exists(path):
            print("FAIL %-24s %s: missing" % (tag, path))
            ok = False
            continue
        src = io.open(path, encoding="utf-8").read()
        n = src.count(old)
        if n != count:
            print("FAIL %-24s %s: anchor found %d times (expected %d)" % (tag, path.rsplit("/", 1)[-1], n, count))
            ok = False
            continue
        io.open(path, "w", encoding="utf-8").write(src.replace(old, new, count))
        print("ok   %-24s %s" % (tag, path.rsplit("/", 1)[-1]))
    print("ALL_OK" if ok else "PATCH_INCOMPLETE")
    return 0 if ok else 1


sys.exit(main())
