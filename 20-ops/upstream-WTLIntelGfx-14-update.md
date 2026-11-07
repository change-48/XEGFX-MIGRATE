# Comment posted to watchthelight-labs/WTLIntelGfx#14 on 2026-11-07

## Update: the flip works now — the report above describes the 11-01 state

Thanks for the reply, and for filing the logs. This issue and the README row are now out of date, so here is where that box actually ended up.

**TL;DR:** deferred zero-copy flip is the boot default here since A-84. Frames are no longer copied. The desktop sits at p50 25.2 ms (39.6 fps), p90 34 ms, at 3120x2080 / HiDPI 1560x1040 @ 120 Hz, with Liquid Glass working.

### On your stride question — we did not have to write `PLANE_STRIDE` at arm time

You wrote: *"For your panel the stride would have to be written together with the surface address when the flip is armed."* That turned out not to be necessary on this stack. What we did instead:

- The framebuffer's boot aperture was rebuilt at **pitch 12544** (`aperture served ... len 26091520 (3120x2080 pitch 12544)`), so `q.pitch` no longer reports the firmware console's 12480.
- The GGTT flip slot is allocated at **26,091,520 B = 12544 x 2080 = 6370 pages**, matching the compositor's IOSurface row bytes. Current hardware state is `PLANE_STRIDE 196` (196 x 64 = 12544), with `PLANE_SURF` armed at vblank by the defer worker.
- **`NV_XE_FLIP_MAX_PAGES` has to be raised from 4224 to 8192.** 4224 pages = 16.5 MB, sized for 2560x1600; a 3120-wide frame needs 6370 pages and `xgFlipMap` silently refuses it. This is the one hard requirement a 3120-wide panel imposes on the flip slots.

So the `q.pitch == P` gate is satisfied by construction rather than relaxed. Finding 2 in the original report is still valid and worth keeping: relaxing the gate produced a claimed-but-never-presented frame (frozen panel, `iop_ok` stops, three boots, two needed a power cycle). We rolled that back byte-for-byte.

Two intermediate attempts also failed and were reverted, in case they save someone time:

- **A-53:** the first 12544-stride flip did run (`iop_xe_flips=1600`, zero failures) but was **slower** (drag median 23 fps vs 30-56 on the copy path), because arming re-mapped all 6370 PTEs every frame (~20 ms) plus a GPU TLB flush.
- **A-54:** a flip-slot mapping cache (97% hit, kernel 560-845 -> 228 ms/s) did not recover drag fps.

The version that works uses deferred flip + per-frame surface marks (XGDR selectors) + page-off, with `flip=1 defer=1` armed by `bootArm` at boot.

One correction to my own 11-01 numbers: it said the desktop caps "around 30-56 fps". With the flip path the copy-path p50 of 27.95 ms / 35.6 fps became 25.2 ms / 39.6 fps — the median moved only a little, but **p90 went from 107 ms to 34 ms**. The tail, not the median, was the win.

### Where the 40 fps ceiling actually is

We instrumented one frame into five segments (A-89):

| segment | p50 | corr with frame time |
| --- | --- | --- |
| submit -> worker | 0.03 ms | +0.04 |
| worker -> GT idle -> arm | 5.81 ms | -0.05 |
| arm -> panel latch | 4.89 ms | +0.02 |
| **driver total** | **10.84 ms** | **-0.01** |
| **after the signal (WS composite + Present + `AccessComplete`)** | **14.10 ms** | **+0.95** |

Four independent attempts to compress the driver side (class lock, moving submission out of the lock, early signalling, and workload staircases of +1000/+3000 us per submit) all left p50 unchanged; the time is absorbed by that 14 ms tail (`corr(L,G) = -0.31`, about half a tick of slack). The panel is not the limiter: 1-tick frames genuinely exist (4-5%, dt 11.6 ms = 86 fps), refresh measured at 8.335 ms. The limit is a serial single-command-buffer chain — `AccessComplete -> waitUntilScheduled` waits for this frame's submission to be scheduled before the next composite can begin. So patch 0009's "submit without waiting" is necessary but not sufficient here; 60 fps needs a second in-flight command buffer.

A useful sanity check on all this: with `flip=1` the GT runs at **1625 MHz median / 2150 MHz peak** under drag; forcing `flip=0` pins it at a constant **800 MHz** (RPn), because the copy path is CPU-bound.

### For `xe-zerocopy-flip`

- I would not special-case stride writes at arm time for this panel. Building the slot at the compositor's pitch works, and writing `PLANE_STRIDE_1_A` inside the arm path was dead code here because the gate is evaluated first (as the original report says).
- Do size the flip slots from the panel's actual pitch rather than a fixed 2560x1600.
- Happy to test the branch once it runs on your laptop. The dead-man watchdog offer still stands — I noticed you already have `WTLGpuGuard`, `WTLStagedReboot` and `WTLBootDump`, so I'll use those instead of hand-rolling one. Cold boot here still needs one WindowServer restart after login (A-90 tried three fixes, all disproved), so a staged-reboot harness fits well.

All raw reports (A-41 through A-91), measurement scripts and per-gate data are public at https://github.com/change-48/XEGFX-MIGRATE — the README carries the consolidated conclusion. The README row here ("frames copied, not flipped") and the opening paragraph of this issue can be updated whenever convenient; I'm not opening a PR for a wording change.
