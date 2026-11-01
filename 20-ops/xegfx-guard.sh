#!/bin/bash
# xegfx-guard.sh — Gate 0 开机护栏
# 开机后若发现 armed 且不是第一次（说明上一次 armed 开机没活下来），自动回滚并重启。
# 存活 120 秒即判定本次开机成功：写 ok、attempts 归零、清 armed。
#
# P-0a (2026-10-31) 修复：清 armed/attempts 必须在调用回滚**之前**。
#   原顺序是「回滚 → 清标记」，而回滚 --auto 结尾自己会 reboot，
#   一旦 rm 没跑到，armed 就残留 → 下次开机 attempts 再加 → 再次回滚 → 回滚死循环。

STATE=/var/db/xegfx
mkdir -p "$STATE"
LOG=$STATE/guard.log

log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG"; echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"; }

log "=== guard 启动 (boot $(sysctl -n kern.boottime 2>/dev/null)) ==="

att=0
[ -f "$STATE/attempts" ] && att=$(cat "$STATE/attempts" 2>/dev/null)
case "$att" in ''|*[!0-9]*) att=0 ;; esac

if [ -f "$STATE/armed" ]; then
    att=$((att+1))
    echo "$att" > "$STATE/attempts"
    if [ "$att" -gt 1 ]; then
        log "前一次 armed 开机没活下来（attempts=$att）→ 回滚"
        # 关键：先清标记再回滚。回滚内部会重启，标记不先清就可能反复触发。
        rm -f "$STATE/armed" "$STATE/attempts"
        if /usr/local/bin/xegfx-rollback.sh --auto >> "$LOG" 2>&1; then
            log "回滚完成，重启"
            /sbin/reboot
        else
            log "回滚失败，不重启，等人"
            echo "ROLLBACK-FAILED $(date)" > "$STATE/failed"
        fi
        exit 0
    fi
    log "armed 首次开机（attempts=$att），进入 120s 存活观察"
fi

sleep 120
date > "$STATE/ok"
echo 0 > "$STATE/attempts"
rm -f "$STATE/armed"
log "存活 120s，写 ok，attempts 归零"
