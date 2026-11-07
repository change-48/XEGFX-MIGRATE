#!/usr/bin/env python3
"""A-88 P2: 0024's early signal (E2), driven by a switch instead of by the 32-frame criterion.

What E2 does here: the deferred flip worker writes PLANE_SURF_1_A, then *waits for the panel to latch it* (a vblank,
3-9 ms measured as iop_xe_flip_latch_us) before it clears the busy flag and signals the display transaction.  WindowServer
cannot start the next frame until that signal arrives.  E2 moves the signal (and the busy-clear) to the arming write: the
plane will latch at the next vblank whether or not this thread stays to watch, so WindowServer can be given the next frame
one latch earlier.

Why it is worth doing after A-84 and A-87 wrote it off:
  * A-84 rejected it on the arithmetic "25.20 - 4.53 = 20.7 > 16.68", i.e. it only subtracted the latch.
  * A-87 removed 4.08 ms of class-lock wait and the frame did not move -- the saved time reappeared in
    DisplaySurface::AccessComplete (0.88 -> 3.01 ms), which is exactly this latch wait coming back as a wait.
  * A-88 P1 (the ladder) measured the slack: work can grow by up to ~5.6 ms per frame before the frame steps from three
    refreshes to four, so the frame is 1-3 ms away from fitting in two.  The latch is 3-9 ms.

So the switch is the experiment: /private/tmp-free sysctl debug.xegfxaccel_iop_xe_early = 1. Default 0 -- this is a
self-verification, not a product default, and the tearing question is the user's to judge.

Usage: patch_a88_early.py <kexts/NVRM dir>
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
    print("ok   %-34s" % tag)


# ---------------------------------------------------------------- 1. state
edit(C, "static int gIopXeReportBusy = 1;\t// debug.xegfxaccel_iop_xe_report_busy: 0 = always report the transaction complete",
     "static int gIopXeReportBusy = 1;\t// debug.xegfxaccel_iop_xe_report_busy: 0 = always report the transaction complete\n"
     "static int gIopXeEarly = 0;\t\t// A-88 P2 debug.xegfxaccel_iop_xe_early: 1 = complete the transaction at the arming write\n"
     "static SInt64 gCntXeEarlySignals;\t/* A-88: frames whose transaction was completed before the latch was confirmed */\n"
     "static void *gXeEarlyPipe;\t\t/* A-88: the pipe being armed; set by the worker around nvAccelXeFlipArm only */",
     "state: switch + counter")

# ---------------------------------------------------------------- 2. sysctl
edit(C, '''SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_report_busy, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeReportBusy, 0,
    "1 = isTransactionComplete is false while a deferred flip is queued/in flight (default), 0 = always complete");''',
     '''SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_report_busy, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeReportBusy, 0,
    "1 = isTransactionComplete is false while a deferred flip is queued/in flight (default), 0 = always complete");
// A-88 P2 (E2): complete the transaction at the arming write instead of after the panel latch.  A-84 only counted the
// 32-frame criterion for this and it never fires (two surfaces in rotation, A-84 P2); here it is driven by the switch so
// the frame-time effect can be measured directly.  Off by default.
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_xe_early, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopXeEarly, 0,
    "1 = complete the display transaction at the PLANE_SURF write instead of after the panel latch (default 0)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_iop_xe_early_signals, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntXeEarlySignals,
    "A-88: transactions completed at the arming write (early) instead of after the latch");''',
     "sysctl: switch + counter")

# ---------------------------------------------------------------- 3. names
edit(H, "#define nvaccel_iop_xe_early_ready xegfxaccel_iop_xe_early_ready\n"
        "#define sysctl__debug_nvaccel_iop_xe_early_ready sysctl__debug_xegfxaccel_iop_xe_early_ready",
     "#define nvaccel_iop_xe_early_ready xegfxaccel_iop_xe_early_ready\n"
     "#define sysctl__debug_nvaccel_iop_xe_early_ready sysctl__debug_xegfxaccel_iop_xe_early_ready\n"
     "// A-88: the early signal's switch and counter\n"
     "#define nvaccel_iop_xe_early xegfxaccel_iop_xe_early\n"
     "#define sysctl__debug_nvaccel_iop_xe_early sysctl__debug_xegfxaccel_iop_xe_early\n"
     "#define nvaccel_iop_xe_early_signals xegfxaccel_iop_xe_early_signals\n"
     "#define sysctl__debug_nvaccel_iop_xe_early_signals sysctl__debug_xegfxaccel_iop_xe_early_signals",
     "names: aliases")

# ---------------------------------------------------------------- 4. registration
edit(C, "          sysctl__debug_nvaccel_iop_xe_early_run_max, sysctl__debug_nvaccel_iop_xe_early_ready,\n"
        "          sysctl__debug_nvaccel_gt_mhz, sysctl__debug_nvaccel_gt_mhz_last, sysctl__debug_nvaccel_gt_mhz_max,",
     "          sysctl__debug_nvaccel_iop_xe_early_run_max, sysctl__debug_nvaccel_iop_xe_early_ready,\n"
     "          sysctl__debug_nvaccel_iop_xe_early, sysctl__debug_nvaccel_iop_xe_early_signals,\n"
     "          sysctl__debug_nvaccel_gt_mhz, sysctl__debug_nvaccel_gt_mhz_last, sysctl__debug_nvaccel_gt_mhz_max,",
     "register: extern list")

edit(C, "      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_early_ready);",
     "      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_early_ready);\n"
     "      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_early);\n"
     "      sysctl_register_oid(&sysctl__debug_nvaccel_iop_xe_early_signals);",
     "register: oids")

# ---------------------------------------------------------------- 5. arm signature
edit(C, "static bool nvAccelXeFlipArm(IOSurface *s, unsigned w, unsigned h, unsigned pitch, bool waitIdle, NVXeFlipTl *tl = nullptr)",
     "static bool nvAccelXeFlipArm(IOSurface *s, unsigned w, unsigned h, unsigned pitch, bool waitIdle, NVXeFlipTl *tl = nullptr,\n"
     "                             bool *earlySignaled = nullptr)",
     "arm: signature")

# ---------------------------------------------------------------- 6. the early signal itself
edit(C, '''        gDispMmio[NV_PLANE_SURF_A / 4] = (uint32_t)gXeFlipGgtt[slot];	/* the arming write */
        gXePlaneSavedStride = strideReg; gXePlaneSavedSurf = (uint32_t)gXeFlipGgtt[slot];''',
     '''        gDispMmio[NV_PLANE_SURF_A / 4] = (uint32_t)gXeFlipGgtt[slot];	/* the arming write */
        gXePlaneSavedStride = strideReg; gXePlaneSavedSurf = (uint32_t)gXeFlipGgtt[slot];
        // A-88 P2 (E2): the frame is on the plane; the panel latches it at the next vblank whether or not this thread
        // stays to watch.  Completing the transaction here gives WindowServer the next frame one latch earlier -- the
        // A-88 P1 ladder leaves 1-3 ms of slack and the latch costs 3-9 ms (debug.xegfxaccel_iop_xe_flip_latch_us).
        // Attempt 0 only: a re-arm after a repair must not complete the same transaction twice.
        if (attempt == 0 && gIopXeEarly && gXeEarlyPipe && earlySignaled) {
            *earlySignaled = true;
            IOLockLock(gXeFlipLock);
            if (gXeFlipQHead == gXeFlipQTail) {	// nothing else queued: this frame is the whole transaction
                __atomic_store_n(&gXeFlipBusy, 0, __ATOMIC_RELEASE);
                gXeFlipBusySince = 0;
                IOLockWakeup(gXeFlipLock, (event_t)&gXeFlipBusy, false);
            }
            IOLockUnlock(gXeFlipLock);
            OSAddAtomic64(1, &gCntXeEarlySignals);
            _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(gXeEarlyPipe, nullptr);
        }''',
     "arm: signal at the write")

# ---------------------------------------------------------------- 7. worker: hand the pipe over, do not signal twice
edit(C, '''        NVXeFlipTl ftl = {}; ftl.enter = mach_absolute_time();
        const bool ok = nvAccelXeFlipArm(p.s, p.w, p.h, p.pitch, true, &ftl);''',
     '''        NVXeFlipTl ftl = {}; ftl.enter = mach_absolute_time();
        bool earlySignaled = false;
        gXeEarlyPipe = p.pipe;		// A-88 P2: valid for this arm only (the queue holds the reference)
        const bool ok = nvAccelXeFlipArm(p.s, p.w, p.h, p.pitch, true, &ftl, &earlySignaled);
        gXeEarlyPipe = nullptr;''',
     "worker: pass the pipe")

edit(C, '''        OSAddAtomic64(1, &gCntXeDeferSignals);
        _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(p.pipe, nullptr);
        ((OSObject *)p.pipe)->release();''',
     '''        if (!earlySignaled) {	// A-88 P2: an early-signalled frame was already completed; a second signal would look
            OSAddAtomic64(1, &gCntXeDeferSignals);	// like a second completion to WindowServer
            _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(p.pipe, nullptr);
        }
        ((OSObject *)p.pipe)->release();''',
     "worker: single signal")

print("\n%d edits applied to %s" % (n, C))
