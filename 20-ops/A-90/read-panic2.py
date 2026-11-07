import json, glob, re
for p in sorted(glob.glob("/Library/Logs/DiagnosticReports/Kernel-2026-11-03-22*.panic")) + \
         sorted(glob.glob("/Library/Logs/DiagnosticReports/Kernel-2026-11-03-2026*.panic")):
    raw = open(p, encoding="utf-8", errors="replace").read()
    m = re.search(r'"macOSPanicString":"((?:[^"\\]|\\.)*)"', raw)
    print("=" * 20, p.split("/")[-1])
    if not m:
        print("no panic string"); continue
    s = m.group(1).encode().decode("unicode_escape", "replace")
    lines = s.split("\n")
    print(lines[0][:300])
    kept = [l for l in lines if "XeGfx" in l or "IONDRV" in l or "IOGraphics" in l or "NVMTL" in l or "Cursor" in l]
    print("\n".join(kept[:14]))
