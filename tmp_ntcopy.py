import sys
p = sys.argv[1]
s = open(p).read()

if "nontemporal_store" in s:
    raise SystemExit("ALREADY PATCHED")

old = """    const uint64_t copyStart = mach_absolute_time();
    if (gIopMemcpy) {
        if (q.pitch == pitch) {
            memcpy((void *)q.kva, (const void *)src, (size_t)h * (size_t)pitch);
        } else {"""
if old not in s:
    raise SystemExit("COPY ANCHOR MISSING")

new = """    const uint64_t copyStart = mach_absolute_time();
    if (gIopMemcpy) {
        // A-69: this writes about 26 MB into the framebuffer aperture, which is write-combined memory, and a
        // plain memcpy reaches only ~3.1 GB/s there -- 8.3 ms per frame, a whole 120 Hz refresh, and the window
        // in which the scan can catch the write half done (the tear).  Non-temporal stores (movnti: no
        // read-for-ownership, no cache pollution) are the right instruction for a destination that is never read
        // back, and they use integer registers only, so they are safe inside a kext.
        if (q.pitch == pitch) {
            const size_t frameBytes = (size_t)h * (size_t)pitch;
            if ((((uintptr_t)q.kva | (uintptr_t)src | (uintptr_t)pitch) & 7u) == 0) {
                const unsigned long long *sp = (const unsigned long long *)src;
                unsigned long long *dp = (unsigned long long *)q.kva;
                const size_t n = frameBytes >> 3;
                for (size_t i = 0; i < n; i++) __builtin_nontemporal_store(sp[i], &dp[i]);
                __builtin_ia32_sfence();
            } else {
                memcpy((void *)q.kva, (const void *)src, frameBytes);
            }
        } else {"""
s = s.replace(old, new, 1)

open(p, "w").write(s)
print("patched")
