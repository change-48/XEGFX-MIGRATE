// winwatch2: wait for a layer-0 window from <pid|owner-name>; print its epoch time (ms).
// usage: winwatch2 <pid|owner> [timeout_s]
// output: "WIN <epoch_ms> <name> <w>x<h>"  |  "TIMEOUT <target>"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

static double epoch_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: winwatch2 <pid|owner> [timeout_s]\n"); return 2; }
    const char *want = argv[1];
    int wantPid = atoi(want);
    double timeout = (argc > 2) ? atof(argv[2]) : 60.0;
    double t0 = epoch_ms();
    while ((epoch_ms() - t0) < timeout * 1000.0) {
        CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        if (list) {
            CFIndex n = CFArrayGetCount(list);
            for (CFIndex i = 0; i < n; i++) {
                CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
                CFNumberRef layer = CFDictionaryGetValue(d, kCGWindowLayer);
                int lv = -1; if (layer) CFNumberGetValue(layer, kCFNumberIntType, &lv);
                if (lv != 0) continue;
                CFStringRef owner = CFDictionaryGetValue(d, kCGWindowOwnerName);
                CFNumberRef pidn = CFDictionaryGetValue(d, kCGWindowOwnerPID);
                int pid = -1; if (pidn) CFNumberGetValue(pidn, kCFNumberIntType, &pid);
                int hit = 0;
                char buf[256] = "?";
                if (owner) CFStringGetCString(owner, buf, sizeof buf, kCFStringEncodingUTF8);
                if (wantPid > 0 && pid == wantPid) hit = 1;
                else if (wantPid <= 0 && strcmp(buf, want) == 0) hit = 1;
                if (hit) {
                    CGRect r = CGRectZero;
                    CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
                    if (b) CGRectMakeWithDictionaryRepresentation(b, &r);
                    printf("WIN %.0f %s %.0fx%.0f\n", epoch_ms(), buf, r.size.width, r.size.height);
                    fflush(stdout);
                    CFRelease(list);
                    return 0;
                }
            }
            CFRelease(list);
        }
        usleep(20000);
    }
    printf("TIMEOUT %s\n", want);
    return 1;
}
