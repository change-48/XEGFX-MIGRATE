#!/bin/bash
# XeGfx bring-up: core -> stage 7 -> accel -> scanout pipe -> transaction hooks.
# Runs AFTER a real GUI login and after the desktop has settled (A-25b: doing this at
# boot/loginwindow made WindowServer assert).  Every step is verified by an observable;
# on failure it stops and leaves the machine on the firmware framebuffer (safe state).
set -u
export PATH=/usr/bin:/bin:/usr/sbin:/sbin

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
  local n=$1 i a b
  a=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null)
  PR_OK=0
  for i in $(seq 1 "$n"); do
    sleep 3
    b=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null)
    if [ "${b:-0}" -gt "${a:-0}" ]; then WRITE "  sample ${i}: iop_ok $a -> $b"; PR_OK=1; return 0; fi
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
  probe 40
fi

WRITE "iop=$($S -n debug.xegfxaccel_iop 2>/dev/null)  iop_ok=$($S -n debug.xegfxaccel_iop_ok 2>/dev/null) (fail=$($S -n debug.xegfxaccel_iop_fail 2>/dev/null) empty=$($S -n debug.xegfxaccel_iop_empty 2>/dev/null))"
if [ "${PR_OK:-0}" = 1 ]; then WRITE "OK: frames are reaching the panel"; exit 0; fi
WRITE "WARN: iop_ok did not advance (panel keeps the firmware frame)"
exit 5
