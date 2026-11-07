
    nvAccelIopHome(fb, head);
    nvAccelIopWaitVblank();
    const unsigned int surfID = s ? (unsigned)s->getSurfaceID() : 0u;
    const unsigned int frameNow = gDispMmio ? gDispMmio[NV_DISP_FRAME_COUNTER / 4] : 0u;
    if (gIopLastSurf && surfID == gIopLastSurf) OSAddAtomic64(1, &gCntIopSurfRepeat);
    if (gIopLastFrameAtCopy && frameNow == gIopLastFrameAtCopy) OSAddAtomic64(1, &gCntIopSameRefresh);
    gIopLastSurf = surfID;
    gIopLastFrameAtCopy = frameNow;
    const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    unsigned preSample[4] = {};
    if (src) {
        preSample[0] = src[0];
        preSample[1] = src[(unsigned long)(h / 2) * spr + (spr / 2)];
        preSample[2] = src[(unsigned long)(h - 1) * spr + (spr - 1)];
        preSample[3] = src[(unsigned long)(h / 4) * spr];
    }
    const uint64_t copyStart = mach_absolute_time();
    if (gIopMemcpy) {
        if (q.pitch == pitch) {
            memcpy((void *)q.kva, (const void *)src, (size_t)h * (size_t)pitch);
        } else {
            for (unsigned y = 0; y < h; y++)
                memcpy((char *)q.kva + (unsigned long)y * q.pitch,
                       (const char *)src + (unsigned long)y * pitch, (size_t)words * 4);
        }
    } else {
        volatile unsigned int *d = (volatile unsigned int *)q.kva;
        for (unsigned y = 0; y < h; y++) {
            const volatile unsigned int *sp = src + (unsigned long)y * spr;
            volatile unsigned int *dp = d + (unsigned long)y * dpr;
            for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
        }
    }
#ifdef NMX_XEGFX
    if (nvXeIsKmsFB(fb) && nvXeRestoreCopyTarget(fb) != kIOReturnSuccess) {
        OSAddAtomic64(1, &gCntIopLate);
        return kIOReturnSuccess; // Keep the prior frame, without claiming a displayed copy.
    }
#endif
    nvIopStat(&gIopCopyLastUs, &gIopCopyMaxUs, &gIopCopySumUs, nvIopUsBetween(copyStart, mach_absolute_time()));
    if (gIopCopyLastUs > 12000) OSAddAtomic64(1, &gCntIopCopyOverrun);
