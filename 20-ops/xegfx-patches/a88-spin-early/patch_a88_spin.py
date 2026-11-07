#!/usr/bin/env python3
"""A-88 P1: inject a known amount of work into ONE path and read the frame time back.

Why: A-87 took 4.08 ms of class-lock wait out of the driver and the frame time did not move (25.30 -> 25.00), and
`defer=0` cut main-thread busy to 9.04 ms yet made the frame *slower* (32.17). Busy+wait is 15.39 ms, under the 16.67 ms
of two refreshes, and the frame still lands on three. So the question is no longer "how much work is left" but "where is
the step, and how far from it are we". That is measured by pushing work IN on one path at a time and watching p50:

  * commit path, on the committing (WindowServer main) thread -- the same place CA's own work lands;
  * the submit-ahead worker -- a control: work that does not sit on the main thread should not move p50 at all if the
    main thread really is off the critical path.

Switch, re-read every 200 ms (no restart, no rebuild between sweep points):

  /private/tmp/nvmtl-spin    "<microseconds>[m|w]"   absent / 0 / negative = off (default)
                             e.g. `3000m` (3 ms per commit, main thread), `3000w` (3 ms per part, ahead worker)

Counters: every spin is accumulated; a line is printed at powers of two (nvlog dedupes by format string, A-86 §6.1, so
never count log lines -- the printed call count is the counter). Calls per frame is derived from the unified log
timestamps plus the flip counter, which is how "us inserted per frame" is computed for the ladder table.

Usage: patch_a88_spin.py <plugin_dir>
"""
import io
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "."
if not P.endswith("/"):
    P += "/"
E = P + "NVMTLEventSync.m"
n = 0


def edit(path, old, new, tag):
    global n
    src = io.open(path, encoding="utf-8", newline="").read()
    c = src.count(old)
    assert c == 1, "FAIL %s: anchor found %d times" % (tag, c)
    io.open(path, "w", encoding="utf-8", newline="").write(src.replace(old, new, 1))
    n += 1
    print("ok   %-32s" % tag)


# ------------------------------------------------------------------ 1. the spin itself
HELPER = r'''#include <mach/mach_time.h>
// <A88> G7-A88 P1. Put a known amount of work into one path and read the frame time back: "how far from the next
// step" has to be measured, because three rounds of arithmetic (A-84 20.7 ms, A-86 15.49 ms, A-87 15.39 ms) all said the
// frame should already fit in two refreshes, and it does not.
static _Atomic unsigned gA88spin_us;
static _Atomic int gA88spin_mode;            // 0 = commit path (main thread), 1 = submit-ahead worker
static _Atomic unsigned long long gA88spin_calls, gA88spin_us_done;
static void nvmtl_a88_spin_sync(void) {
    static _Atomic uint64_t last;
    static _Atomic int seen_us = -1, seen_mode = -1;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    const uint64_t was = atomic_load(&last);
    if (was && now - was < 200000000ull) return;
    atomic_store(&last, now);
    unsigned us = 0; int mode = 0;
    FILE *fd = fopen("/private/tmp/nvmtl-spin", "r");
    if (fd) {
        char line[64];
        if (fgets(line, sizeof line, fd)) {
            char *end = NULL;
            const long v = strtol(line, &end, 10);
            if (v > 0) us = (unsigned)v;
            if (us > 20000) us = 20000;      // beyond 20 ms per call the sweep is meaningless anyway
            if (end && (*end == 'w' || *end == 'W')) mode = 1;
        }
        fclose(fd);
    }
    if ((int)us != atomic_load(&seen_us) || mode != atomic_load(&seen_mode)) {
        atomic_store(&seen_us, (int)us);
        atomic_store(&seen_mode, mode);
        // the cumulative counter travels with every config change: two markers bracket a run, and their difference over
        // the flip counter of the same window is "how many injected work units a frame carries"
        nvlog("A88 spin: %u us on the %s -- %llu calls, %llu us inserted since load",
              us, mode ? "submit-ahead worker" : "commit path (main thread)",
              (unsigned long long)atomic_load(&gA88spin_calls),
              (unsigned long long)atomic_load(&gA88spin_us_done));
    }
    atomic_store(&gA88spin_us, us);
    atomic_store(&gA88spin_mode, mode);
}
static void nvmtl_a88_spin(unsigned us) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t want = (uint64_t)us * 1000ull * (uint64_t)tb.denom / (uint64_t)tb.numer;
    const uint64_t t0 = mach_absolute_time();
    do { __asm__ __volatile__(""); } while (mach_absolute_time() - t0 < want);
    const unsigned long long k = atomic_fetch_add(&gA88spin_calls, 1) + 1;
    atomic_fetch_add(&gA88spin_us_done, (unsigned long long)us);
    if (!(k & (k - 1)))
        nvlog("A88 spin: %llu calls, %llu us inserted in total, %u us per call",
              k, (unsigned long long)atomic_load(&gA88spin_us_done), us);
}
static inline void nvmtl_a88_spin_here(int mode) {
    const unsigned us = atomic_load(&gA88spin_us);
    if (!us || atomic_load(&gA88spin_mode) != mode) return;
    nvmtl_a88_spin(us);
}
static NSMutableArray *nvmtl_quarantined_parts;'''

edit(E, "static NSMutableArray *nvmtl_quarantined_parts;", HELPER, "spin helper")

# ------------------------------------------------------------------ 2. commit path (main thread)
OLD_COMMIT = r'''        [self nvmtlSealPartWithSignal:nil value:0 restart:NO];
    }
    // the commit's kernel marks can wait for the accelerator lock, so they go in outside the class lock (see'''
NEW_COMMIT = r'''        [self nvmtlSealPartWithSignal:nil value:0 restart:NO];
    }
    nvmtl_a88_spin_sync();                 // <A88> outside the class lock: same thread as CA's own commit work
    nvmtl_a88_spin_here(0);
    // the commit's kernel marks can wait for the accelerator lock, so they go in outside the class lock (see'''
edit(E, OLD_COMMIT, NEW_COMMIT, "spin site: commit path")

# ------------------------------------------------------------------ 3. ahead worker (control)
OLD_WORKER = r'''- (void)nvmtlA87Run:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f {
    for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);'''
NEW_WORKER = r'''- (void)nvmtlA87Run:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f {
    nvmtl_a88_spin_sync();                 // <A88> control path: work that is NOT on the main thread
    nvmtl_a88_spin_here(1);
    for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);'''
edit(E, OLD_WORKER, NEW_WORKER, "spin site: ahead worker")

print("\n%d edits applied to %s" % (n, E))
