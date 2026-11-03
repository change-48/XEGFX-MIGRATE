#!/usr/bin/env python3
"""A-89 P1: label every frame 2-tick / 3-tick, then decompose each frame time into its segments.

Input: dumps of debug.xegfxaccel_iop_tl_raw (absolute columns):

    # idx sub_ns vcnt enter_ns arm_ns latch_ns ret_ns path sid ok w h pitch

    sub    mach time at performTransaction entry (the display thread's grid point for this frame)
    vcnt   BAR0 0x70040 there -- the panel's refresh counter, 8.335 ms per count on this panel (measured, not assumed)
    enter  the flip worker picked the frame up / started the arm
    arm    PLANE_SURF_1_A written
    latch  the panel latched it (a vblank, polled at 1 ms)
    ret    the worker returned; the transaction is signalled right after

Per frame, dt = sub[i+1] - sub[i] decomposes exactly into

    dt = (sub->enter) + (enter->arm) + (arm->latch) + (latch->ret) + (ret->next sub)
       = qsub        + e2a         + a2l          + l2r          + G

The first four are inside the driver; G is everything between our signal and WindowServer's next submission.
The point of the decomposition is the variance share: whichever segment covaries with dt is the one that moves the
frame time, and the others are noise that happens to be there.

The tick label comes from the hardware counter (vcnt delta), so it does not depend on any assumption about 8.333 ms.
"""
import sys
from collections import Counter, defaultdict


def pct(v, p):
    if not v:
        return float("nan")
    k = int(round((len(v) - 1) * p))
    return v[k]


def f3(v):
    return "%.2f" % v


def load(paths):
    rows = {}
    for p in paths:
        for line in open(p, encoding="utf-8", errors="replace"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            f = line.split()
            if len(f) < 13:
                continue
            rows[int(f[0])] = dict(
                idx=int(f[0]), sub=int(f[1]), vcnt=int(f[2]), enter=int(f[3]), arm=int(f[4]),
                latch=int(f[5]), ret=int(f[6]), path=int(f[7]), sid=int(f[8]), ok=int(f[9]),
            )
    return [rows[k] for k in sorted(rows)]


def main():
    rows = load(sys.argv[1:])
    fr = [r for r in rows if r["path"] == 2 and r["ret"] and r["arm"] and r["latch"]]
    print("frames: %d total, %d usable on the deferred flip path (path 2)" % (len(rows), len(fr)))
    if len(fr) < 20:
        print("not enough path-2 frames")
        return

    # --- the refresh period, measured end to end: total time / total counter steps.  Per-frame ratios are quantised to
    # whole counts, so a least squares through the origin over 200 short intervals is biased; the endpoint ratio is not.
    a, b = fr[0], fr[-1]
    P = ((b["sub"] - a["sub"]) / 1e6) / ((b["vcnt"] - a["vcnt"]) & 0xFFFFFFFF)
    print("refresh period, endpoint: %.3f ms  (counter steps %d over %.0f ms)"
          % (P, (b["vcnt"] - a["vcnt"]) & 0xFFFFFFFF, (b["sub"] - a["sub"]) / 1e6))

    for r in fr:
        r["qsub"] = (r["enter"] - r["sub"]) / 1e6
        r["e2a"] = (r["arm"] - r["enter"]) / 1e6
        r["a2l"] = (r["latch"] - r["arm"]) / 1e6
        r["l2r"] = (r["ret"] - r["latch"]) / 1e6
        r["L"] = (r["ret"] - r["sub"]) / 1e6
        r["arm_phase"] = P - r["a2l"]          # where in the refresh the arming write landed

    groups = defaultdict(list)
    for x, y in zip(fr, fr[1:]):
        dv = (y["vcnt"] - x["vcnt"]) & 0xFFFFFFFF
        dt = (y["sub"] - x["sub"]) / 1e6
        if not (0 < dt < 500) or not (0 < dv <= 16):
            continue
        g = y
        g["dv"] = dv
        g["dt"] = dt
        g["G"] = (y["sub"] - x["ret"]) / 1e6
        groups[dv].append(g)

    labelled = [r for r in fr if "dv" in r]
    tot = len(labelled)
    print("\n== refreshes per frame (hardware counter)")
    for dv in sorted(groups):
        v = groups[dv]
        print("  %2d refresh(es)  %5.2f ms nominal   %3d frames  %4.1f%%   dt p50 %6.2f  dt p90 %6.2f"
              % (dv, dv * P, len(v), 100.0 * len(v) / tot, pct(sorted(r["dt"] for r in v), .5),
                 pct(sorted(r["dt"] for r in v), .9)))

    def table(sel, label):
        segs = ("qsub", "e2a", "a2l", "l2r", "L", "G", "dt")
        print("  %-12s %4s " % (label, "n") + " ".join("%-13s" % s for s in segs))
        for name, key in (("all frames", lambda r: True), ("2 tick", lambda r: r["dv"] == 2),
                          ("3 tick", lambda r: r["dv"] == 3)):
            v = [r for r in labelled if key(r)]
            if not v:
                continue
            cells = []
            for s in segs:
                d = sorted(r[s] for r in v)
                cells.append("%.2f/%.2f/%.2f" % (pct(d, .1), pct(d, .5), pct(d, .9)))
            print("  %-12s %4d " % (name, len(v)) + " ".join("%-13s" % c for c in cells))

    print("\n== where the frame goes (ms, p10/p50/p90)")
    table(labelled, "")

    # --- variance share: dt is exactly the sum of the segments, so var(dt) = sum of cov(segment, dt)
    print("\n== what moves dt (share of var(dt), by covariance with dt)")
    dts = [r["dt"] for r in labelled]
    m = sum(dts) / len(dts)
    var = sum((x - m) ** 2 for x in dts) / len(dts)
    for s in ("qsub", "e2a", "a2l", "l2r", "G"):
        col = [r[s] for r in labelled]
        mc = sum(col) / len(col)
        cov = sum((c - mc) * (d - m) for c, d in zip(col, dts)) / len(dts)
        cor = cov / (var ** 0.5 * (sum((c - mc) ** 2 for c in col) / len(col)) ** 0.5) if var > 0 else 0
        print("  %-5s mean %6.2f  sd %5.2f  share %+6.1f%%   corr with dt %+.2f"
              % (s, mc, (sum((c - mc) ** 2 for c in col) / len(col)) ** 0.5, 100.0 * cov / var if var else 0, cor))

    # --- is the arming write parked at a vblank (locked), or does it land anywhere in the refresh?
    print("\n== arm phase inside a refresh (P - arm->latch): 0 = armed just after a vblank, %.2f = just before" % P)
    ap = sorted(r["arm_phase"] for r in labelled)
    print("  p10/p25/p50/p75/p90: " + " ".join("%.2f" % pct(ap, p) for p in (.1, .25, .5, .75, .9))
          + "   (uniform over 0..%.2f = %.2f/%.2f)" % (P, P * .1, P * .9))

    # --- clustering: runs of 2-tick frames tell us whether the short frames come in a periodic beat
    runs, cur = [], 0
    for r in labelled:
        if r["dv"] == 2:
            cur += 1
        else:
            if cur:
                runs.append(cur)
            cur = 0
    if cur:
        runs.append(cur)
    n2 = sum(1 for r in labelled if r["dv"] == 2)
    print("\n== clustering")
    print("  2-tick frames: %d / %d (%.0f%%)" % (n2, tot, 100.0 * n2 / tot))
    print("  runs of consecutive 2-tick frames: %s   longest %d" % (Counter(runs).most_common(6), max(runs or [0])))
    alt = sum(1 for x, y in zip(labelled, labelled[1:]) if x["dv"] == 2 and y["dv"] >= 3)
    print("  2-tick followed by >=3: %d of %d (%.0f%%)" % (alt, n2, 100.0 * alt / max(1, n2)))

    print("\n== per frame (idx sub_ms vcnt dv dt qsub e2a a2l l2r G sid)")
    for r in labelled:
        print("  %5d %10.3f %6d %2d %6.2f %5.2f %5.2f %5.2f %5.2f %5.2f %3d"
              % (r["idx"], r["sub"] / 1e6, r["vcnt"], r["dv"], r["dt"],
                 r["qsub"], r["e2a"], r["a2l"], r["l2r"], r["G"], r["sid"]))


if __name__ == "__main__":
    main()
