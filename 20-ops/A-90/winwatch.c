// winwatch: poll CGWindowList until a window owned by <owner> appears on screen.
// usage: winwatch <ownerName> [timeoutSeconds]
// prints: "<ms since start> <owner> <w>x<h>" once, then exits 0; exit 1 on timeout.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: winwatch <owner> [timeout_s]\n"); return 2; }
    const char *want = argv[1];
    double timeout = (argc > 2) ? atof(argv[2]) : 60.0;
    double t0 = now_ms();
    while ((now_ms() - t0) < timeout * 1000.0) {
        CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
        if (list) {
            CFIndex n = CFArrayGetCount(list);
            for (CFIndex i = 0; i < n; i++) {
                CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
                CFStringRef owner = CFDictionaryGetValue(d, kCGWindowOwnerName);
                CFNumberRef layer = CFDictionaryGetValue(d, kCGWindowLayer);
                int lv = -1; if (layer) CFNumberGetValue(layer, kCFNumberIntType, &lv);
                if (owner && CFStringGetCStringPtr(owner, kCFStringEncodingUTF8)) {
                    char buf[256];
                    CFStringGetCString(owner, buf, sizeof buf, kCFStringEncodingUTF8);
                    if (strcmp(buf, want) == 0 && lv == 0) {
                        CGRect r = CGRectZero;
                        CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
                        if (b) CGRectMakeWithDictionaryRepresentation(b, &r);
                        printf("%.0f %s %.0fx%.0f\n", now_ms() - t0, buf, r.size.width, r.size.height);
                        fflush(stdout);
                        CFRelease(list);
                        return 0;
                    }
                }
            }
            CFRelease(list);
        }
        usleep(30000); // 30 ms
    }
    printf("TIMEOUT %s\n", want);
    return 1;
}
