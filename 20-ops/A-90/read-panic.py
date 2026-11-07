import json, glob, re
p = sorted(glob.glob("/Library/Logs/DiagnosticReports/Kernel-2026-11-03-221637.panic"))[0]
raw = open(p, encoding="utf-8", errors="replace").read()
try:
    d = json.loads(raw)
    body = json.dumps(d, ensure_ascii=False)
    print("keys:", list(d.keys())[:12])
    for k in ("panicString", "macOSPanicString", "panicFlags"):
        if k in d:
            print(k, "=", str(d[k])[:800])
except Exception as e:
    print("not json:", e)
    print(raw[:800])

for m in re.finditer(r'"panicString"\s*:\s*"((?:[^"\\]|\\.)*)"', raw):
    print("PANIC:", m.group(1)[:700].replace("\\n", "\n"))
    break
# which kext appears in the backtrace
for name in ("XeGfxIntelFB", "XeGfxAccel", "XeGfx", "NVMTL", "IOGraphics", "IONDRV"):
    print(name, raw.count(name))
