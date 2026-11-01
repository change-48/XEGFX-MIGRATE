#!/usr/bin/env python3
# A-09 decoder: parse the XeDiag ioreg blob dump (read-only MMIO windows) and
# decode Intel display-14 (Arrow Lake / MTL family) registers.
#
# Every offset/bitfield below was read from the local i915 tree, not guessed:
#   drivers/gpu/drm/i915/i915_reg.h
#       _TRANS_HTOTAL_A 0x60000, _TRANS_HBLANK_A 0x60004, _TRANS_HSYNC_A 0x60008,
#       _TRANS_VTOTAL_A 0x6000c, _TRANS_VBLANK_A 0x60010, _TRANS_VSYNC_A 0x60014
#       GGC 0x108040 (GMS_MASK 15:8, GGMS_MASK 7:6), GEN6_DSMBASE 0x1080C0
#   drivers/gpu/drm/i915/display/skl_universal_plane_regs.h
#       _PLANE_CTL_1_A 0x70180, _PLANE_STRIDE 0x70188, _PLANE_SIZE 0x70190,
#       _PLANE_SURF 0x7019c, _PLANE_OFFSET 0x701a4, _PLANE_SURFLIVE 0x701ac
#       PLANE_CTL_ENABLE bit31, PLANE_CTL_FORMAT bits 27:24 (RGB path) /
#       27:23 (ICL+ YUV path), PLANE_CTL_TILED bits 12:10
#   drivers/gpu/drm/i915/gvt/fb_decoder.c
#       PIPESRC: width-1 in bits 31:16, height-1 in bits 15:0
#   drivers/gpu/drm/i915/gem/i915_gem_stolen.c
#       mtl_get_gms_size(): gms 0x0..4 -> gms*32 MB ; 0xf0..0xfe -> (gms-0xf0+1)*4 MB
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'ioreg.txt'
txt = open(path, 'r', errors='replace').read()

blobs = {}
for name, hexs in re.findall(r'"(\w+)"\s*=\s*<([0-9a-fA-F\s]+)>', txt):
    blobs[name] = bytes.fromhex(re.sub(r'\s+', '', hexs))

BASE = {'w40': 0x40000, 'w44': 0x44000, 'w45': 0x45000, 'w50': 0x50000,
        'w60': 0x60000, 'w61': 0x61000, 'w62': 0x62000, 'w63': 0x63000,
        'w64': 0x64000, 'w65': 0x65000, 'w66': 0x66000, 'w67': 0x67000,
        'w68': 0x68000, 'w69': 0x69000,
        'w70': 0x70000, 'w71': 0x71000, 'w72': 0x72000, 'w73': 0x73000,
        'misc': 0x108000}

out = []
W = out.append


def rd(addr, width=4):
    for n, b in blobs.items():
        base = BASE.get(n)
        if base is None:
            continue
        if base <= addr and addr + width <= base + len(b):
            off = addr - base
            return int.from_bytes(b[off:off + width], 'little')
    return None


def h(v, bits=8):
    return 'N/A' if v is None else ('0x%0*X' % (bits, v))


FMT24 = {0: 'YUV422', 1: 'NV12', 2: 'XRGB2101010', 3: 'P010', 4: 'XRGB8888',
         5: 'P012', 6: 'XRGB161616F', 7: 'P016', 8: 'XYUV', 12: 'INDEXED',
         14: 'RGB565'}
TILE = {0: 'LINEAR', 1: 'X-tile', 4: 'Y-tile', 5: 'Yf-tile(4)'}

W('=== windows dumped by XeDiag (%d) ===' % len(blobs))
for n in sorted(blobs, key=lambda k: BASE.get(k, 0)):
    W('  %-6s base 0x%05X  %d bytes' % (n, BASE[n], len(blobs[n])))

W('')
W('=== Q1/Q2  which plane is scanning ===')
for p in range(4):
    b = 0x70000 + 0x1000 * p
    ctl, stride, size = rd(b + 0x180), rd(b + 0x188), rd(b + 0x190)
    surf, off, live = rd(b + 0x19C), rd(b + 0x1A4), rd(b + 0x1AC)
    W('pipe%d plane1 CTL=%s STRIDE=%s(%d B) SIZE=%s(%dx%d) SURF=%s TILEOFF=%s '
      'SURFLIVE=%s'
      % (p, h(ctl), h(stride), (stride or 0) * 64,
         h(size), (size & 0xFFFF) + 1 if size else 0,
         (size >> 16) + 1 if size else 0,
         h(surf), h(off), h(live)))
    if live:
        W('   >>> pipe%d is the LIVE one. SURF == SURFLIVE ? %s'
          % (p, 'YES (latched, no pending flip)' if surf == live else 'NO'))

W('')
W('=== Q3  PLANE_CTL decode ===')
for p in range(4):
    ctl = rd(0x70000 + 0x1000 * p + 0x180)
    if not ctl:
        continue
    fmt24 = (ctl >> 24) & 0xF
    fmt23 = (ctl >> 23) & 0x1F
    tiling = (ctl >> 10) & 0x7
    W('pipe%d CTL=%s ENABLE(31)=%d FORMAT 27:24=%d(%s) [ICL 27:23=%d] '
      'TILING 12:10=%d(%s) bit10=%d'
      % (p, h(ctl), ctl >> 31, fmt24, FMT24.get(fmt24, '?'), fmt23,
         tiling, TILE.get(tiling, '?'), (ctl >> 10) & 1))

W('')
W('=== transcoder timing (HTOTAL 0x00 VTOTAL 0x0C VSYNC 0x14 PIPESRC 0x1C '
  'FRMCOUNT 0x40) ===')
for p in range(4):
    b = 0x60000 + 0x1000 * p
    ht, hb, hs = rd(b + 0x00), rd(b + 0x04), rd(b + 0x08)
    vt, vb, vs = rd(b + 0x0C), rd(b + 0x10), rd(b + 0x14)
    src, fc = rd(b + 0x1C), rd(b + 0x40)
    if not ht:
        W('pipe%d  all zero (inactive)' % p)
        continue
    W('pipe%d HTOTAL=%s  htotal=%d hblank_start=%d hsync_start=%d'
      % (p, h(ht), (ht >> 16) + 1, (hb & 0x1FFF) + 1, (hs & 0x1FFF) + 1))
    W('      VTOTAL=%s  vtotal=%d vblank_start=%d vsync_start=%d'
      % (h(vt), (vt >> 16) + 1, (vb & 0x1FFF) + 1, (vs & 0x1FFF) + 1))
    W('      PIPESRC=%s  source = %d x %d   FRMCOUNT=%s'
      % (h(src), (src >> 16) + 1, (src & 0xFFFF) + 1, h(fc)))

W('')
W('=== pipe scaler (0x68174 / 0x68180) ===')
pswin, psctl = rd(0x68174), rd(0x68180)
if pswin:
    W('PS_WIN_SZ 0x68174=%s -> %d x %d' % (h(pswin), (pswin >> 16) & 0x1FFF,
                                           pswin & 0x1FFF))
W('PS_CTRL   0x68180=%s -> scaler enable bit31 = %s'
  % (h(psctl), psctl >> 31 if psctl is not None else 'N/A'))

W('')
W('=== Q4  GGC / DSMBASE / stolen ===')
ggc, dsm = rd(0x108040), rd(0x1080C0, 8)
W('GGC      0x108040 = %s' % h(ggc))
if ggc is not None:
    gms, ggms = (ggc >> 8) & 0xFF, (ggc >> 6) & 0x3
    W('  GGMS(7:6) = %d   GMS(15:8) = 0x%02X' % (ggms, gms))
    if gms <= 4:
        mb = gms * 32
    elif 0xF0 <= gms <= 0xFE:
        mb = (gms - 0xF0 + 1) * 4
    else:
        mb = None
    if mb is None:
        W('  GMS value invalid -> i915 mtl_get_gms_size() would return -EIO')
    else:
        W('  mtl_get_gms_size() -> %d MB = %d bytes' % (mb, mb << 20))
        W('  26 MB (27262976 B) fits ? %s'
          % ('YES' if (mb << 20) >= 27262976 else 'NO'))
W('DSMBASE  0x1080C0 = %s -> 0x%X'
  % (h(dsm, 16), (dsm or 0) & 0x7FFFFFFF000))

W('')
W('=== raw words 0x60000..0x6004F (pipe A transcoder) ===')
for a in range(0x60000, 0x60050, 4):
    W('  0x%05X = %s' % (a, h(rd(a))))

open('decode.txt', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
