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
print("same code: the manifest can be shared" if not diff else "code differs: needs its own manifest")
