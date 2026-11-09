#!/usr/bin/env python3
"""Compares the code of two editions word by word, to tell whether one can use the other's
manifest unchanged: python3 tools/compare_images.py pal2 fr

Input: assets/guest_image_<region>.bin, written by tools/codegen.sh. Only the code section is
compared (0x82110000 up to the end given by --end); data and strings may differ freely."""
import argparse, struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
p = argparse.ArgumentParser()
p.add_argument("a"); p.add_argument("b")
p.add_argument("--start", default="82110000"); p.add_argument("--end", default="82B34C64")
args = p.parse_args()
start, end = int(args.start, 16), int(args.end, 16)
imgs = []
for r in (args.a, args.b):
    f = ROOT / "assets" / f"guest_image_{r}.bin"
    if not f.exists():
        sys.exit(f"{f} not found: run REGION={r} tools/codegen.sh first (it writes the image)")
    imgs.append(f.read_bytes())
n = (end - start) // 4
o = start - 0x82000000
wa = struct.unpack_from(f">{n}I", imgs[0], o)
wb = struct.unpack_from(f">{n}I", imgs[1], o)
diff = [i for i in range(n) if wa[i] != wb[i]]
print(f"{args.a} vs {args.b}: code 0x{start:08X}-0x{end:08X}, {n} words, {len(diff)} differ")
# Group differing words into runs and show the first ones.
runs, i = [], 0
while i < len(diff):
    j = i
    while j + 1 < len(diff) and diff[j + 1] - diff[j] <= 4:
        j += 1
    runs.append((diff[i], diff[j])); i = j + 1
for s, e in runs[:40]:
    a = start + s * 4
    print(f"  0x{a:08X}-0x{start + e * 4 + 3:08X}: {args.a} {wa[s]:08X}  {args.b} {wb[s]:08X}")
if len(runs) > 40:
    print(f"  ... {len(runs) - 40} more runs")
# Same instructions with other immediates (data moved, branch displacements) still means the same
# functions at the same addresses: the manifest holds only code addresses.
sys.path.insert(0, str(ROOT / "tools"))
import numpy as np
from map_edition import normalize
na = normalize(np.array(wa, dtype=np.uint32))
nb = normalize(np.array(wb, dtype=np.uint32))
nd = np.nonzero(na != nb)[0]
print(f"ignoring immediates and branch displacements: {len(nd)} words differ"
      + (f", first at 0x{start + int(nd[0]) * 4:08X}" if len(nd) else ""))
if not diff:
    print("same code: the manifest can be shared")
elif not len(nd):
    print("same layout (only data offsets differ): the manifest can be shared")
else:
    print("code moved: needs its own manifest (tools/make_edition_manifest.py)")
