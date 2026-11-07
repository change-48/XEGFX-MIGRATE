import json, os, sys

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(BASE, "body.md")

HEADER = """### Machine

MacBookPro16,2 (Core Ultra 7 255H laptop, 32 GB, SMBIOS spoof, board Mac-5F9802EFE386AA28)

### CPU, GPU, Wi-Fi / Bluetooth card

Intel Core Ultra 7 255H · Intel Arrow Lake Xe-LPG+ `8086:7D51` · Wi-Fi `8086:0094` · Bluetooth BCM_4350C2

### macOS version and build, OpenCore version

26.5.2 (25F84), OpenCore DBG-108-2026-09-27

### Which project is this about?

WTLIntelGfx / XeGfx 0.0.46 (kernel driver, accelerator and plugin built from `xe-2.6.2`);
`XeGfxIntelFB` 0.8, `XeGfxAGDC` 0.1.1, `XeGfxAccel` 0.1, `XeGfxDisplay` 0.5, `IGPUConsoleRange` 0.4.

### What happened

Bring-up on a **3120x2080 @ 120 Hz** eDP panel. It works: WindowServer composites on the iGPU through
Metal -> NVMTL -> ANV, Liquid Glass renders, `Resolution 3120 x 2080 / UI Looks like 1560 x 1040 @ 120 Hz /
Online: Yes`, `boot_steps = 123`.

Two facts from this machine may be worth having on file, both written up in
https://github.com/watchthelight-labs/WTLIntelGfx/issues/14 :

1. **The panel width breaks the "surface pitch == scanout pitch" assumption.** 3120 x 4 = 12480 and
   12480 / 256 = 48.75, so CoreAnimation pads its IOSurface rows to 12544 while the firmware console stays
   at `PLANE_STRIDE_1_A = 0xc3` (195 x 64 = 12480). On 2560-wide panels 2560 x 4 = 10240 = 256 x 40, so the
   two are equal by construction. Here that means `iop_xe_flips` and `iop_xe_defer_queued` stay 0 and every
   frame is copied by the CPU (about 4 ms at this size).

2. **WindowServer blocks every frame.** A `sample` taken during a window drag shows 666 of 669 samples under
   `CompositorMetal::CompositeLayersToDestination -> MetalContext::Flush -> commitAndWaitUntilSubmitted ->
   waitUntilCompleted -> __psynch_cvwait`. It waits because the pixels must be finished before the copy.

Also worth knowing for other testers: after two boots that ended in a hard freeze, the next boot came up in
"CPU rendering, no HiDPI" and looked like a dead kext. It was `XeGfxIntelFB` own guard
(`boot guard tripped (two starts without WindowServer): declining`); `nvram xeintelfb-starts=%00` clears it
and the next boot is normal again.

One more thing that may be relevant to #14: a panic report from this machine (2026-10-31 22:51, kept below)
has this backtrace, i.e. a fault inside the deferred flip path itself:

```
org.watchthelight.XeGfxAccel : __ZL19nvAccelResidentCallPKcPvS1_S1_S1_ + 0x7e
org.watchthelight.XeGfxAccel : __ZL16nvAccelXeFlipArmP9IOSurfacejjjb + 0x2ae
org.watchthelight.XeGfxAccel : __ZL21nvAccelXeFlipDeferRunPvS_ + 0xc4
panic: Kernel trap, type = 14=page fault, CR2: 0x5dfe22000
```

We are not claiming this is the same failure as the frozen frames reported in #14 (different build, different
day); it is here only because it is the one record we have of `nvAccelXeFlipArm()` actually being entered, and
the fault lands in the `nvAccelResidentCall()` it makes.

No request in this issue, just the machine on file. Plane registers, the framebuffer node, the loaded kexts,
the boot sequence and the accelerator counters are below.

### Logs

"""

SECTIONS = [
    ("Plane registers (userspace, after the desktop came up)", "05-plane.txt"),
    ("Accelerator counters", "06-counters.txt"),
    ("XeGfxIntelFramebuffer node", "04-fb.txt"),
    ("Loaded kexts", "03-kexts.txt"),
    ("Bring-up log (three boots: two failed attempts, then the working one)", "07-bringup.txt"),
    ("Accelerator log, last 5 minutes", "08-accel-log.txt"),
    ("Hardware", "01-hardware.txt"),
    ("macOS and OpenCore version", "02-macos.txt"),
    ("Panic report on this machine (2026-10-31 22:51, the frames that matter)", "09-panics-trim.txt"),
]

FOOTER = """
### Publishing

- [X] I agree that these logs are published in this repository, redacted, under CC BY 4.0
"""

parts = [HEADER]
for label, name in SECTIONS:
    path = os.path.join(BASE, name)
    if not os.path.exists(path):
        continue
    with open(path, encoding="utf-8", errors="replace") as f:
        content = f.read().rstrip("\n")
    parts.append("**%s**\n\n```text\n%s\n```\n\n" % (label, content))
parts.append(FOOTER)
body = "".join(parts)

with open(OUT, "w", encoding="utf-8") as f:
    f.write(body)

print("body.md written:", len(body), "chars")

if "--post" in sys.argv:
    token = os.environ["GH_TOKEN"]
    payload = {
        "title": "Logs: MacBookPro16,2 Core Ultra 7 255H, Xe-LPG+ 7D51, 3120x2080 panel",
        "body": body,
        "labels": ["logs"],
    }
    with open(os.path.join(BASE, "_payload.json"), "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False)
    print("payload written")
