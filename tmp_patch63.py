import sys
p = sys.argv[1]
s = open(p).read()
old = """    pthread_mutex_lock(&lk);
    NSArray *hit = cache[key];
    pthread_mutex_unlock(&lk);
    if (hit) {"""
new = """    pthread_mutex_lock(&lk);
    NSArray *hit = cache[key];
    pthread_mutex_unlock(&lk);
    // A-63: measure before optimising.  If this reports a high hit rate, the naga passes in the profile are
    // something else's work; if it reports misses, CoreAnimation really is handing us new shader bytes.
    {
        static _Atomic unsigned long long hits, misses;
        const unsigned long long k = hit ? atomic_fetch_add(&hits, 1) + 1 : atomic_fetch_add(&misses, 1) + 1;
        const unsigned long long tot = atomic_load(&hits) + atomic_load(&misses);
        if (tot && (tot % 64) == 0)
            nvlog("xlate cache: %llu hit, %llu miss (%.1f%% hit)", atomic_load(&hits), atomic_load(&misses),
                  100.0 * (double)atomic_load(&hits) / (double)tot);
        (void)k;
    }
    if (hit) {"""
if old not in s: raise SystemExit("ANCHOR MISSING")
open(p,"w").write(s.replace(old,new,1)); print("patched")
