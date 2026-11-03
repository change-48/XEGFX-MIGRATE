#!/usr/bin/env python3
"""A-86 diag: a per-lookup census of the function-constant (fc) cache.

Adds to nvmtl_translate_with_fc_cached (NVMTLLibraryLoad.m):
  * one line per lookup into /private/tmp/nvmtl-fc.log:
        <ns> pid N FC <TAG> <entry> air=<crc> j=<crc> airlen=.. obj=.. spv=.. ms=.. pre=..
    air = crc32 of the AIR text only, j = crc32 of the JSON the key is built from,
    obj = the NSString object that holds the AIR (a new pointer every frame means CA
    hands us a new buffer), spv = bytes produced (0 on a hit), ms = time in translate,
    pre = time spent before translate (JSON + digest + path), i.e. the key's own cost;
  * the first time a given (entry, j) pair is seen, the JSON itself is written out
    (truncated to 300 chars) so two frames can be diffed by hand.
Everything is off when NVMTL_FC_TRACE_OFF is set. Usage: patch_a86_diag.py <plugin_dir>
"""
import io
import os
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "."
if not P.endswith("/"):
    P += "/"
F = P + "NVMTLLibraryLoad.m"

src = io.open(F, encoding="utf-8").read()
EDITS = 0


def edit(old, new, tag):
    global src, EDITS
    n = src.count(old)
    assert n == 1, "FAIL %s: anchor found %d times" % (tag, n)
    src = src.replace(old, new, 1)
    EDITS += 1
    print("ok   %-28s" % tag)


HELPERS = r'''
// <A86> census of the function-constant cache: what the key is made of, on every lookup.
static uint32_t nvmtl_a86_crc(const void *p, size_t n) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
static uint32_t nvmtl_a86_air_hash(NVMTLAirDigest *ad) {
    if (!ad) return 0;
    CC_SHA256_CTX c = ad->_ctx;
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(h, &c);
    return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | (uint32_t)h[3];
}
// The JSON of a key is a few hundred bytes; print it the first time an (entry, digest) pair shows up.
static void nvmtl_a86_json_once(uint32_t j, const char *name, const char *js) {
    static uint32_t seen[256]; static char owner[256][48];
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER; static int n;
    char nm[48]; snprintf(nm, sizeof nm, "%s", name ? name : "?");
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) if (seen[i] == j && !strcmp(owner[i], nm)) { pthread_mutex_unlock(&lk); return; }
    if (n < 256) { seen[n] = j; snprintf(owner[n], sizeof owner[n], "%s", nm); n++; }
    pthread_mutex_unlock(&lk);
    FILE *f = fopen("/private/tmp/nvmtl-fc.log", "a");
    if (!f) return;
    fprintf(f, "pid %d FCJSON %s j=%08x %.300s\n", getpid(), nm, j, js ? js : "");
    fclose(f);
}
static void nvmtl_a86_trace(const char *tag, const char *name, uint32_t air, uint32_t j, NSUInteger airLen,
                            const void *obj, long spv, double ms, double pre, const char *js) {
    if (getenv("NVMTL_FC_TRACE_OFF")) return;
    FILE *f = fopen("/private/tmp/nvmtl-fc.log", "a");
    if (!f) return;
    fprintf(f, "%llu pid %d FC %-7s %-28s air=%08x j=%08x airlen=%lu obj=%p spv=%ld ms=%.2f pre=%.2f\n",
            (unsigned long long)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), getpid(), tag,
            name ? name : "?", air, j, (unsigned long)airLen, obj, spv, ms, pre);
    fclose(f);
    if (j) nvmtl_a86_json_once(j, name, js);
}
'''

edit(
    'NSData *nvmtl_translate_with_fc_cached(NSString *ll, NSString *stage, NSString *name, NSDictionary *constants,\n',
    HELPERS + '\nNSData *nvmtl_translate_with_fc_cached(NSString *ll, NSString *stage, NSString *name, NSDictionary *constants,\n',
    "helpers + entry")

edit(
    '    if (!ll || !constants.count || !nvmtl_xlate_ready())\n'
    '        return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n',
    '    if (!ll || !constants.count || !nvmtl_xlate_ready()) {\n'
    '        nvmtl_a86_trace("NOCONST", name.UTF8String, 0, 0, ll.length, (__bridge const void *)ll, 0, 0, 0, nil);   // <A86>\n'
    '        return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n'
    '    }\n'
    '    const uint64_t a86t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);   // <A86>\n',
    "guard NOCONST")

edit(
    '    if (!json || !airDigest) return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n'
    '    NSString *path = nvmtl_spvcache_path_air(airDigest, json, @"fc1");\n',
    '    if (!json || !airDigest) {\n'
    '        nvmtl_a86_trace("GUARD", name.UTF8String, nvmtl_a86_air_hash(airDigest), json ? nvmtl_a86_crc(json.bytes, json.length) : 0,\n'
    '                        ll.length, (__bridge const void *)ll, 0, 0, 0, nil);   // <A86>\n'
    '        return nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n'
    '    }\n'
    '    NSString *path = nvmtl_spvcache_path_air(airDigest, json, @"fc1");\n'
    '    // <A86>\n'
    '    const uint32_t a86air = nvmtl_a86_air_hash(airDigest);\n'
    '    const uint32_t a86j = nvmtl_a86_crc(json.bytes, json.length);\n'
    '    NSString *a86jstr = [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding];\n'
    '    const char *a86js = a86jstr.UTF8String;\n',
    "guard + key census")

edit(
    '    if (hit) { gFcHit++; gFcMemoHit++; nvmtl_cache_touch(path); nvmtl_fc_note_lookup(); return hit; }',
    '    if (hit) { gFcHit++; gFcMemoHit++; nvmtl_cache_touch(path); nvmtl_fc_note_lookup();\n'
    '        nvmtl_a86_trace("MEMOHIT", name.UTF8String, a86air, a86j, ll.length, (__bridge const void *)ll, hit.length, 0,\n'
    '                        (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - a86t0) / 1e6, a86js);   // <A86>\n'
    '        return hit; }',
    "memo hit")

edit(
    '            gFcHit++; nvmtl_cache_touch(path);   // <wtl>\n',
    '            gFcHit++; nvmtl_cache_touch(path);   // <wtl>\n'
    '            nvmtl_a86_trace("FILEHIT", name.UTF8String, a86air, a86j, ll.length, (__bridge const void *)ll, hit.length, 0,\n'
    '                            (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - a86t0) / 1e6, a86js);   // <A86>\n',
    "file hit")

edit(
    '    gFcMiss++;\n'
    '    NSData *result = nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n',
    '    gFcMiss++;\n'
    '    const double a86pre = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - a86t0) / 1e6;   // <A86>\n'
    '    NSData *result = nvmtl_translate_with_fc(ll, stage, idx, sizes, payloads, n, err);\n'
    '    nvmtl_a86_trace("MISS", name.UTF8String, a86air, a86j, ll.length, (__bridge const void *)ll, (long)result.length,\n'
    '                    (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - a86t0) / 1e6, a86pre, a86js);   // <A86>\n',
    "miss")

io.open(F, "w", encoding="utf-8").write(src)
print("ALL_OK %d edits -> %s" % (EDITS, F))
