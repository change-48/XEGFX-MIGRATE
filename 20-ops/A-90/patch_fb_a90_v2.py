#!/usr/bin/env python3
"""A-90 A1 (v2): the sysctl never showed up (no sysctl set registered from this kext), so the notification is
fired by two delayed thread calls from the framebuffer's own start() instead: +70 s and +95 s after start,
which is the window in which bootArm has opened the display pipe and WindowServer is still holding a stale
registration.  Mask comes from a boot-arg, so a different variant needs no rebuild: -xifbnotify=<mask>.
"""
import os, sys

SRC = os.path.expanduser("~/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8/kexts/XeGfxIntelFB/intelfb.cpp")
s = open(SRC, encoding="utf-8").read()

def sub_once(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        print("ANCHOR FAIL (%s): %d hits" % (label, n)); sys.exit(2)
    s = s.replace(old, new, 1)

# 1) drop the sysctl (it never registered) ------------------------------------------------
i = s.find("static int xifbNotify_sysctl SYSCTL_HANDLER_ARGS")
if i >= 0:
    j = s.find('"A-90: replay display notifications', i)
    j = s.find(");\n", j) + 3
    s = s[:i] + s[j:]
    print("removed sysctl block")

# 2) replace the static enter() with a boot-arg driven timer --------------------------------
old_enter = '''    static void xifbNotifyEnter(uint32_t mask) {
        if (!sNotifyTc) sNotifyTc = thread_call_allocate(&XeGfxIntelFramebuffer::xifbNotifyThunk, nullptr);
        if (!sNotifyTc) { XIFBLOG("A-90 notify: thread_call_allocate failed"); return; }
        thread_call_enter1(sNotifyTc, (thread_call_param_t)(uintptr_t)mask);
    }
'''
new_enter = '''    static void xifbNotifyArm(uint64_t seconds, uint32_t mask) {
        thread_call_t tc = thread_call_allocate(&XeGfxIntelFramebuffer::xifbNotifyThunk, nullptr);
        if (!tc) { XIFBLOG("A-90 notify: thread_call_allocate failed"); return; }
        uint64_t dl = 0;
        clock_interval_to_deadline(seconds, kSecondScale, &dl);
        thread_call_enter1_delayed(tc, (thread_call_param_t)(uintptr_t)mask, dl);
        XIFBLOG("A-90 notify: armed at +%llu s (mask %#x)", seconds, (unsigned)mask);
    }
'''
sub_once(old_enter, new_enter, "enter -> arm")

# 3) fire it from start(), after the base class start returned ------------------------------
anchor = '''            if (setProperty("XeGfxL0Ready", kOSBooleanTrue))'''
sub_once(anchor, '''            // A-90: replay the display notifications once the accelerator has its pipe.  Default mask 3 = online
            // change + display-mode-did-change; -xifbnotify=<mask> picks another one (4 wake, 8 power on, 15 all).
            uint32_t nmask = 3, narg = 0;
            if (PE_parse_boot_argn("xifbnotify", &narg, sizeof narg)) nmask = (uint32_t)narg;
            if (nmask) { xifbNotifyArm(70, nmask); xifbNotifyArm(95, nmask); }
''' + anchor, "L0Ready anchor")

open(SRC, "w", encoding="utf-8", newline="\n").write(s)
print("PATCHED v2", len(s), "bytes")
