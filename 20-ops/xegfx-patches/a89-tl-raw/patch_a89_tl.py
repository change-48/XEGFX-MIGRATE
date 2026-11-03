#!/usr/bin/env python3
"""A-89 P1: per-frame absolute timestamps + the panel's own refresh counter in the iop_tl ring.

Why: A-88 showed the frame time is quantised to whole refreshes (24.8 = 3 x 8.33) and that 23-29% of the frames
already land on 2 refreshes (16.7 ms = 60 fps).  Same driver, same load, some frames make it and some do not, so the
difference is *phase*, not capability.  The existing ring keeps only relative us between marks, which is exactly what
throws the phase away -- two frames with identical enter->ret can sit at opposite ends of the refresh.

What this adds (observation only, no behaviour change):
  * sub   -- mach_absolute_time() at performTransaction entry, i.e. the display thread's per-frame grid point.
             For the deferred path the ring used to keep only the worker's arm time, so the submission instant was lost.
  * vcnt  -- BAR0 0x70040 (the panel's frame counter) read at that same instant.  It steps once per vblank, so the
             delta between two frames is the *hardware* answer to "how many refreshes did this frame take" -- no
             timestamp bucketing involved.
  * a second sysctl, debug.xegfxaccel_iop_tl_raw, that dumps the whole ring as machine-readable columns (the A-72 one
             keeps printing its us table, so A-88's tooling still works).

Usage: patch_a89_tl.py <NVRM dir>   (the dir that contains accel/nvrm-accel.cpp)
"""
import io
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "."
if not P.endswith("/"):
    P += "/"
C = P + "accel/nvrm-accel.cpp"
H = P + "accel/xegfx_accel_names.h"
n = 0


def edit(path, old, new, tag):
    global n
    src = io.open(path, encoding="utf-8", newline="").read()
    c = src.count(old)
    assert c == 1, "FAIL %s: anchor found %d times" % (tag, c)
    io.open(path, "w", encoding="utf-8", newline="").write(src.replace(old, new, 1))
    n += 1
    print("ok   %-38s" % tag)


# ------------------------------------------------------------------ 1. the ring record
edit(C,
     """struct NVIopTl {
    uint64_t enter, src, vb, copy, ret;   // mach_absolute_time() marks; 0 = that stage was not reached
    uint64_t mark, arm, latch;            // A-75, path 2 only: mark wait start, the arming write, the panel's latch
    uint32_t sid, pitch;""",
     """struct NVIopTl {
    uint64_t enter, src, vb, copy, ret;   // mach_absolute_time() marks; 0 = that stage was not reached
    uint64_t mark, arm, latch;            // A-75, path 2 only: mark wait start, the arming write, the panel's latch
    // A-89: the display thread's grid point for this frame.  enter is overwritten by the flip worker's arm (A-75
    // semantics), so without sub the ring has no timestamp for the instant WindowServer handed the frame over --
    // which is the only instant that can be compared across frames.
    uint64_t sub;                         // mach_absolute_time() at performTransaction entry
    uint32_t vcnt;                        // BAR0 0x70040 read at that same instant: the panel's refresh counter
    uint32_t sid, pitch;""",
     "ring: sub + vcnt fields")

edit(C,
     "    d->mark = r->mark; d->arm = r->arm; d->latch = r->latch;",
     "    d->mark = r->mark; d->arm = r->arm; d->latch = r->latch;\n"
     "    d->sub = r->sub; d->vcnt = r->vcnt;",
     "ring publish: copy the new fields")

# ------------------------------------------------------------------ 2. the deferred path carries them
edit(C, "struct NVXeFlipTl { uint64_t enter, mark, arm, latch; };",
     "struct NVXeFlipTl { uint64_t enter, mark, arm, latch; uint64_t sub; uint32_t vcnt; };",
     "NVXeFlipTl: sub + vcnt")

edit(C, "struct NVXeFlipPend { void *pipe; IOSurface *s; unsigned w, h, pitch; void *dst; unsigned dstPitch; };",
     "struct NVXeFlipPend { void *pipe; IOSurface *s; unsigned w, h, pitch; void *dst; unsigned dstPitch;\n"
     "                      uint64_t sub; uint32_t vcnt; };   /* A-89: when the display thread submitted it, and the\n"
     "                                                          panel's refresh counter at that instant */",
     "NVXeFlipPend: sub + vcnt")

edit(C,
     """static void nvAccelXeFlipDeferQueue(void *pipe, IOSurface *s, unsigned w, unsigned h, unsigned pitch,
                                    void *dst, unsigned dstPitch)""",
     """static void nvAccelXeFlipDeferQueue(void *pipe, IOSurface *s, unsigned w, unsigned h, unsigned pitch,
                                    void *dst, unsigned dstPitch, uint64_t sub, uint32_t vcnt)""",
     "DeferQueue: submission arguments")

edit(C,
     "        q->pipe = pipe; q->s = s; q->w = w; q->h = h; q->pitch = pitch;\n"
     "        q->dst = dst; q->dstPitch = dstPitch;\t/* A-74: where the frame goes if the GGTT slot cannot take it */",
     "        q->pipe = pipe; q->s = s; q->w = w; q->h = h; q->pitch = pitch;\n"
     "        q->dst = dst; q->dstPitch = dstPitch;\t/* A-74: where the frame goes if the GGTT slot cannot take it */\n"
     "        q->sub = sub; q->vcnt = vcnt;\t\t/* A-89: the display thread's instant, not the worker's */",
     "DeferQueue: store them in the slot")

edit(C,
     "        NVXeFlipPend p; p.pipe = nullptr; p.s = nullptr; p.w = p.h = p.pitch = 0; p.dst = nullptr; p.dstPitch = 0;",
     "        NVXeFlipPend p; p.pipe = nullptr; p.s = nullptr; p.w = p.h = p.pitch = 0; p.dst = nullptr; p.dstPitch = 0;\n"
     "        p.sub = 0; p.vcnt = 0;",
     "worker: reset the new fields")

edit(C,
     "        NVXeFlipTl ftl = {}; ftl.enter = mach_absolute_time();",
     "        NVXeFlipTl ftl = {}; ftl.enter = mach_absolute_time();\n"
     "        ftl.sub = p.sub; ftl.vcnt = p.vcnt;\t// A-89: keep the submission instant through the arm",
     "worker: hand the submission instant to the arm")

edit(C,
     '''    NVIopTl r = {};
    r.enter = t->enter; r.mark = t->mark; r.arm = t->arm; r.latch = t->latch;''',
     '''    NVIopTl r = {};
    r.enter = t->enter; r.mark = t->mark; r.arm = t->arm; r.latch = t->latch;
    r.sub = t->sub ? t->sub : t->enter; r.vcnt = t->vcnt;   // A-89''',
     "publishFlip: sub + vcnt")

# ------------------------------------------------------------------ 3. submission instant at the entry
edit(C,
     """    const uint64_t txnStart = mach_absolute_time();
    NVIopTl tl = {}; tl.enter = txnStart;   // A-72 timeline record for this frame""",
     """    const uint64_t txnStart = mach_absolute_time();
    NVIopTl tl = {}; tl.enter = txnStart;   // A-72 timeline record for this frame
    // A-89: the per-frame grid point and the panel's own refresh counter at that instant.  One MMIO read, on a path that
    // already reads the same register at 10 Hz (nvXePlaneWatch) -- the counter is what the panel counts, so the delta
    // between two frames is "how many refreshes this frame took", measured in hardware rather than inferred from us.
    tl.sub = txnStart;
    tl.vcnt = gDispMmio ? gDispMmio[NV_DISP_FRAME_COUNTER / 4] : 0u;""",
     "performTransaction: sub + vcnt at entry")

edit(C,
     "                nvAccelXeFlipDeferQueue(this, s, W, H, P, q.kva, (unsigned)q.pitch);",
     "                nvAccelXeFlipDeferQueue(this, s, W, H, P, q.kva, (unsigned)q.pitch, txnStart, tl.vcnt);",
     "performTransaction: pass them to the worker")

# ------------------------------------------------------------------ 4. the machine-readable dump
RAW = r'''// A-89: the same ring, as columns instead of a formatted us table.  Absolute mach timestamps only -- every interval
// (and, more to the point, every phase) is derived downstream by work/a89/classify.py, so a change of question does not
// need another kext.  200 frames is ~5 s at 40 fps: read it twice during one drag to get ~400 consecutive frames.
static int nvIopTlRawText(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    const size_t cap = 200 * 128 + 512;
    char *b = (char *)IOMalloc(cap);
    if (!b) return ENOMEM;
    const SInt64 n = __atomic_load_n(&gIopTlN, __ATOMIC_ACQUIRE);
    const unsigned cnt = n < 200 ? (unsigned)n : 200u;
    size_t o = snprintf(b, cap, "# idx sub_ns vcnt enter_ns arm_ns latch_ns ret_ns path sid ok w h pitch\n");
    for (unsigned k = 0; k < cnt && o + 128 < cap; k++) {
        const SInt64 idx = n - cnt + (SInt64)k;
        const NVIopTl *r = &gIopTl[(unsigned)idx & 255u];
        if (r->seq != (uint32_t)(idx + 1)) continue;      // being filled right now -- skip
        o += snprintf(b + o, cap - o, "%lld %llu %u %llu %llu %llu %llu %u %u %u %u %u %u\n",
                      (long long)idx, (unsigned long long)r->sub, (unsigned)r->vcnt,
                      (unsigned long long)r->enter, (unsigned long long)r->arm,
                      (unsigned long long)r->latch, (unsigned long long)r->ret,
                      (unsigned)r->path, (unsigned)r->sid, (unsigned)r->ok,
                      (unsigned)r->w, (unsigned)r->h, (unsigned)r->pitch);
    }
    const int err = SYSCTL_OUT(req, b, strlen(b) + 1);
    IOFree(b, cap);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop_tl_raw, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvIopTlRawText, "A",
            "A-89: the last 200 frames as absolute columns (sub_ns vcnt enter arm latch ret path sid ok w h pitch)");
'''
edit(C,
     '''SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop_tl, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvIopTlText, "A",
            "A-72: the last 200 copied frames as a per-stage timeline in us (enter, source mapped, vblank waited, copy done, return)");''',
     '''SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop_tl, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvIopTlText, "A",
            "A-72: the last 200 copied frames as a per-stage timeline in us (enter, source mapped, vblank waited, copy done, return)");
''' + RAW,
     "new sysctl: nvaccel_iop_tl_raw")

edit(C,
     "    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_tl; sysctl_register_oid(&sysctl__debug_nvaccel_iop_tl); }",
     "    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_tl; sysctl_register_oid(&sysctl__debug_nvaccel_iop_tl); }\n"
     "    { extern struct sysctl_oid sysctl__debug_nvaccel_iop_tl_raw; sysctl_register_oid(&sysctl__debug_nvaccel_iop_tl_raw); }",
     "register the new sysctl")

# ------------------------------------------------------------------ 5. the xegfx names
edit(H,
     "#define nvaccel_iop_tl xegfxaccel_iop_tl\n#define sysctl__debug_nvaccel_iop_tl sysctl__debug_xegfxaccel_iop_tl",
     "#define nvaccel_iop_tl xegfxaccel_iop_tl\n#define sysctl__debug_nvaccel_iop_tl sysctl__debug_xegfxaccel_iop_tl\n"
     "#define nvaccel_iop_tl_raw xegfxaccel_iop_tl_raw\n#define sysctl__debug_nvaccel_iop_tl_raw sysctl__debug_xegfxaccel_iop_tl_raw",
     "names header: iop_tl_raw")

print("== %d edits ok" % n)
