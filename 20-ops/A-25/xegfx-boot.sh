#!/bin/bash
# A-25b: 开机把 CPU 侧加速链路拉起来 core -> stage7 -> accel -> accelfb=3
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
STATE=/var/db/xegfx-boot ; LOG=/var/db/xegfx/boot.log
mkdir -p "$STATE"
log(){ echo "[$(date "+%F %T")] $*" >> "$LOG"; }

if [ -f "$STATE/disabled" ]; then log "self-disabled, skip"; exit 0; fi
N=$(( $(cat "$STATE/attempts" 2>/dev/null || echo 0) + 1 )); echo "$N" > "$STATE/attempts"
if [ "$N" -gt 3 ]; then log "连续 $N 次未成功 → 自我禁用"; touch "$STATE/disabled"; exit 0; fi
log "=== boot-arm start (attempt $N) ==="

# 0) 等 fbgeom 完成（本次开机内），core 依赖其后的 framebuffer 状态
BOOT_EPOCH=$(/usr/sbin/sysctl -n kern.boottime | /usr/bin/awk -F'[ ,{}]+' '{print $4}')
for i in $(seq 1 30); do
    FM=$(/usr/bin/stat -f %m /var/db/xegfx/fbgeom.log 2>/dev/null || echo 0)
    if [ "$FM" -ge "${BOOT_EPOCH:-0}" ] && tail -20 /var/db/xegfx/fbgeom.log 2>/dev/null | grep -q "完成"; then
        log "fbgeom 本次开机已就绪（等待 $((i*2))s）"; break
    fi
    sleep 2
done

ok=0
for i in $(seq 1 20); do                      # 等 IOPCIDevice 与 drm 就绪，最多 100 s
    /usr/bin/kmutil load -b org.watchthelight.XeGfx >/dev/null 2>&1
    if /usr/sbin/sysctl -a 2>/dev/null | grep -q debug.xegfx_stage; then ok=1; break; fi
    sleep 5
done
[ "$ok" = 1 ] || { log "core 未能暴露 sysctl（放弃本轮）"; exit 0; }
log "core loaded, stage sysctl present"

/usr/sbin/sysctl -w debug.xegfx_wdog=60  >/dev/null 2>&1
/usr/sbin/sysctl -w debug.xegfx_stage=7  >/dev/null 2>&1
sleep 20
log "stage=7 已推进"

for i in $(seq 1 10); do
    /usr/bin/kmutil load -b org.watchthelight.XeGfxAccel >/dev/null 2>&1
    if /usr/sbin/ioreg -c NMXAccel -l 2>/dev/null | grep -q MetalPluginName; then
        log "NMXAccel 实例出现（第 $i 次尝试）"; break
    fi
    sleep 3
done
sleep 3
/usr/sbin/sysctl -w debug.xegfxaccelfb=3 >/dev/null 2>&1
log "accelfb=3 已设置"

R=$(/usr/sbin/sysctl -b debug.xegfx_results 2>/dev/null | od -A n -t d4 | tr -s ' \n' ' ')
log "results:$R"
NA=$(/usr/sbin/ioreg -c NMXAccel -l -w0 2>/dev/null | grep -c MetalPluginName)
log "NMXAccel MetalPluginName 行数: $NA"

WS=$(/usr/bin/pgrep -x WindowServer)
if [ -n "${WS:-}" ] && /usr/bin/vmmap "$WS" 2>/dev/null | grep -qi nvmtl; then
    CNT=$(/usr/bin/vmmap "$WS" 2>/dev/null | grep -ci nvmtl)
    log "vmmap nvmtl = $CNT ✅"
    echo 0 > "$STATE/attempts"
else
    log "vmmap nvmtl = 0（WS 可能还没加载插件，稍后复查）"
fi
log "=== done ==="
