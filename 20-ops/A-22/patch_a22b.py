#!/usr/bin/env python3
# A-22b: mode ID must be the one IOFB published (100), not NMIntelFB's synthetic 1 -- CoreDisplay indexes its mode
# list with it and asserts on an unknown ID (WindowServer-2026-10-31-165956 build_mode_list_if_needed).
import io

P = "kexts/XeGfxIntelFB/intelfb.cpp"
src = io.open(P, encoding="utf-8").read()

start = src.index("    // The accelerator asks these while wiring the display pipe")
end = src.index("        return kIOReturnSuccess;\n    }\n", src.index("getPixelInformation(", start)) + len("        return kIOReturnSuccess;\n    }\n")
old = src[start:end]
assert "getCurrentDisplayMode" in old and "getPixelInformation" in old, old[:200]

new = '''    // The accelerator asks these while wiring the display pipe; IONDRVFramebuffer would forward them to an NDRV
    // instance this machine does not have, so answer from the raster we measured. The mode ID must be the one the base
    // class published (100 here): CoreDisplay uses it as an index into its mode list and aborts on an unknown ID.
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    {
        if (!mode || !depth) return kIOReturnBadArgument;
        if (kIOReturnSuccess == IONDRVFramebuffer::getCurrentDisplayMode(mode, depth)) return kIOReturnSuccess;
        // no NDRV instance answers: the first mode of the list we published is the firmware's raster
        IODisplayModeID m = 0;
        if (getDisplayModeCount() && kIOReturnSuccess == getDisplayModes(&m)) { *mode = m; *depth = 0; return kIOReturnSuccess; }
        XIFBLOG("getCurrentDisplayMode: no mode list to answer from");
        return kIOReturnUnsupported;
    }

    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override
    { return getCurrentDisplayMode(mode, depth); }

    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
                                 IOPixelAperture aperture, IOPixelInformation *pi) override
    {
        if (!pi || aperture != kIOFBSystemAperture) return kIOReturnUnsupportedMode;
        if (!fW || !fH || !fRowBytes) return kIOReturnUnsupported;      // not measured: refuse honestly
        IODisplayModeInformation mi = {};
        if (kIOReturnSuccess != IONDRVFramebuffer::getInformationForDisplayMode(mode, &mi)) return kIOReturnUnsupportedMode;
        bzero(pi, sizeof(*pi));
        pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32;
        pi->pixelType = kIORGBDirectPixels;
        pi->componentCount = 3; pi->bitsPerComponent = 8;
        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
        pi->activeWidth = fW; pi->activeHeight = fH;
        static unsigned logged;
        if (logged++ < 4) XIFBLOG("scanout described: mode %#x depth %u %ux%u rowBytes %u",
                                  (unsigned)mode, (unsigned)depth, fW, fH, fRowBytes);
        return kIOReturnSuccess;
    }
'''

src = src[:start] + new + src[end:]
io.open(P, "w", encoding="utf-8").write(src)
print("rewritten: %d -> %d bytes" % (len(old), len(new)))
