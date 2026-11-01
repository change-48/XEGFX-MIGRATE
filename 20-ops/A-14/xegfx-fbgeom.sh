#!/bin/bash
# 只在 macOS 报 3120x2080 时把平面几何切成原生并锁存；否则完全不动。
LOG=/var/db/xegfx/fbgeom.log
CTL=/usr/local/bin/xenative-ctl
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
"$CTL" latch                                     >> "$LOG" 2>&1
"$CTL" status                                    >> "$LOG" 2>&1
log "完成"
