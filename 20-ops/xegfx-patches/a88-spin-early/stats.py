#!/usr/bin/env python3
"""A-88 P1: p50/p90/p99 + the step histogram for a frame-times dump.

The A-88 ladder is read from the *shape* of the distribution, not just p50: a frame time that is quantised to whole
refreshes shows up as spikes at 8.33/16.67/25.0/33.3 ms. p50 lands on whichever tick holds the most frames, so p50 alone
can hide "half the frames are already at the next tick".
"""
import glob
import os
import statistics
import sys


def pct(v, p):
    if not v:
        return float("nan")
    k = int(round((len(v) - 1) * p))
    return v[k]


def hist(v):
    # buckets at the 120 Hz refresh (8.333 ms) boundaries, in ms
    edges = [(0, 9.5), (9.5, 17.5), (17.5, 26.2), (26.2, 34.5), (34.5, 43), (43, 1e9)]
    out = []
    for a, b in edges:
        n = sum(1 for x in v if a <= x < b)
        out.append((f"{a:g}-{b:g}", n))
    return out


def load(path):
    v = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        try:
            v.append(int(line) / 1000.0)   # us -> ms
        except ValueError:
            pass
    return sorted(x for x in v if 1.0 < x < 500.0)   # drop the first-start artefact


files = sys.argv[1:] or sorted(glob.glob("iv-*.txt"))
print(f"{'tag':>10} {'n':>5} {'p50':>7} {'p90':>7} {'p99':>7} {'mean':>7}  histogram (8.33 ms ticks, %)")
for f in files:
    v = load(f)
    if not v:
        print(f"{os.path.basename(f)[3:-4]:>10}   (no data)")
        continue
    tag = os.path.basename(f)[3:-4]
    h = hist(v)
    tot = sum(n for _, n in h) or 1
    hs = " ".join(f"{lab}:{100.0*n/tot:.0f}%" for lab, n in h if n)
    print(f"{tag:>10} {len(v):>5} {pct(v,.5):7.2f} {pct(v,.9):7.2f} {pct(v,.99):7.2f} {statistics.mean(v):7.2f}  {hs}")
