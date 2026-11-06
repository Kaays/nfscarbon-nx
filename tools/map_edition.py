#!/usr/bin/env python3
"""Finds the USA addresses nfscarbon-nx uses in another edition's default.xex.

The Europe default.xex is the same build of the game with small changes: the image has the same size
and functions sit a few hundred bytes to a few KB away from their USA address. So each USA address is
looked up by the instructions that start there, in a window around the same address of the other
image. Branch displacements and 16-bit immediates (data addresses, offsets) are ignored in the
comparison, since those move too.

Input: the images written by the code generator (tools/codegen.sh writes assets/guest_image_<region>.bin).
Addresses mapped: the hand-declared functions of app/nfscarbon_manifest.toml and app/overrides.toml, and
every sub_8XXXXXXX / 0x8XXXXXXX named in the USA-only hook sources (app/hooks_usa.txt).

    python3 tools/map_edition.py --region pal
        -> app/map_usa_to_pal.txt (one line per address: usa, other, how it was found)

Lines marked "?" were not found uniquely and need a look by hand.
"""
import argparse
import bisect
import re
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "app"
BASE = 0x82000000
SEARCH = 0x10000        # how far from the expected place to look (bytes)
WINDOWS = (16, 32, 64)  # instructions compared, longer when the short one is not unique
MIN_WINDOW = 6

D_FORM_IMM = set(range(7, 16)) | set(range(24, 30)) | set(range(32, 64))


def normalize(words):
    """Keep opcode and registers, drop branch displacements and 16-bit immediates."""
    op = words >> 26
    out = words.copy()
    out[op == 18] &= 0xFC000003           # b / bl: keep AA, LK
    out[op == 16] &= 0xFFFF0003           # bc: keep BO, BI, AA, LK
    imm = np.isin(op, list(D_FORM_IMM))
    out[imm] &= 0xFFFF0000
    return out


def load(path):
    data = np.fromfile(path, dtype=">u4").astype(np.uint32)
    return data, normalize(data)


def usa_addresses():
    found = {}
    for name in ("nfscarbon_manifest.toml", "overrides.toml"):
        for m in re.finditer(r'^\s*"?(0x8[0-9A-Fa-f]{7})"?\s*=', (APP / name).read_text(), re.M):
            found.setdefault(int(m.group(1), 16), f"manifest:{name}")
    for line in (APP / "hooks_usa.txt").read_text().splitlines():
        if not line.startswith("src/"):
            continue
        text = (APP / line).read_text(errors="ignore")
        for m in re.finditer(r"\bsub_(8[0-9A-Fa-f]{7})\b", text):
            found.setdefault(int(m.group(1), 16), f"hook:{line}")
        for m in re.finditer(r"\b0x(8[0-9A-Fa-f]{7})u?\b", text):
            found.setdefault(int(m.group(1), 16), f"const:{line}")
    return found


def find(usa_n, other_n, addr, expected_delta):
    """Address in the other image whose instructions match those at addr (USA), or None."""
    i = (addr - BASE) // 4
    if i < 0 or i >= len(usa_n):
        return None, "outside image"
    if not usa_n[i:i + MIN_WINDOW].any():
        return None, "no code (data or zero)"
    centre = i + expected_delta // 4
    lo = max(0, centre - SEARCH // 4)
    hi = min(len(other_n), centre + SEARCH // 4)
    for n in WINDOWS:
        pattern = usa_n[i:i + n]
        if len(pattern) < n:
            break
        region = other_n[lo:hi + n]
        if len(region) < n:
            return None, "no room"
        view = np.lib.stride_tricks.sliding_window_view(region, n)
        hits = np.nonzero((view == pattern).all(axis=1))[0]
        if len(hits) == 1:
            return BASE + (lo + int(hits[0])) * 4, f"window {n}"
        if len(hits) == 0:
            if n == WINDOWS[0]:
                # The function may differ a little: try a shorter window before giving up.
                pattern = usa_n[i:i + MIN_WINDOW * 2]
                view = np.lib.stride_tricks.sliding_window_view(other_n[lo:hi + len(pattern)], len(pattern))
                hits = np.nonzero((view == pattern).all(axis=1))[0]
                if len(hits) == 1:
                    return BASE + (lo + int(hits[0])) * 4, f"short window {len(pattern)}"
            return None, "not found"
        # Several hits: the closest to the expected place, if it is clearly closer.
        dist = sorted((abs(lo + int(h) - centre), int(h)) for h in hits)
        if n == WINDOWS[-1]:
            if dist[0][0] * 4 < dist[1][0]:
                return BASE + (lo + dist[0][1]) * 4, f"closest of {len(hits)}"
            return None, f"{len(hits)} matches"
    return None, "not found"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--region", required=True, help="other edition (pal, ...)")
    ap.add_argument("--usa", default=str(ROOT / "assets/guest_image_usa.bin"))
    ap.add_argument("--other", help="default: assets/guest_image_<region>.bin")
    ap.add_argument("-o", "--output", help="default: app/map_usa_to_<region>.txt")
    args = ap.parse_args()
    other_path = Path(args.other or ROOT / f"assets/guest_image_{args.region}.bin")
    for p in (Path(args.usa), other_path):
        if not p.is_file():
            sys.exit(f"{p} not found: run tools/codegen.sh for that region first (it writes the image)")

    _, usa_n = load(args.usa)
    _, other_n = load(other_path)
    size = len(usa_n) * 4
    addrs = {a: w for a, w in usa_addresses().items() if a % 4 == 0 and BASE <= a < BASE + size}

    # First pass with no expected offset; then again with the offset of the nearest mapped neighbours,
    # which settles most of the ambiguous ones.
    result = {a: find(usa_n, other_n, a, 0) for a in sorted(addrs)}
    known = sorted((a, r[0] - a) for a, r in result.items() if r[0] is not None)
    keys = [k for k, _ in known]
    for a in sorted(addrs):
        if result[a][0] is not None or not known:
            continue
        j = min(bisect.bisect_left(keys, a), len(keys) - 1)
        delta = known[j][1]
        r = find(usa_n, other_n, a, delta)
        if r[0] is not None:
            result[a] = (r[0], r[1] + f", near delta {delta:+#x}")

    out = Path(args.output or APP / f"map_usa_to_{args.region}.txt")
    ok = 0
    with out.open("w") as f:
        f.write(f"# usa -> {args.region}: {len(addrs)} addresses (tools/map_edition.py)\n")
        for a in sorted(addrs):
            other, how = result[a]
            if other is None:
                f.write(f"0x{a:08X} ?          {how:<40} {addrs[a]}\n")
            else:
                ok += 1
                f.write(f"0x{a:08X} 0x{other:08X} {other - a:+#07x} {how:<30} {addrs[a]}\n")
    print(f"mapped {ok} of {len(addrs)}; written {out}")


if __name__ == "__main__":
    main()
