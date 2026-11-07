#!/usr/bin/env python3
"""A-87 P1/P2: the submit-ahead pass claims under the class lock, submits outside it.

Why not "a second lock for the ahead pass" (G7-A87 prompt §3.1): the pass reads two containers that commit, dealloc and
the execution worker mutate under the class lock -- nvmtl_pending_buffers (commit appends, pump removes) and
cb->_queue->_reservations (reserve appends, release removes). Handing the pass another lock turns those into
unsynchronised NSMutableArray mutation (P0 table in the A-87 report). So the split is by phase, not by lock object:
claim (in the lock) -> nvmtl_buffer_sync_in + nvmtl_vk_submit_begin (out of it) -> land (in the lock).

Runtime switch, re-read every 200 ms, default OFF (= A-80 behaviour, everything under the class lock):

  /private/tmp/nvmtl-a87   line `oollock`  -> claim in, submit out (the new path)

Counters: every claim/land/fail is printed at powers of two, so the A/B can check claim == land + fail
(nvlog dedupes by format string, A-86 §6.1 -- never count log lines).

Usage: patch_a87_lock.py <plugin_dir>
"""
import io
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "."
if not P.endswith("/"):
    P += "/"
E = P + "NVMTLEventSync.m"
H = P + "NVMTLObjects.h"
n = 0


def edit(path, old, new, tag):
    global n
    src = io.open(path, encoding="utf-8", newline="").read()
    c = src.count(old)
    assert c == 1, "FAIL %s: anchor found %d times" % (tag, c)
    io.open(path, "w", encoding="utf-8", newline="").write(src.replace(old, new, 1))
    n += 1
    print("ok   %-30s" % tag)


# ---------------------------------------------------------------- 1. state
GLOBALS = r'''static _Atomic unsigned long long gsub_ahead, gsub_peak;
// <A87> claim inside the class lock, submit outside it. gA87claim == gA87land + gA87fail + (parts still running).
static _Atomic unsigned long long gA87claim, gA87land, gA87fail;
static _Atomic int nvmtl_a87_passing;   // one ahead pass at a time: a pass drops the class lock while it submits
static int nvmtl_a87_oollock(void) {
    static _Atomic int on, seen = -1;
    static _Atomic uint64_t last;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    const uint64_t was = atomic_load(&last);
    if (!was || now - was >= 200000000ull) {
        atomic_store(&last, now);
        int v = 0;
        FILE *fd = fopen("/private/tmp/nvmtl-a87", "r");
        if (fd) { char line[160]; while (fgets(line, sizeof line, fd)) if (!strncmp(line, "oollock", 7)) v = 1; fclose(fd); }
        if (v != atomic_load(&seen)) {
            atomic_store(&seen, v);
            nvlog("A87 switch: submit-ahead %s", v ? "claims in the class lock, submits OUTSIDE it"
                                                  : "all under the class lock (A-80 behaviour)");
        }
        atomic_store(&on, v);
    }
    return atomic_load(&on);
}
'''

edit(E, "static _Atomic unsigned long long gsub_ahead, gsub_peak;\n", GLOBALS, "state + switch")

# ------------------------------------------------------- 2. part gets aheadBusy
edit(E, "    BOOL claimed;\n",
     "    BOOL claimed;\n    BOOL aheadBusy;   // <A87> claimed by the ahead pass, being submitted outside the class lock\n",
     "part->aheadBusy")

# ------------------------------------------------------- 3. command buffer field
edit(H,
     "  @public nvk_cmdbuf _c; NVMTLCommandQueue *_queue; BOOL _committed, _commitMarking, _partsRetiring; BOOL _capacityHeld; MTLCommandBufferStatus _cbstatus;   // <wtl>",
     "  @public nvk_cmdbuf _c; NVMTLCommandQueue *_queue; BOOL _committed, _commitMarking, _partsRetiring; BOOL _capacityHeld; MTLCommandBufferStatus _cbstatus;   // <wtl>\n"
     "  int _aheadBusy;   // <A87> parts of this buffer the ahead pass holds outside the class lock (atomic)",
     "cb->_aheadBusy")

# ------------------------------------------------------- 4. method declarations
DECL = r'''@end
// <A87> the ahead pass, split by phase: claim and land under the class lock, the 0.45 ms submit between them outside it
@interface NVMTLCommandBuffer (A87Ahead)
- (BOOL)nvmtlA87Claim:(NVMTLSubmissionPart **)outPart inflight:(nvmtl_inflight **)outInflight;
- (void)nvmtlA87Run:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f;
- (void)nvmtlA87Land:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f failed:(int)r;
@end
'''
edit(E, "    BOOL copyInputsPrepared, hasHostCopies;\n}\n@end\n",
     "    BOOL copyInputsPrepared, hasHostCopies;\n}\n" + DECL, "method declarations")

# --------------------------------------------- 5. nvmtlSubmitAhead -> claim / run / land
OLD_AHEAD = r'''- (BOOL)nvmtlSubmitAhead {
    // a commit still putting its kernel marks in (nvmtlSubmitWithEvents) must not reach the GPU before them
    if (_executionDone || _partsRetiring || _cbError || !_committed || _commitMarking) return NO;   // <wtl>
    if (_nextSubmit < _nextPart) _nextSubmit = _nextPart;
    while (_nextSubmit < _parts.count) {
        if (atomic_load(&gsub_inflight) >= nvmtl_gsub_cap()) return NO;
        NVMTLSubmissionPart *part = _parts[_nextSubmit];
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) return NO;
        if (part->claimed) return NO;
        if (part->waitEvent) {
            if (!nvmtl_evfold_on() || nvmtl_evfold_promised(part->waitEvent) < part->value) return NO;
            part->waitFolded = YES; _nextSubmit++; nvmtl_evfold_census(&gEvFold, "event waits ordered by the channel (no CPU hold)");
            continue;
        }
        if (!part->commands.cb) return NO;
        if (!part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && nvmtl_skip_empty_on()) {
            if (!nvmtl_evfold_on() || _nextSubmit + 1 >= _parts.count) return NO;
            [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            nvmtl_vk_cmd_abandon(&part->commands);
            part->aheadEmpty = YES; _nextSubmit++;
            nvmtl_evfold_promise_part(part);
            nvmtl_evfold_census(&gEvFoldEmpty, "empty split parts retired by the ahead pass");
            continue;
        }
        nvmtl_prepare_submission_copies(part);
        {
            static int hold = -1;
            if (hold < 0) { const char *e = getenv("NVMTL_GSUB_HOSTPTR_LIFT"); hold = (e && *e && *e != '0') ? 0 : 1; }
            if (hold && part->hasHostCopies) return NO;
        }
        nvmtl_inflight *f = nvmtl_vk_inflight_new();
        if (!f) return NO;
        for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);
        [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        const int r = nvmtl_vk_submit_begin(&part->commands, f);
        _nextSubmit++;
        if (r) {
            nvmtl_vk_inflight_free(f);
            part->aheadFailed = r;
            return NO;
        }
        part->inflight = f;
        nvmtl_evfold_promise_part(part);
        const unsigned long long n = atomic_fetch_add(&gsub_inflight, 1) + 1;
        if (n > atomic_load(&gsub_peak)) atomic_store(&gsub_peak, n);
        atomic_fetch_add(&gsub_ahead, 1);
        nvmtl_gsub_census();
    }
    return YES;
}'''

NEW_AHEAD = r'''// <A87> Claim ONE part. Called with the class lock held; the part comes back with aheadBusy set, which is the promise
// the execution worker reads (nvmtlAdvancePart waits it out) -- otherwise ahead and the worker both submit the same part.
- (BOOL)nvmtlA87Claim:(NVMTLSubmissionPart **)outPart inflight:(nvmtl_inflight **)outInflight {
    *outPart = nil; *outInflight = NULL;
    // a commit still putting its kernel marks in (nvmtlSubmitWithEvents) must not reach the GPU before them
    if (_executionDone || _partsRetiring || _cbError || !_committed || _commitMarking) return NO;   // <wtl>
    if (_nextSubmit < _nextPart) _nextSubmit = _nextPart;
    while (_nextSubmit < _parts.count) {
        if (atomic_load(&gsub_inflight) >= nvmtl_gsub_cap()) return NO;
        NVMTLSubmissionPart *part = _parts[_nextSubmit];
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) return NO;
        if (part->claimed || part->aheadBusy) return NO;
        if (part->waitEvent) {
            if (!nvmtl_evfold_on() || nvmtl_evfold_promised(part->waitEvent) < part->value) return NO;
            part->waitFolded = YES; _nextSubmit++; nvmtl_evfold_census(&gEvFold, "event waits ordered by the channel (no CPU hold)");
            continue;
        }
        if (!part->commands.cb) return NO;
        if (!part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && nvmtl_skip_empty_on()) {
            if (!nvmtl_evfold_on() || _nextSubmit + 1 >= _parts.count) return NO;
            [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            nvmtl_vk_cmd_abandon(&part->commands);
            part->aheadEmpty = YES; _nextSubmit++;
            nvmtl_evfold_promise_part(part);
            nvmtl_evfold_census(&gEvFoldEmpty, "empty split parts retired by the ahead pass");
            continue;
        }
        nvmtl_prepare_submission_copies(part);
        {
            static int hold = -1;
            if (hold < 0) { const char *e = getenv("NVMTL_GSUB_HOSTPTR_LIFT"); hold = (e && *e && *e != '0') ? 0 : 1; }
            if (hold && part->hasHostCopies) return NO;
        }
        nvmtl_inflight *f = nvmtl_vk_inflight_new();
        if (!f) return NO;
        _nextSubmit++;                       // claimed: from here only this pass may touch the part
        part->aheadBusy = YES;
        __atomic_add_fetch(&_aheadBusy, 1, __ATOMIC_ACQ_REL);
        const unsigned long long k = atomic_fetch_add(&gA87claim, 1) + 1;
        if (!(k & (k - 1)))
            nvlog("A87 out-of-lock: %llu parts claimed, %llu landed, %llu failed",
                  k, (unsigned long long)atomic_load(&gA87land), (unsigned long long)atomic_load(&gA87fail));
        *outPart = part; *outInflight = f;
        return YES;
    }
    return NO;
}
// <A87> The work that used to happen inside the class lock: host copies in, then vkQueueSubmit (~451 us a part).
// The part is held by _parts and by this stack frame; nvmtlFinishExecution waits _aheadBusy out before it empties _parts.
- (void)nvmtlA87Run:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f {
    for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);
    [self nvmtlMarkScheduled];
    if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    const int r = nvmtl_vk_submit_begin(&part->commands, f);
    @synchronized ([NVMTLCommandBuffer class]) { [self nvmtlA87Land:part inflight:f failed:r]; }
}
- (void)nvmtlA87Land:(NVMTLSubmissionPart *)part inflight:(nvmtl_inflight *)f failed:(int)r {
    part->aheadBusy = NO;
    __atomic_sub_fetch(&_aheadBusy, 1, __ATOMIC_ACQ_REL);
    if (r) {
        nvmtl_vk_inflight_free(f);
        part->aheadFailed = r;
        atomic_fetch_add(&gA87fail, 1);
    } else {
        part->inflight = f;
        nvmtl_evfold_promise_part(part);
        const unsigned long long n = atomic_fetch_add(&gsub_inflight, 1) + 1;
        if (n > atomic_load(&gsub_peak)) atomic_store(&gsub_peak, n);
        atomic_fetch_add(&gsub_ahead, 1);
        const unsigned long long k = atomic_fetch_add(&gA87land, 1) + 1;
        if (!(k & (k - 1)))
            nvlog("A87 out-of-lock: %llu parts landed, %llu claimed, %llu failed, %u in flight",
                  k, (unsigned long long)atomic_load(&gA87claim), (unsigned long long)atomic_load(&gA87fail),
                  (unsigned)atomic_load(&gsub_inflight));
        nvmtl_gsub_census();
    }
    // the worker may have given up while this part was out of the lock; put the pipeline back on its feet
    if (!nvmtl_pumping && _nextPart < _parts.count) nvmtl_pump_buffers();
}'''
edit(E, OLD_AHEAD, NEW_AHEAD, "claim / run / land")

# ------------------------------------------------------------ 6. the pass itself
OLD_PASS = r'''static void nvmtl_gsub_ahead_pass1(void);
static void nvmtl_gsub_ahead_pass(void) {
    for (int round = 0; round < 64; round++) {
        const unsigned long long before = atomic_load(&gEvFold);
        nvmtl_gsub_ahead_pass1();
        if (atomic_load(&gEvFold) == before) break;
    }
}
static void nvmtl_gsub_ahead_pass1(void) {
    NSMutableSet *queues = [NSMutableSet new];
    for (NVMTLCommandBuffer *cb in nvmtl_pending_buffers) {
        id q = cb->_queue;
        if (!q || [queues containsObject:q]) continue;
        [queues addObject:q];
        for (NVMTLQueueReservation *r in [cb->_queue->_reservations copy]) {
            NVMTLCommandBuffer *b = r->buffer;
            if (!b) continue;
            if (![b nvmtlSubmitAhead]) break;
        }
    }
}'''

NEW_PASS = r'''static void nvmtl_gsub_ahead_pass1(void);
// <A87> The pass no longer runs inside the caller's @synchronized: it takes the class lock per claim and per land, and
// the submit in between happens without it. One pass at a time (CAS), because two passes would drop the lock into each
// other's window; a second caller simply returns -- the running pass picks up whatever it left behind.
static void nvmtl_gsub_ahead_pass(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&nvmtl_a87_passing, &expected, 1)) return;
    for (int round = 0; round < 64; round++) {
        const unsigned long long before = atomic_load(&gEvFold) + atomic_load(&gA87claim);
        nvmtl_gsub_ahead_pass1();
        if (atomic_load(&gEvFold) + atomic_load(&gA87claim) == before) break;
    }
    atomic_store(&nvmtl_a87_passing, 0);
}
static void nvmtl_gsub_ahead_pass1(void) {
    NSMutableSet *queues = [NSMutableSet new];
    NSArray *pending; @synchronized ([NVMTLCommandBuffer class]) { pending = [nvmtl_pending_buffers copy]; }
    for (NVMTLCommandBuffer *cb in pending) {
        id q = cb->_queue;
        if (!q || [queues containsObject:q]) continue;
        [queues addObject:q];
        NSArray *reservations; @synchronized ([NVMTLCommandBuffer class]) { reservations = [cb->_queue->_reservations copy]; }
        for (NVMTLQueueReservation *r in reservations) {
            NVMTLCommandBuffer *b = r->buffer;
            if (!b) continue;
            for (;;) {   // every part of b, in order, as before
                const BOOL outside = nvmtl_a87_oollock();
                NVMTLSubmissionPart *part = nil; nvmtl_inflight *f = NULL;
                BOOL claimed = NO;
                @synchronized ([NVMTLCommandBuffer class]) {
                    if (!nvmtl_pumping) return;
                    claimed = [b nvmtlA87Claim:&part inflight:&f];
                    if (claimed && !outside) [b nvmtlA87Run:part inflight:f];   // switch off: the A-80 path, lock held
                }
                if (!claimed) break;
                if (outside) [b nvmtlA87Run:part inflight:f];
            }
        }
    }
}'''
edit(E, OLD_PASS, NEW_PASS, "ahead pass")

# ------------------------------------------------------------ 7. call site: ahead kick
edit(E,
     "            @autoreleasepool { @synchronized ([NVMTLCommandBuffer class]) { if (nvmtl_pumping) nvmtl_gsub_ahead_pass(); } }",
     "            @autoreleasepool { nvmtl_gsub_ahead_pass(); }   // <A87> the pass takes the class lock itself, per part",
     "call site: ahead kick")

# ------------------------------------------------------------ 8. call site: worker
edit(E,
     """                NSArray *batch; uint64_t generation;
                @synchronized ([NVMTLCommandBuffer class]) {
                    if (nvmtl_gsub_ahead_on()) nvmtl_gsub_ahead_pass();
                    batch = [nvmtl_pending_buffers copy]; generation = nvmtl_pump_generation;
                }""",
     """                NSArray *batch; uint64_t generation;
                if (nvmtl_gsub_ahead_on()) nvmtl_gsub_ahead_pass();   // <A87> not under the class lock any more
                @synchronized ([NVMTLCommandBuffer class]) {
                    batch = [nvmtl_pending_buffers copy]; generation = nvmtl_pump_generation;
                }""",
     "call site: worker")

# ------------------------------------------------------------ 9. AdvancePart
edit(E,
     """    NVMTLSubmissionPart *part = _parts[_nextPart];
    BOOL ahead;   // claimed under the class lock: the commit-time pass stops at a claimed part
    @synchronized ([NVMTLCommandBuffer class]) { ahead = part->inflight || part->aheadFailed; part->claimed = YES; }""",
     """    NVMTLSubmissionPart *part = _parts[_nextPart];
    // <A87> the ahead pass may be submitting exactly this part outside the class lock: it is 0.45 ms, wait it out rather
    // than reading inflight too early or reporting "no progress" while the part is still on its way to the GPU
    if (__atomic_load_n(&part->aheadBusy, __ATOMIC_ACQUIRE)) {
        for (int i = 0; i < 2000 && __atomic_load_n(&part->aheadBusy, __ATOMIC_ACQUIRE); i++) usleep(10);
        if (__atomic_load_n(&part->aheadBusy, __ATOMIC_ACQUIRE)) return NO;
    }
    BOOL ahead;   // claimed under the class lock: the commit-time pass stops at a claimed part
    @synchronized ([NVMTLCommandBuffer class]) {
        if (part->aheadBusy) return NO;   // <A87> claimed by the ahead pass between the wait above and this lock
        ahead = part->inflight || part->aheadFailed; part->claimed = YES;
    }""",
     "AdvancePart waits aheadBusy")

# ------------------------------------------------------------ 10. FinishExecution
edit(E,
     """    @synchronized ([NVMTLCommandBuffer class]) { _partsRetiring = YES; }
    // </wtl>""",
     """    @synchronized ([NVMTLCommandBuffer class]) { _partsRetiring = YES; }
    // </wtl>
    // <A87> a part the ahead pass claimed is being submitted outside the class lock. _partsRetiring stops new claims;
    // this waits the one in flight out (bounded, it is one submit) before _parts is emptied below, or that part's
    // in-flight slot and fence would never come back. No lock is held here: the ahead thread needs it to land.
    for (int i = 0; i < 4000 && __atomic_load_n(&_aheadBusy, __ATOMIC_ACQUIRE); i++) usleep(25);""",
     "FinishExecution waits aheadBusy")

print("ALL_OK %d edits" % n)
