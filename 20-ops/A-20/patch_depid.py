#!/usr/bin/env python3
# A-20: move IOFBDependentID/Index before IOFramebuffer::start(), use XEGF id + index 0.
import re, sys
p = sys.argv[1]
s = open(p).read()
orig = s

lines = s.split('\n')
# 1) remove the two old setProperty lines wherever they are (both branches)
old_block = ['    setProperty("IOFBDependentID", 0x4E56524D00000001ull, 64);',
             '    setProperty("IOFBDependentIndex", 4ull, 32);']
new = []
removed = 0
i = 0
while i < len(lines):
    if lines[i] == old_block[0] and i + 1 < len(lines) and lines[i+1] == old_block[1]:
        i += 2
        removed += 1
        continue
    new.append(lines[i])
    i += 1
if removed != 2:
    print("ERR: expected 2 old depid blocks, removed", removed)
    sys.exit(1)

ins = ['    setProperty("IOFBDependentID",    0x5845474600000001ull, 64);',
       '    setProperty("IOFBDependentIndex", 0ull, 32);']
out = []
ins_count = 0
for ln in new:
    st = ln.strip()
    if st.startswith('if (!IOFramebuffer::start(provider))'):
        out.extend(ins)
        ins_count += 1
    out.append(ln)
if ins_count != 2:
    print("ERR: expected 2 start() sites, got", ins_count)
    sys.exit(1)

open(p, 'w').write('\n'.join(out))
print("OK removed=%d inserted=%d" % (removed, ins_count))
