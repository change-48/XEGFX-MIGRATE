#!/usr/bin/env python3
# A-22: make XeGfxIntelFramebuffer describe its own scanout (getPixelInformation / getCurrentDisplayMode)
import io, sys

P = "kexts/XeGfxIntelFB/intelfb.cpp"
src = io.open(P, encoding="utf-8").read()
orig = src

# ---- 1. three members -------------------------------------------------------------------------------
anchor1 = "    uint32_t fDimSurf = 0;\n"
add1 = anchor1 + ("    // the raster we publish to the accelerator; PLANE_SIZE gives it, PLANE_STRIDE (64 B units) the pitch\n"
                  "    uint32_t fW = 0, fH = 0, fRowBytes = 0;\n")
if "uint32_t fW = 0, fH = 0, fRowBytes = 0;" not in src:
    assert src.count(anchor1) == 1, "anchor1"
    src = src.replace(anchor1, add1)

# ---- 2. fill them where PLANE_SIZE is read ----------------------------------------------------------
anchor2 = "        const uint32_t w = (size & 0xffff) + 1, h = (size >> 16) + 1;\n"
add2 = anchor2 + (
    "        // exactly what the plane is scanning: width/height from PLANE_SIZE, pitch from PLANE_STRIDE (64 B)\n"
    "        fW = w; fH = h; fRowBytes = (r[0x70188 / 4] & 0xffff) * 64u;\n"
    "        if (!fRowBytes) fRowBytes = w * 4;\n"
    "        XIFBLOG(\"scanout geometry for accel: %ux%u rowBytes %u\", fW, fH, fRowBytes);\n")
if "scanout geometry for accel" not in src:
    assert src.count(anchor2) == 1, "anchor2"
    src = src.replace(anchor2, add2)

# ---- 3. the three methods, next to getAttributeForConnection -----------------------------------------
anchor3 = ("    IOReturn getAttributeForConnection(IOIndex ci, IOSelect attr, uintptr_t *value) override {\n"
           "        if (fEdidLen && attr == kConnectionSupportsHLDDCSense) return kIOReturnSuccess;\n"
           "        return IONDRVFramebuffer::getAttributeForConnection(ci, attr, value);\n"
           "    }\n")
add3 = anchor3 + (
    "    // The accelerator asks these while wiring the display pipe; IONDRVFramebuffer would forward them to an NDRV\n"
    "    // instance this machine does not have, so answer from the raster we measured. Same shape as NMIntelFB.\n"
    "    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override\n"
    "    { *mode = 1; *depth = 0; return kIOReturnSuccess; }\n"
    "\n"
    "    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override\n"
    "    { return getCurrentDisplayMode(mode, depth); }\n"
    "\n"
    "    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,\n"
    "                                 IOPixelAperture aperture, IOPixelInformation *pi) override\n"
    "    {\n"
    "        if (mode != 1 || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;\n"
    "        if (!fW || !fH || !fRowBytes) return kIOReturnUnsupported;      // not measured: refuse honestly\n"
    "        bzero(pi, sizeof(*pi));\n"
    "        pi->bytesPerRow = fRowBytes; pi->bytesPerPlane = 0; pi->bitsPerPixel = 32;\n"
    "        pi->pixelType = kIORGBDirectPixels;\n"
    "        pi->componentCount = 3; pi->bitsPerComponent = 8;\n"
    "        pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;\n"
    "        strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));\n"
    "        pi->activeWidth = fW; pi->activeHeight = fH;\n"
    "        static unsigned logged;\n"
    "        if (logged++ < 4) XIFBLOG(\"scanout described: mode %#x %ux%u rowBytes %u\", (unsigned)mode, fW, fH, fRowBytes);\n"
    "        return kIOReturnSuccess;\n"
    "    }\n")
if "getPixelInformation" not in src:
    assert src.count(anchor3) == 1, "anchor3"
    src = src.replace(anchor3, add3)

assert src != orig
io.open(P, "w", encoding="utf-8").write(src)
print("patched, %d -> %d bytes" % (len(orig), len(src)))
