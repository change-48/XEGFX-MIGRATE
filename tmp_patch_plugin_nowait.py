import sys
p = sys.argv[1]
s = open(p).read()

if "nvmtl_ws_no_wait" in s:
    raise SystemExit("ALREADY PATCHED")

anchor = "- (BOOL)commitAndWaitUntilSubmitted {"
if anchor not in s:
    raise SystemExit("METHOD ANCHOR NOT FOUND")

helper = r'''// A-54: let WindowServer hand each frame over without waiting for the GPU.  The accelerator's deferred
// flip worker orders it instead -- it arms PLANE_SURF only once the GPU reports idle -- so the frame that
// reaches the panel is always the finished one.  Without this the compositor waits for the GPU and the
// worker then waits for it again: two serialisation points per frame, which is why the zero-copy path ran
// at 19 fps against the 56 fps of the CPU copy it replaced.  Off unless the sentinel exists or
// NVMTL_WS_NO_WAIT=1; re-read every 64 calls so it can be switched while everything keeps running.
static int nvmtl_ws_no_wait(void)
{
    static int on = -1; static unsigned calls;
    if (on < 0 || (++calls & 0x3fu) == 0) {
        int v = 0;
        const char *e = getenv("NVMTL_WS_NO_WAIT");
        if (e && e[0] == '1') v = 1;
        else { FILE *f = fopen("/private/tmp/nvmtl-ws-nowait", "r"); if (f) { v = 1; fclose(f); } }
        if (on >= 0 && v != on) nvlog("cw: WindowServer no-wait %s", v ? "ON (the accel orders the flip)" : "off");
        on = v;
    }
    return on;
}
'''

s = s.replace(anchor, helper + anchor, 1)

old_cond = "    if (ws && !nvmtl_ws_kernel_waits()) [self waitUntilCompleted]; else [self waitUntilScheduled];"
if old_cond not in s:
    raise SystemExit("COND ANCHOR NOT FOUND")
new_cond = "    if (ws && !nvmtl_ws_kernel_waits() && !nvmtl_ws_no_wait()) [self waitUntilCompleted]; else [self waitUntilScheduled];"
s = s.replace(old_cond, new_cond, 1)

open(p, "w").write(s)
print("patched ok")
