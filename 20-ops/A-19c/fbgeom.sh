#!/bin/bash
# 只在 macOS 报 3120x2080 时把平面几何切成原生并锁存；否则完全不动。
# A-19c：在 latch 之前把帧缓冲的 GGTT 入口从 0xFE600000 搬到 0xFD000000
#        （加速器分配器从 0xFEE00000 往下吃，会吃掉 0xFE600000 起那 8 MB；
#          0xFD000000..0xFEA00000 这一段它不碰 —— 见 A-19a 报告）
LOG=/var/db/xegfx/fbgeom.log
CTL=/usr/local/bin/xenative-ctl
FB_IDX=0xFD000          # 6656 页 = 0x1A00 页；覆盖 0xFD000..0xFE9FF
FB_PAGES=6656
FB_PHYS=0x800000        # 物理起始（未变）
FB_SURF=0xFD000000      # 平面寄存器要指的新入口
mkdir -p /var/db/xegfx
log(){ echo "[$(date '+%F %T')] $*" >> "$LOG"; echo "[$(date '+%F %T')] $*"; }

log "start (pid $$)"

if [ ! -x "$CTL" ]; then log "$CTL 不存在，退出"; exit 0; fi

res=""
for i in $(seq 1 24); do          # 最多等 120 s，等 WindowServer 起来
    res=$(system_profiler SPDisplaysDataType 2>/dev/null | grep -m1 "Resolution:")
    case "$res" in
        *"3120 x 2080"*) break ;;
    esac
    sleep 5
done
case "$res" in
    *"3120 x 2080"*) ;;
    *) log "console 不是 3120x2080（$res），不动"; exit 0 ;;
esac

log "检测到原生控制台（$res），应用 3120 几何"
"$CTL" regset 3 2 195 1 0x081F0C2F 3 0x0C2F081F >> "$LOG" 2>&1

# --- 1) 把帧缓冲的 GGTT 入口建到 0xFD000000（6656 页，物理 0x800000 起）---
#     幂等：重复建同样的映射无害。bad 不为 0 就绝不改 SURF。
MAP_OUT=$("$CTL" mapfast "$FB_IDX" "$FB_PAGES" "$FB_PHYS" 2>&1)
echo "$MAP_OUT" >> "$LOG"
case "$MAP_OUT" in
    *"bad 0"*)
        log "mapfast ok（bad 0）"
        ;;
    *)
        log "mapfast FAILED（bad != 0 或未出结果），保留原 SURF 不动"
        "$CTL" latch >> "$LOG" 2>&1   # 仍沿用旧入口锁存几何，保证屏幕正常
        log "已按旧入口 latch，退出"
        exit 0
        ;;
esac

# --- 2) 把平面指过去 ---
"$CTL" reg surf "$FB_SURF" >> "$LOG" 2>&1

# --- 3) 再 latch（几何与 SURF 都是双缓冲，靠它锁存）---
"$CTL" latch >> "$LOG" 2>&1

"$CTL" status >> "$LOG" 2>&1
log "完成"
