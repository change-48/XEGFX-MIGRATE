# Upstream issue draft: deferred zero-copy flip never runs on a 3120-wide panel (pitch equality gate)

> Draft for `watchthelight-labs/NvGfx` (branch `xe`). Written from a working bring-up on
> Arrow Lake-HX `8086:7D51` with a 3120×2080 eDP panel, XeGfxIntelFB owning the panel,
> XeGfxAccel holding head 4, `boot_steps=123`, desktop composited on the iGPU.

## Summary

The K9b deferred zero-copy flip is armed correctly (`debug.xegfxaccel_iop_xe_flip=1`,
`debug.xegfxaccel_iop_xe_defer=1`, `boot-armed: 6/6 … READY in 3001 ms`) but **never runs**:
`iop_xe_defer_queued`, `iop_xe_flips` and `iop_xe_arm_last_us` all stay 0 while every frame
goes through the CPU copy path (4.4–4.7 ms per frame at 3120×2080).

The gate is this line in `NVDisplayPipe::performTransaction()`:

```c
if (qr == kIOReturnSuccess && q.kva && q.pitch == P && q.height &&
    W == q.width && H == q.height && s->getPixelFormat() == 0x42475241u) {
```

On a 3120-wide panel this comparison can never be true:

| | value | where from |
| --- | --- | --- |
| `P` (compositor IOSurface row bytes) | **12544** | CoreAnimation aligns rows to 256 bytes; 3120×4 = 12480 → 12544 |
| `q.pitch` (scanout row bytes) | **12480** | the firmware console: `PLANE_STRIDE_1_A = 0xc3` (195 × 64) |

`3120 × 4 = 12480`, and `12480 / 256 = 48.75`, so CoreAnimation pads to 12544 while the firmware
mode stays 12480. On the 2560-wide panels this was developed against, `2560 × 4 = 10240 = 256 × 40`,
so the two values are equal by construction and the gate always passes — which is presumably why
this never showed up.

## Why it cannot be fixed after the gate

`q.pitch` is not read from `PLANE_STRIDE_1_A` at call time: it comes from
`askFramebufferForScanout()` → `nvScanoutInfo`, which only NVRMFB implements. For an
IONDRV-style framebuffer (`XeGfxIntelFramebuffer`) the answer is the boot-aperture fallback,
which reports the firmware mode's pitch. So programming the stride inside `nvAccelXeFlipArm()`
does not change `q.pitch`, and the arm is never reached to begin with. (We tried exactly that:
the added stride write compiled and installed, `iop_xe_arm_last_us` stayed 0 — dead code.)

## What would unblock it

Any one of these, all of which we did not want to do unilaterally:

1. **Relax the gate** to allow `P != q.pitch` when `P % 64 == 0 && P / 64 <= 0x7FF`, and program
   `PLANE_STRIDE_1_A = P / 64` together with the arming write of `PLANE_SURF_1_A`
   (restoring both on the `!latched` path). The panel accepts arbitrary 64-byte-aligned strides —
   we have driven it at a different stride before.
2. **Make the framebuffer report the compositor's pitch**, i.e. have the scanout answer carry the
   row bytes the surface actually uses, so the existing gate passes unchanged.
3. **Allocate/publish a 256-aligned scanout** (3120 → 12544 stride) at mode-set time, so the
   firmware-mode pitch and the compositor's agree from the start.

Option 1 is the smallest: one comparison plus one register write, and it degrades to today's
behaviour when `P % 64 != 0`.

## Reproduction data

```
debug.xegfxaccel_boot_steps        123        (bit 64 = flip armed)
boot-armed: 6/6 … (wait_idle 1, idle_ms 40, report_busy 1)
boot-armed: READY in 3001 ms -- head 4 registered and piped, scanout 1 aperture 0, iop/flip/defer armed

PLANE ctl=84000008 stride=000000c3 size=081f0c2f pos=00000000 surf=fd000000
K6 plane 0: surface yes id 0x6 3120x2080 row 12544 fmt 0x42475241

iop_ok 3858   iop_xe_defer_queued 0   iop_xe_flips 0   iop_xe_arm_last_us 0
iop_copy_sum_us 18719611  → 4.4 ms/frame
```

Also worth noting for other IONDRV-style panels: `nvAccelIopFlip()` returns false for anything
that is not an `NVRMNVDAFramebuffer`, so `debug.xegfxaccel_iop_flip` can never move there —
only the Xe path can.

## Environment

macOS 26.5.2, OpenCore, Arrow Lake-HX `8086:7D51` (Xe-LPG+), eDP 3120×2080 @ 120 Hz,
kernel driver and accelerator built from `xe-2.6.2` with the plugin at the same tip.
