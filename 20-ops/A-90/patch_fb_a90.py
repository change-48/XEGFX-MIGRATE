#!/usr/bin/env python3
"""A-90 A1: add a debug.xegfxfb_notify sysctl to XeGfxIntelFB that replays the IOFramebuffer display
notifications (online change / mode change / wake) so CoreDisplay + WindowServer re-read the display after
the accelerator has opened a pipe.  Nothing here touches hardware: it is the notification only.
Anchors are asserted: if the source has moved, the patch refuses instead of writing half of it.
"""
import os, shutil, sys, time

SRC = os.path.expanduser("~/src/NvGfx-5a19a73f36249efb99c44c3f9cb34603ef0c0eb8/kexts/XeGfxIntelFB/intelfb.cpp")
BK = os.path.expanduser("~/xegfx-log/A-90/backup")
os.makedirs(BK, exist_ok=True)
bak = os.path.join(BK, "intelfb.cpp.before-a90")
if not os.path.exists(bak):
    shutil.copy2(SRC, bak)
s = open(SRC, encoding="utf-8").read()

if "xegfxfb_notify" in s:
    print("ALREADY PATCHED"); sys.exit(0)

def sub_once(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        print("ANCHOR FAIL (%s): %d hits" % (label, n)); sys.exit(2)
    s = s.replace(old, new, 1)

# 1) sysctl header
sub_once('#include <pexpert/pexpert.h>',
         '#include <pexpert/pexpert.h>\n#include <sys/sysctl.h>', "include")

# 2) class member + methods, just before start()
anchor = "    bool start(IOService *provider) override {"
block = '''    // ---- A-90 (problem A): replay the display notifications, no hardware access ----
public:
    static XeGfxIntelFramebuffer *sSelf;
    static thread_call_t sNotifyTc;
    // CoreDisplay found no display pipe for this IOFB at boot ("Unable to find display pipe for IOFB service")
    // because WindowServer enumerates displays ~15 s in, while the accelerator only opens the pipe ~60 s in, and
    // it never asks again.  These are the same notifications IOFramebuffer publishes on a hotplug or a wake
    // (see IOFramebuffer.h: kIOFBNotifyOnlineChange / kIOFBNotifyDisplayModeChange / kIOFBNotifyDidWake), which is
    // what makes the compositor re-read the display.  mask: 1 online, 2 mode change, 4 wake, 8 power on.
    void xifbNotifyDeliver(uint32_t mask) {
        IOReturn rOn = kIOReturnUnsupported, rMode = kIOReturnUnsupported, rWake = kIOReturnUnsupported;
        if (mask & 1u) rOn = deliverFramebufferNotification(kIOFBNotifyOnlineChange, (void *)true);
        if (mask & 2u) rMode = deliverFramebufferNotification(kIOFBNotifyDisplayModeChange, (void *)(uintptr_t)fCurrentMode);
        if (mask & 4u) {
            rWake = deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)true);
            deliverFramebufferNotification(kIOFBNotifyDidWake, (void *)false);
        }
        if (mask & 8u) handleEvent(kIOFBNotifyDidPowerOn);
        XIFBLOG("A-90 notify: mask %#x -> online %#x, modechange %#x, wake %#x (mode %#x)",
                (unsigned)mask, (unsigned)rOn, (unsigned)rMode, (unsigned)rWake, (unsigned)fCurrentMode);
    }
    static void xifbNotifyThunk(thread_call_param_t p0, thread_call_param_t) {
        if (sSelf) sSelf->xifbNotifyDeliver((uint32_t)(uintptr_t)p0);
    }
    static void xifbNotifyEnter(uint32_t mask) {
        if (!sNotifyTc) sNotifyTc = thread_call_allocate(&XeGfxIntelFramebuffer::xifbNotifyThunk, nullptr);
        if (!sNotifyTc) { XIFBLOG("A-90 notify: thread_call_allocate failed"); return; }
        thread_call_enter1(sNotifyTc, (thread_call_param_t)(uintptr_t)mask);
    }

'''
sub_once(anchor, block + anchor, "start anchor")

# 3) remember the instance
sub_once("    bool start(IOService *provider) override {\n        xifbReadBootArgs();",
         "    bool start(IOService *provider) override {\n        sSelf = this;\n        xifbReadBootArgs();",
         "start body")
sub_once("    void stop(IOService *provider) override {\n",
         "    void stop(IOService *provider) override {\n        if (sSelf == this) sSelf = nullptr;\n",
         "stop body")

# 4) statics + the sysctl, after the metaclass declaration
tail = "OSDefineMetaClassAndStructors(XeGfxIntelFramebuffer, IONDRVFramebuffer)\n"
extra = tail + '''
XeGfxIntelFramebuffer *XeGfxIntelFramebuffer::sSelf = nullptr;
thread_call_t XeGfxIntelFramebuffer::sNotifyTc = nullptr;

static int xifbNotify_sysctl SYSCTL_HANDLER_ARGS
{
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v & 15) XeGfxIntelFramebuffer::xifbNotifyEnter((uint32_t)v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, xegfxfb_notify, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
            xifbNotify_sysctl, "I",
            "A-90: replay display notifications (1 online, 2 mode change, 4 wake, 8 power-on)");
'''
sub_once(tail, extra, "metaclass tail")

open(SRC, "w", encoding="utf-8", newline="\n").write(s)
print("PATCHED", SRC, len(s), "bytes; backup", bak)
