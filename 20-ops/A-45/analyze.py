import re, sys

path = r"D:/XEGFX-MIGRATE/.workbuddy/tmp/a45-load.txt"
rows = []
for line in open(path, encoding="utf-8", errors="replace"):
    line = line.strip()
    if not line or line == "DONE":
        continue
    m = re.match(r"(\d\d):(\d\d):(\d\d)\s+WindowServer\s+\d+\s+([\d.]+).*?kernel_task\s+\d+\s+([\d.]+).*?ok=(\d+)\s+copy_sum=(\d+)\s+copy_last=(\d+)\s+overrun=(\d+)", line)
    if not m:
        continue
    t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
    rows.append(dict(t=t, clock=m.group(0)[:8], ws=float(m.group(4)), kk=float(m.group(5)),
                     ok=int(m.group(6)), cs=int(m.group(7)), cl=int(m.group(8)), ov=int(m.group(9))))

print("样本点:", len(rows))
intervals = []
for a, b in zip(rows, rows[1:]):
    dt = b["t"] - a["t"]
    dok = b["ok"] - a["ok"]
    if dt <= 0:
        continue
    dcs = b["cs"] - a["cs"]
    fps = dok / dt
    copyms = (dcs / dok / 1000.0) if dok else 0.0
    intervals.append(dict(clock=b["clock"], dt=dt, ws=b["ws"], kk=b["kk"], fps=fps,
                          copyms=copyms, cl=b["cl"] / 1000.0, dok=dok,
                          wsper=(b["ws"] / fps if fps else 0), kper=(b["kk"] / fps if fps else 0)))

load = [i for i in intervals if i["fps"] >= 25]
idle = [i for i in intervals if i["fps"] < 25]


def avg(a, k):
    return sum(x[k] for x in a) / len(a) if a else 0.0


print("\n=== 逐段（fps>=25 记为负载段）===")
print("%-9s %4s %7s %5s %7s %8s %7s %7s" % ("clock", "dt", "WS", "fps", "copy_ms", "WS/frame", "kern/fr", "last_ms"))
for i in intervals:
    tag = "LOAD" if i["fps"] >= 25 else "    "
    print("%-9s %4d %7.0f %5.1f %7.2f %8.1f %7.1f %7.2f %s" % (
        i["clock"], i["dt"], i["ws"], i["fps"], i["copyms"], i["wsper"], i["kper"], i["cl"], tag))

for name, seg in (("负载段 fps>=25", load), ("空闲段 fps<25", idle)):
    print("\n--- %s  样本 %d ---" % (name, len(seg)))
    print("WS   ms/s 均值 %.0f" % avg(seg, "ws"))
    print("kern ms/s 均值 %.0f" % avg(seg, "kk"))
    print("帧率 fps  均值 %.1f" % avg(seg, "fps"))
    print("每帧拷贝 ms 均值 %.2f  (末帧 last_ms 均值 %.2f)" % (avg(seg, "copyms"), avg(seg, "cl")))
    print("每帧 WS   ms %.1f   每帧 kern ms %.1f" % (avg(seg, "wsper"), avg(seg, "kper")))
    tot = avg(seg, "copyms")
    tot2 = avg(seg, "cl")
    print("拷贝占 kern 比例: %.1f%% (按 copy_ms) / %.1f%% (按 last_ms)" % (
        100 * tot * avg(seg, "fps") / avg(seg, "kk") if avg(seg, "kk") else 0,
        100 * tot2 * avg(seg, "fps") / avg(seg, "kk") if avg(seg, "kk") else 0))
