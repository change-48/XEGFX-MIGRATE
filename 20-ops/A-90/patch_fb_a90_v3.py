#!/usr/bin/env python3
"""A-90 A1 (v3): the +70/+95 s nudges never ran, and the only difference to the guard's own timer that does run
(120 s, "boot guard cleared") is thread_call_enter1_delayed vs thread_call_enter_delayed.  So: one global mask,
plain thread_call_enter_delayed, and the log line goes out BEFORE the deliveries, so "fired" and "delivered"
are distinguishable in the log.
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

# log before delivering, so a blocked delivery is still visible
sub_once('''        IOReturn rOn = kIOReturnUnsupported, rMode = kIOReturnUnsupported, rWake = kIOReturnUnsupported;''',
         '''        XIFBLOG("A-90 notify: delivering mask %#x (mode %#x) to the clients", (unsigned)mask, (unsigned)fCurrentMode);
        IOReturn rOn = kIOReturnUnsupported, rMode = kIOReturnUnsupported, rWake = kIOReturnUnsupported;''',
         "log first")

# thunk reads a global mask, and is armed with the same call the guard uses
sub_once('''    static void xifbNotifyThunk(thread_call_param_t p0, thread_call_param_t) {
        if (sSelf) sSelf->xifbNotifyDeliver((uint32_t)(uintptr_t)p0);
    }''',
         '''    static uint32_t sNotifyMask;
    static void xifbNotifyThunk(thread_call_param_t, thread_call_param_t) {
        if (!sSelf) { XIFBLOG("A-90 notify: no framebuffer instance at the nudge"); return; }
        sSelf->xifbNotifyDeliver(sNotifyMask);
    }''',
         "thunk")

sub_once('''        uint64_t dl = 0;
        clock_interval_to_deadline(seconds, kSecondScale, &dl);
        thread_call_enter1_delayed(tc, (thread_call_param_t)(uintptr_t)mask, dl);''',
         '''        uint64_t dl = 0;
        sNotifyMask = mask;
        clock_interval_to_deadline(seconds, kSecondScale, &dl);
        thread_call_enter_delayed(tc, dl);''',
         "enter")

# static definition
sub_once('thread_call_t XeGfxIntelFramebuffer::sNotifyTc = nullptr;',
         'thread_call_t XeGfxIntelFramebuffer::sNotifyTc = nullptr;\nuint32_t XeGfxIntelFramebuffer::sNotifyMask = 0;',
         "static def")

open(SRC, "w", encoding="utf-8", newline="\n").write(s)
print("PATCHED v3", len(s), "bytes")
