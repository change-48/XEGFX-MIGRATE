import sys
p = sys.argv[1]
s = open(p).read()

old = ("            info->flags = (base.flags & kDisplayModeSafetyFlags) | kDisplayModeValidFlag "
       "| kDisplayModeSafeFlag;")
if old not in s:
    raise SystemExit("ANCHOR NOT FOUND")

new = '''            // CoreDisplay builds the menu from IOFBConfig; a mode that keeps the panel raster but hands the
            // compositor a smaller backing still has to look like a first-class mode there, so mark it native
            // and high-res the way the base path does, and carry the panel physical size across with it.
            info->flags = (base.flags & kDisplayModeSafetyFlags) | kDisplayModeValidFlag | kDisplayModeSafeFlag
                        | kDisplayModeNativeFlag | kDisplayModeValidForHiResFlag;
            info->imageWidth = base.imageWidth; info->imageHeight = base.imageHeight;'''

s = s.replace(old, new, 1)
open(p, "w").write(s)
print("patched")
