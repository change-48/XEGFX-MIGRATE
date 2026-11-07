#!/bin/bash
# XeGfx bring-up: core -> stage 7 -> accel -> scanout pipe -> transaction hooks.
# Runs AFTER a real GUI login and after the desktop has settled (A-25b: doing this at
# boot/loginwindow made WindowServer assert).  Every step is verified by an observable;
# on failure it stops and leaves the machine on the firmware framebuffer (safe state).
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

# A-79: the plugin must not wait for the GPU before it hands the frame over (every round so far
# touched this by hand and forgot it after a reboot -- see G7-A79 铁律).
: > /private/tmp/nvmtl-ws-nowait ; chmod 644 /private/tmp/nvmtl-ws-nowait
LOGF=/var/log/xegfx-bringup.log
STATE=/var/db/xegfx
mkdir -p "$STATE"
WRITE() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOGF"; }

K=/usr/bin/kmutil
[ -x "$K" ] || K=/usr/sbin/kmutil
S=/usr/sbin/sysctl
BOOT=$(date -r "$($S -n kern.boottime | awk -F'[ ,{}]+' '{print $4}')" '+%F %T' 2>/dev/null)

# core present?  NOTE: bare 'org.watchthelight.XeGfx' also matches XeGfxIntelFB /
# XeGfxAGDC / XeGfxDisplay, which load at boot -- the trailing " (" makes it exact.
core_present()  { $K showloaded 2>/dev/null | grep -q 'org\.watchthelight\.XeGfx ('; }
accel_present() { $K showloaded 2>/dev/null | grep -q 'org\.watchthelight\.XeGfxAccel ('; }

# a REAL logged-in user, not loginwindow's placeholders.  On macOS 26 /dev/console is
# owned by _windowserver while the login window is up, so "!= root" is not enough.
real_user() {
  local u uid
  u=$(stat -f %Su /dev/console 2>/dev/null)
  case "$u" in
    ""|root|loginwindow|_windowserver|_mbsetupuser|_unknown) return 1 ;;
  esac
  uid=$(id -u "$u" 2>/dev/null) || return 1
  [ "${uid:-0}" -ge 500 ] || return 1
  echo "$u"
}

WRITE "=== bringup start (boot=${BOOT:-?}) ==="

# 1) wait for a real console user (up to 15 min; the login window period is skipped)
u=""
for i in $(seq 1 900); do
  u=$(real_user) && { WRITE "console user=$u (uid $(id -u "$u")) after ${i}s"; break; }
  u=""
  sleep 1
done
[ -n "$u" ] || { WRITE "no real console user after 900s -- abort"; exit 1; }

# 1b) the GUI session must exist, and WindowServer must be up
GUID=$(id -u "$u")
for i in $(seq 1 90); do
  if launchctl print "gui/$GUID" >/dev/null 2>&1 && pgrep -x WindowServer >/dev/null 2>&1; then
    WRITE "GUI session + WindowServer up after ${i}s"; break
  fi
  sleep 1
done
pgrep -x WindowServer >/dev/null 2>&1 || { WRITE "no WindowServer -- abort"; exit 1; }

# 2) let the desktop finish settling (WindowServer first paint)
sleep 25

# 3) core -- verified by the stage sysctl appearing, not by kmutil output
if ! core_present; then
  for i in $(seq 1 20); do
    $K load -b org.watchthelight.XeGfx >/dev/null 2>&1
    if [ -n "$($S -n debug.xegfx_stage 2>/dev/null)" ]; then
      WRITE "core loaded (debug.xegfx_stage present, attempt $i)"; break
    fi
    sleep 5
  done
else
  WRITE "core already loaded"
fi
[ -n "$($S -n debug.xegfx_stage 2>/dev/null)" ] || { WRITE "load core FAILED (no debug.xegfx_stage) -- abort"; exit 2; }

# 4) stage 7
$S -w debug.xegfx_wdog=60 >/dev/null 2>&1
$S -w debug.xegfx_stage=7 >/dev/null 2>&1
results() { $S -b debug.xegfx_results 2>/dev/null | od -A n -t d4 | tr -s ' \n' ' ' | sed 's/^ *//;s/ *$//'; }
r=""
for i in $(seq 1 40); do
  r=$(results)
  [ "$r" = "0 1 1 1 1 1 1 1" ] && { WRITE "stages ok: $r (after ${i}s)"; break; }
  sleep 1
done
r=$(results)
[ "$r" = "0 1 1 1 1 1 1 1" ] || { WRITE "stages NOT ok: $r -- abort"; exit 3; }

# 5) accel -- verified by the NMXAccel instance appearing
if ! accel_present; then
  for i in $(seq 1 10); do
    $K load -b org.watchthelight.XeGfxAccel >/dev/null 2>&1
    if ioreg -c NMXAccel -l 2>/dev/null | grep -q MetalPluginName; then
      WRITE "accel loaded (NMXAccel present, attempt $i)"; break
    fi
    sleep 3
  done
else
  WRITE "accel already loaded"
fi
[ -n "$($S -n debug.xegfxaccel_iop_ok 2>/dev/null)" ] || { WRITE "load accel FAILED (no iop_ok) -- abort"; exit 4; }
sleep 4

# 5b) idempotency guard.  debug.xegfxaccel_iop is a one-way per-boot latch that can only
#     reach 1 after the scanout pipe is open (step 6 precedes step 7), and re-writing it
#     returns EPERM.  So iop==1 proves both remaining steps are already done.
#     Re-applying accelfb=3 is NOT free: measured 2026-11-01 09:12/09:15, it re-delivers
#     the mode change and stalls iop_ok for 1-2 minutes (a real picture hiccup).
need=1
if [ "$($S -n debug.xegfxaccel_iop 2>/dev/null)" = "1" ]; then
  need=0
  WRITE "already live (debug.xegfxaccel_iop=1) -- skipping pipe + hooks, not re-delivering the mode change"
fi

# bootArm (xegfxaccelboot=1) runs steps 4..6 itself, including the K9b deferred zero-copy flip.
# Writing accelfb=3 a second time stalls the transaction counter for 1-2 minutes (A-44), so stand aside.
BA=$(/usr/sbin/sysctl -n debug.xegfxaccel_boot 2>/dev/null || echo 0)
if [ "${BA:-0}" != "0" ]; then
  need=0
  WRITE "bootArm is driving (debug.xegfxaccel_boot=$BA) -- not touching accelfb/iop, waiting for its steps"
  for i in $(seq 1 40); do
    ST=$(/usr/sbin/sysctl -n debug.xegfxaccel_boot_steps 2>/dev/null || echo 0)
    [ $(( ST & 32 )) -ne 0 ] && break
    sleep 3
  done
  WRITE "boot_steps=$ST (bit32 iop, bit64 flip)"
fi

# 6) scanout pipe  (how>=3 -- writing 1 only arms, it does NOT open the pipe)
if [ "$need" = 1 ]; then
  $S -w debug.xegfxaccelfb=3 >/dev/null 2>&1
  WRITE "accelfb=3 written (read-back $($S -n debug.xegfxaccelfb 2>/dev/null))"
  sleep 3
  if /usr/bin/log show --last 60s --predicate 'eventMessage CONTAINS "scanout-backed display pipe ENABLED"' 2>/dev/null | grep -q ENABLED; then
    WRITE "scanout-backed display pipe ENABLED seen"
  else
    WRITE "WARN: pipe-ENABLED line not seen in the last 60s (continuing)"
  fi

  # 7) transaction hooks -- this is the step that makes frames reach the panel
  $S -w debug.xegfxaccel_iop=1 >/dev/null 2>&1
  sleep 3
fi

# 8) verify.  A freshly re-delivered mode change can stall the counter, and the desktop is
#    idle right after login (can be <1 fps), so sample for up to ~21 s first.
#    Cold-boot fact (measured 2026-11-01 09:26 on a fresh boot): WindowServer starts BEFORE
#    the pipe exists, keeps a stale display registration and produces ZERO transactions
#    (iop_ok stayed 0 for 120 s).  Restarting it once made iop_ok jump 0 -> 169 in seconds.
#    So: if nothing moves, restart WindowServer exactly once and re-check.
probe() {   # $1 = number of 3 s samples; sets PR_OK
  # A-84: with the zero-copy flip as the boot default a frame advances iop_xe_flips instead of iop_ok, so a probe
  # that only watched iop_ok would "see no frames" on every boot and restart WindowServer for nothing.
  local n=$1 i a b fa fb
  a=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null); fa=$($S -n debug.xegfxaccel_iop_xe_flips 2>/dev/null)
  PR_OK=0
  for i in $(seq 1 "$n"); do
    sleep 3
    b=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null); fb=$($S -n debug.xegfxaccel_iop_xe_flips 2>/dev/null)
    if [ "${b:-0}" -gt "${a:-0}" ] || [ "${fb:-0}" -gt "${fa:-0}" ]; then
      WRITE "  sample ${i}: iop_ok ${a:-0} -> ${b:-0}, iop_xe_flips ${fa:-0} -> ${fb:-0}"; PR_OK=1; return 0
    fi
  done
  return 1
}

caffeinate -u -t 3 >/dev/null 2>&1 &
if [ "$need" = 1 ]; then
  if ! probe 7; then
    WRITE "no frames yet -- restarting WindowServer so it re-registers against the open pipe"
    launchctl kickstart -k system/com.apple.WindowServer >/dev/null 2>&1
    sleep 10
    pgrep -x WindowServer >/dev/null 2>&1 && WRITE "WindowServer back (pid $(pgrep -x WindowServer))" \
      || WRITE "WARN: WindowServer did not come back"
    probe 40
  fi
else
  # bootArm armed the pipe itself, but WindowServer started before it existed and keeps a
  # stale display registration -- iop_ok stays 0 until it is restarted once (A-44 conclusion 6,
  # seen again on 2026-11-01 18:29).  Same step as the manual branch above.
  if ! probe 7; then
    WRITE "bootArm armed but no frames yet -- restarting WindowServer once"
    launchctl kickstart -k system/com.apple.WindowServer >/dev/null 2>&1
    sleep 10
    pgrep -x WindowServer >/dev/null 2>&1 && WRITE "WindowServer back (pid $(pgrep -x WindowServer))"       || WRITE "WARN: WindowServer did not come back"
    probe 40
  fi
fi

WRITE "iop=$($S -n debug.xegfxaccel_iop 2>/dev/null)  iop_ok=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null) (fail=$($S -n debug.xegfxaccel_iop_fail 2>/dev/null) empty=$($S -n debug.xegfxaccel_iop_empty 2>/dev/null))"
# A-84 P1 double insurance: flip/defer are the accel's boot default now.  If an older binary, or a hand-set 0, left
# them off, turn them on once and say so -- the A-79 self-heal chain is what makes that safe to do unattended.
FLIP=$($S -n debug.xegfxaccel_iop_xe_flip 2>/dev/null || echo -1)
DEFER=$($S -n debug.xegfxaccel_iop_xe_defer 2>/dev/null || echo -1)
if [ "$FLIP" = "0" ] && [ "$DEFER" = "0" ]; then
  $S -w debug.xegfxaccel_iop_xe_flip=1 debug.xegfxaccel_iop_xe_defer=1 >/dev/null 2>&1
  WRITE "A-84: iop_xe_flip/defer were 0 -- set both to 1 (zero-copy flip on, copy path was the old safe mode)"
elif [ "$FLIP" != "1" ] || [ "$DEFER" != "1" ]; then
  WRITE "A-84: flip/defer are flip=$FLIP defer=$DEFER -- left as they are (one of them is deliberately off)"
else
  WRITE "A-84: flip/defer already on (boot default)"
fi
# Zero-copy: frames can reach the panel by flipping the plane at the compositor surface instead of
# copying, and then iop_ok stays 0 by design.  Count a moving flip counter as success too.
if [ "${PR_OK:-0}" != 1 ]; then
  FL=$($S -n debug.xegfxaccel_iop_xe_flips 2>/dev/null || echo 0)
  if [ "${FL:-0}" != "0" ]; then
    PR_OK=1; WRITE "frames are flipped onto the panel (zero-copy): iop_xe_flips=$FL, iop_ok=0"
  fi
fi
if [ "${PR_OK:-0}" = 1 ]; then WRITE "OK: frames are reaching the panel"; exit 0; fi
WRITE "WARN: iop_ok did not advance (panel keeps the firmware frame)"
exit 5
=== WC ===
     215 /usr/local/bin/xegfx-bringup.sh
32f9db924b8062868bde8154c393ca6ecb0691ad3d9b615f1f947c5d6e6572fa  /usr/local/bin/xegfx-bringup.sh
