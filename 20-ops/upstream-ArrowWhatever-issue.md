# Issue opened at Mountainatic/ArrowWhatever on 2026-11-07

**Title:** 3120-wide panel: patches 0008/0009's `q.pitch == P` gate rejects every frame

---

`watchthelight` suggested in [WTLIntelGfx#14](https://github.com/watchthelight-labs/WTLIntelGfx/issues/14) that I post this finding here too, since the deferred flip queue and the `q.pitch == P` gate come from your patches 0008/0009 rather than from their tree. This is a measurement report, not a patch.

## Environment

- HONOR MagicBook Art 14 2025, Core Ultra 7 255H, Xe-LPG+ `8086:7D51`, eDP **3120x2080 @ 120 Hz**, macOS 26 Tahoe / OpenCore.
- XeGfx `xe-2.6.2` core + accelerator, NVMTL/ANV plugin; desktop composites on the iGPU, HiDPI `1560 x 1040 @ 120 Hz`, Liquid Glass renders.

## The mismatch

In `NVDisplayPipe::performTransaction()` (defer block):

```c
if (qr == kIOReturnSuccess && q.kva && q.pitch == P && q.height &&
    W == q.width && H == q.height && s->getPixelFormat() == 0x42475241u) {
        nvAccelXeFlipDeferQueue(this, s, W, H, P);
```

| | value | origin |
| --- | --- | --- |
| `P` — compositor IOSurface row bytes | **12544** | CoreAnimation aligns rows to 256 B; 3120 x 4 = 12480 -> 12544 |
| `q.pitch` — scanout row bytes | **12480** | firmware console `PLANE_STRIDE_1_A = 0xc3` (195 x 64) |

`3120 x 4 = 12480`, and `12480 / 256 = 48.75`, so CoreAnimation pads to 12544 while the firmware mode stays 12480. On a 2560-wide panel `2560 x 4 = 10240 = 256 x 40`, so the two are equal by construction — presumably why this never surfaced.

**Observed:** `iop_xe_defer_queued`, `iop_xe_flips`, `iop_xe_arm_last_us` all stay **0**; every frame takes the copy path (4.4-4.7 ms at 3120x2080). `q.pitch` is not read live from the register — `askFramebufferForScanout()` -> `nvScanoutInfo` is only implemented by `NVRMFB`, so for an IONDRV-style framebuffer it returns the boot-aperture answer. Writing `PLANE_STRIDE_1_A` inside `nvAccelXeFlipArm()` is dead code here because the gate is evaluated first (we built and installed that; `iop_xe_arm_last_us` stayed 0).

## Relaxing the gate is not the fix

We changed only that comparison to also accept a 64-byte-aligned pitch the register can hold:

```c
const bool pitchArmable = (P == q.pitch) || (P % 64u == 0u && (P / 64u) <= 0x7FFu);
if (qr == kIOReturnSuccess && q.kva && pitchArmable && q.height && ...)
```

Three consecutive boots, `boot_steps = 123` each time: `iop_ok` stops growing, `iop_xe_flips = 0`, `iop_xe_arm_last_us = 0`, `iop_xe_defer_queued = 0`, `iop_copy_sum_us` stops, screen frozen on the last frame, two boots needed a power cycle. So `performTransaction()` returns success from the defer block (the frame is claimed) but neither a flip nor a copy happens. `iop_xe_defer_queued` was 0 **both** with the gate closed and with it open, which points at the gap between `nvAccelXeFlipDeferQueue()` and `nvAccelXeFlipArm()`, not at the comparison. Rolled back byte-for-byte.

Related: `nvAccelIopHome()` returns early for anything that is not an `NVRMNVDAFramebuffer`, so on an IONDRV panel nothing restores `PLANE_SURF` / `PLANE_STRIDE` after an arm.

## What did work on this panel

Not a stride write at arm time. We made the two pitches equal by construction:

- The framebuffer boot aperture was rebuilt at **pitch 12544** (`aperture served ... len 26091520 (3120x2080 pitch 12544)`), so `q.pitch` stops reporting 12480.
- The GGTT flip slot is allocated at **26,091,520 B = 12544 x 2080 = 6370 pages**. Current hardware state: `PLANE_STRIDE 196` (196 x 64 = 12544).
- **`NV_XE_FLIP_MAX_PAGES` must go from 4224 to 8192.** 4224 pages = 16.5 MB sized for 2560x1600; 6370 pages is silently refused by `xgFlipMap`.

Two intermediate versions were reverted and may be worth knowing about:

- A first 12544-stride flip ran (`iop_xe_flips=1600`, zero failures) but was **slower** than the copy path (drag median 23 fps vs 30-56), because arming re-mapped all 6370 PTEs every frame (~20 ms) plus a GPU TLB flush.
- A flip-slot mapping cache (97% hit) did not recover drag fps.

The working version is a deferred flip with per-frame surface marks and page-off, armed as the boot default. It reaches p50 25.2 ms (39.6 fps) / p90 34 ms, and the GT climbs from a constant 800 MHz (copy path, RPn) to 1625 MHz median / 2150 MHz peak.

## Suggestion for ArrowWhatever

From the description in WTLIntelGfx#14, your slots are a fixed **16,384,000 B = 2560 x 1600 x 4**, and your counters include `iop_xe_flip_miss` described as "frame geometry mismatch". Both appear to assume `q.pitch == P`. On a panel whose width is not a multiple of 64 pixels, sizing the slot (and the geometry check) from the compositor's actual row bytes rather than a fixed 2560x1600 would avoid the silent no-flip case. I have not measured your tree directly, so please treat this as a suggestion from the observed failure mode, not a claim about your code.

## Data

- Full write-up and the maintainer discussion: [WTLIntelGfx#14](https://github.com/watchthelight-labs/WTLIntelGfx/issues/14)
- Raw reports A-41 through A-91, measurement scripts and per-gate numbers: https://github.com/change-48/XEGFX-MIGRATE

Happy to run a specific probe or test a patch on this box if that helps — it is a dedicated test machine.
