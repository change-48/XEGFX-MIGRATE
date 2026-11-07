import sys
p = sys.argv[1]
s = open(p).read()

old = "#define NV_XE_FLIP_MAX_PAGES 4224"
new = ("// 4224 pages is 16.5 MB -- the 2560x1600 panel this was written for.  A 3120x2080 surface at the\n"
       "// compositor's 12544 row pitch is 24.9 MB, i.e. 6370 pages, so the mapping below would refuse it\n"
       "// even with the pitch gate open.  8192 pages is 32 MB.\n"
       "#define NV_XE_FLIP_MAX_PAGES 8192")
if old not in s:
    raise SystemExit("MAX_PAGES ANCHOR NOT FOUND")
s = s.replace(old, new, 1)

old2 = "    if (need > NV_XE_FLIP_MAX_PAGES) { md->release(); gScanoutState = 3; return false; }"
new2 = ("    if (need > NV_XE_FLIP_MAX_PAGES) {\n"
        "        ALOG(\"scanout: %llu B needs %u pages, cap is %u -- refusing\", bytes, need, (unsigned)NV_XE_FLIP_MAX_PAGES);\n"
        "        md->release(); gScanoutState = 3; return false; }\n")
if old2 not in s:
    raise SystemExit("NEED-CHECK ANCHOR NOT FOUND")
s = s.replace(old2, new2, 1)

open(p, "w").write(s)
print("patched ok")
