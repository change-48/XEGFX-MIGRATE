## Environment

- Arrow Lake-HX, iGPU `8086:7D51` (Xe-LPG+), eDP panel **3120×2080 @ 120 Hz**, macOS 26.5.2, OpenCore.
- Kernel driver + accelerator from `xe-2.6.2`; plugin at the same tip; `XeGfxIntelFB` owns the panel
  (head 4), `XeGfxAGDC` serves the display pipe, `XeGfxAccel` in `IOPropertyMatch` on `nmxgpu-claimed`.
- Desktop works: WindowServer composites on the iGPU through Metal → NVMTL → ANV, Liquid Glass renders,
  `Resolution 3120 x 2080 / UI Looks like 1560 x 1040 @ 120 Hz / Online: Yes`.
  `boot-armed: 6/6 … iop_xe_flip=1 + iop_xe_defer=1`, `boot_steps = 123`.

Everything below is about one thing: **frames are still copied by the CPU, so WindowServer blocks on the
GPU every frame and the desktop caps around 30–56 fps instead of 120.**

## Finding 1 — on a 3120-wide panel the flip gate rejects every frame

`NVDisplayPipe::performTransaction()` (defer block):

```c
if (qr == kIOReturnSuccess && q.kva && q.pitch == P && q.height &&
    W == q.width && H == q.height && s->getPixelFormat() == 0x42475241u) {
        nvAccelXeFlipDeferQueue(this, s, W, H, P);
```

| | value | where from |
| --- | --- | --- |
| `P` — compositor IOSurface row bytes | **12544** | CoreAnimation aligns rows to 256 B; 3120 × 4 = 12480 → 12544 |
| `q.pitch` — scanout row bytes | **12480** | firmware console: `PLANE_STRIDE_1_A = 0xc3` (195 × 64) |

`3120 × 4 = 12480`, and `12480 / 256 = 48.75`, so CoreAnimation pads to 12544 while the firmware mode stays
12480. On the 2560-wide panels this was developed against, `2560 × 4 = 10240 = 256 × 40`, so the two are equal
by construction — which is presumably why this never came up. ArrowWhatever's slots are likewise a fixed
16,384,000 B = 2560 × 1600 × 4, and its counters include `iop_xe_flip_miss` described as "frame geometry
mismatch", so that path also assumes equality.

Consequence here: `iop_xe_defer_queued`, `iop_xe_flips`, `iop_xe_arm_last_us` all stay **0** and every frame
takes the copy path (4.4–4.7 ms at 3120×2080).

Note `q.pitch` is not read live from the register: `askFramebufferForScanout()` → `nvScanoutInfo`, which only
`NVRMFB` implements; for an IONDRV-style framebuffer it is the boot-aperture answer, so writing
`PLANE_STRIDE_1_A` inside `nvAccelXeFlipArm()` cannot change it (we tried: the code compiled and installed,
`iop_xe_arm_last_us` stayed 0 — dead code, because the gate is evaluated first).

## Finding 2 — loosening the gate makes it worse: the frame is claimed and never presented

We changed only that comparison to also accept a 64-byte-aligned pitch the register can hold:

```c
const bool pitchArmable = (P == q.pitch) || (P % 64u == 0u && (P / 64u) <= 0x7FFu);
if (qr == kIOReturnSuccess && q.kva && pitchArmable && q.height && …)
```

Result on three consecutive boots (`boot_steps = 123` every time, i.e. the flip bit is armed):

```
iop_ok               stops growing entirely
iop_xe_flips         0
iop_xe_arm_last_us   0        <- arm never entered
iop_xe_defer_queued  0        <- the queue never took the frame
iop_copy_sum_us      stops growing
screen               frozen on the last frame; WindowServer stuck; two boots needed a power cycle
```

So the gate was **not** the missing link: with it open, `performTransaction()` returns
`kIOReturnSuccess` from the defer block (the frame is claimed) but neither a flip nor a copy happens, and the
panel never updates. `iop_xe_defer_queued` is **0 both with the gate closed (A/B before the change) and with it
open**, which is what makes us think the gap is between `nvAccelXeFlipDeferQueue()` and `nvAccelXeFlipArm()`
rather than in the comparison. We rolled the change back (source, binary and AuxKC restored byte for byte) and
are back on the copy path.

Relevant: `nvAccelIopHome()` returns early for anything that is not an `NVRMNVDAFramebuffer`
(`:3866`), so on an IONDRV panel nothing ever restores `PLANE_SURF`/`PLANE_STRIDE` after an arm.

## Why this matters beyond this machine

With the copy path, WindowServer blocks every frame:

```
CompositorMetal::CompositeLayersToDestination  1109
  └ MetalContext::EndUpdate 669 → MetalContext::Flush 669
     └ -[NVMTLCommandBuffer commitAndWaitUntilSubmitted] 666
        └ -[NVMTLCommandBuffer waitUntilCompleted] 666 → __psynch_cvwait 666
```

666 of 669 samples in `waitUntilCompleted`. That is the 120 Hz gap: the compositor waits because the pixels
must be finished before we copy them. ArrowWhatever's patch 0009 ("submit without waiting, flip once the
engines are idle") is exactly what removes it — and its notes say the remaining bottleneck then is the
CA→Metal encoding path, which matches what we measure.

## What we are not claiming

We could not verify, independently of this stack, that the display engine accepts `PLANE_STRIDE_1_A = 0xc4`
on this panel: our userspace test tool cannot allocate a 26,091,520 B buffer (`kIOReturnNoMemory`), so a
`fill` + stride change only produced garbage from stale contents. So we cannot say whether programming the
stride together with `PLANE_SURF` would produce a correct image here; we can only say the deferred path never
gets far enough to try.

Happy to run any experiment you want on this machine — it is a dedicated test box, and we can add a
dead-man watchdog (reboot if `debug.xegfxaccel_iop_ok` does not advance for 30 s) so a bad attempt cannot
leave it wedged.

## Related, possibly useful for other testers

After two boots that ended in a hard freeze, the next boot came up in "CPU rendering, no HiDPI" and looked
like a broken kext. It was `XeGfxIntelFB`'s own guard:

```
XeGfxIntelFB: boot guard tripped (two starts without WindowServer): declining
NMXAccel: boot-armed: FAILED -- no XeGfx framebuffer with an open start gate after 30000 ms
          (steps 0x0, published 0x0, registered 0x0, piped 0x0, igpu 1, fb 0, iop 0, flip 0/defer 0)
XEGFX_DISPLAY: provider has no fb-index
```

`nvram xeintelfb-starts=%00` (and `nmintelfb-starts=%00`) plus a reboot brings it straight back. Worth knowing
before anyone starts replacing kexts.
