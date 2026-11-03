#!/usr/bin/env python3
"""A-86 P2 probe: runtime knobs for the submit path, so one reboot A/Bs every configuration.

  /private/tmp/nvmtl-a86   one directive per line:
        nogsub     the submit-ahead pass does not run; the execution worker submits (outside the class lock)
        cap=N      parts allowed in flight (1..32)

The file is re-read at most every 200 ms, so flipping a line changes the next frame; nothing needs a restart.
Usage: patch_a86_knobs.py <plugin_dir>
"""
import io
import sys

P = sys.argv[1] if len(sys.argv) > 1 else "."
if not P.endswith("/"):
    P += "/"
F = P + "NVMTLEventSync.m"
src = io.open(F, encoding="utf-8").read()
n = 0


def edit(old, new, tag):
    global src, n
    c = src.count(old)
    assert c == 1, "FAIL %s: anchor found %d times" % (tag, c)
    src = src.replace(old, new, 1)
    n += 1
    print("ok   %-24s" % tag)


KNOBS = r'''// <A86> runtime knobs. One reboot, many configurations: /private/tmp/nvmtl-a86 is re-read every 200 ms.
//   nogsub  the submit-ahead pass does not run (the execution worker submits, outside the class lock)
//   cap=N   parts allowed in flight (1..32)
static int nvmtl_a86_nogsub, nvmtl_a86_cap;
static void nvmtl_a86_knobs(void) {
    static _Atomic uint64_t last;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (now - atomic_load(&last) < 200000000ull) return;
    atomic_store(&last, now);
    int ng = 0, cp = 0;
    FILE *f = fopen("/private/tmp/nvmtl-a86", "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            int v = 0;
            if (!strncmp(line, "nogsub", 6)) ng = 1;
            else if (sscanf(line, "cap=%d", &v) == 1 && v >= 1 && v <= 32) cp = v;
        }
        fclose(f);
    }
    if (ng != nvmtl_a86_nogsub || cp != nvmtl_a86_cap)
        nvlog("A86 knobs: submit-ahead %s, cap %d", ng ? "OFF (execution worker submits)" : "on", cp ? cp : -1);
    nvmtl_a86_nogsub = ng; nvmtl_a86_cap = cp;
}
'''

edit("static unsigned nvmtl_gsub_cap(void) {\n    static int cap = -1;\n",
     KNOBS + "static unsigned nvmtl_gsub_cap(void) {\n    static int cap = -1;\n",
     "knob helper")

edit('''              nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) ? "ON" : "off", cap, e ? e : "unset"); }
    return (unsigned)cap;
}''',
     '''              nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) ? "ON" : "off", cap, e ? e : "unset"); }
    nvmtl_a86_knobs();   // <A86>
    return (unsigned)(nvmtl_a86_cap ? nvmtl_a86_cap : cap);
}''',
     "cap honours knob")

edit('static BOOL nvmtl_gsub_ahead_on(void) { return nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) && nvmtl_gsub_cap(); }',
     'static BOOL nvmtl_gsub_ahead_on(void) { nvmtl_a86_knobs(); if (nvmtl_a86_nogsub) return NO; return nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) && nvmtl_gsub_cap(); }   // <A86>',
     "ahead honours nogsub")

io.open(F, "w", encoding="utf-8").write(src)
print("ALL_OK %d edits -> %s" % (n, F))
