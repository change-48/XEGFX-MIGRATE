#include <stdio.h>
#include <CoreGraphics/CoreGraphics.h>

int main(void) {
    CFArrayRef l = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!l) { printf("nil list\n"); return 1; }
    long n = CFArrayGetCount(l);
    printf("windows=%ld\n", n);
    for (long i = 0; i < n; i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(l, i);
        CFStringRef o = CFDictionaryGetValue(d, kCGWindowOwnerName);
        char b[128] = "?";
        if (o) CFStringGetCString(o, b, sizeof b, kCFStringEncodingUTF8);
        CFNumberRef la = CFDictionaryGetValue(d, kCGWindowLayer);
        CFNumberRef pn = CFDictionaryGetValue(d, kCGWindowOwnerPID);
        CFNumberRef wn = CFDictionaryGetValue(d, kCGWindowNumber);
        int lv = -999, pid = -1, wid = -1;
        if (la) CFNumberGetValue(la, kCFNumberIntType, &lv);
        if (pn) CFNumberGetValue(pn, kCFNumberIntType, &pid);
        if (wn) CFNumberGetValue(wn, kCFNumberIntType, &wid);
        CGRect r = CGRectZero;
        CFDictionaryRef bd = CFDictionaryGetValue(d, kCGWindowBounds);
        if (bd) CGRectMakeWithDictionaryRepresentation(bd, &r);
        printf("  wid=%d pid=%d layer=%d %.0fx%.0f owner=%s\n", wid, pid, lv, r.size.width, r.size.height, b);
    }
    return 0;
}
