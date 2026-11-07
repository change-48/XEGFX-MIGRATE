#include <stdio.h>
#include <CoreGraphics/CoreGraphics.h>

int main(void) {
    CFArrayRef l = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    printf("windows=%ld\n", l ? (long)CFArrayGetCount(l) : -1L);
    if (l) {
        long n = CFArrayGetCount(l);
        for (long i = 0; i < n && i < 15; i++) {
            CFDictionaryRef d = CFArrayGetValueAtIndex(l, i);
            CFStringRef o = CFDictionaryGetValue(d, kCGWindowOwnerName);
            char b[128] = "?";
            if (o) CFStringGetCString(o, b, sizeof b, kCFStringEncodingUTF8);
            CFNumberRef la = CFDictionaryGetValue(d, kCGWindowLayer);
            int lv = -1;
            if (la) CFNumberGetValue(la, kCFNumberIntType, &lv);
            printf("  %s layer=%d\n", b, lv);
        }
        CFRelease(l);
    }
    return 0;
}
