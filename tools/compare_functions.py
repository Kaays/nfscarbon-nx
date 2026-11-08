#!/usr/bin/env python3
"""Finds function starts of another edition that have no counterpart in USA (and the reverse).

A wrong hand-declared function start in an edition's manifest splits a real function in two; the code
still runs, but the split one is entered in the middle with the wrong state, which shows up as crashes
on rare paths. Each edition's generated code lists its functions; this compares the two lists through
the code itself: a function start of the edition counts as matched when USA has a function start
nearby whose first instructions match (branch displacements and 16-bit immediates ignored).

Lists: after a codegen of an edition,
    grep -o 'sub_8[0-9A-F]\\{7\\}' app/generated/default/nfscarbon_register.cpp | sort -u > funcs_<region>.txt

    python3 tools/compare_functions.py --region pal funcs_usa.txt funcs_pal.txt
        -> prints the unmatched starts, those declared in the manifest first
"""
import argparse
import bisect
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from map_edition import BASE, load  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WINDOW = 6       # instructions compared at a function start
NEAR = 0x2000    # how far from the expected place a counterpart may be


def read_list(path):
    return sorted({int(m, 16) for m in re.findall(r"sub_(8[0-9A-Fa-f]{7})", Path(path).read_text())})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--region", required=True)
    ap.add_argument("usa_list")
    ap.add_argument("other_list")
    args = ap.parse_args()
    _, usa_n = load(ROOT / "assets/guest_image_usa.bin")
    _, oth_n = load(ROOT / f"assets/guest_image_{args.region}.bin")
    usa = read_list(args.usa_list)
    oth = read_list(args.other_list)

    # Piecewise delta from the map (unique code matches of manifest and hook addresses).
    pairs = []
    for line in (ROOT / f"app/generated/map_usa_to_{args.region}.txt").read_text().splitlines():
        p = line.split()
        if len(p) > 3 and p[1] != "?" and "window" in line and p[-1].startswith(("manifest", "hook")):
            pairs.append((int(p[1], 16), int(p[1], 16) - int(p[0], 16)))
    pairs.sort()
    keys = [k for k, _ in pairs]

    def delta_at(o):
        j = min(bisect.bisect_left(keys, o), len(keys) - 1)
        return pairs[j][1]

    def code(n, a):
        i = (a - BASE) // 4
        return tuple(int(x) for x in n[i:i + WINDOW])

    by_code = {}
    for u in usa:
        by_code.setdefault(code(usa_n, u), []).append(u)
    manifest = {int(m, 16) for m in re.findall(r"^(0x8[0-9A-Fa-f]{7}) =",
                (ROOT / f"app/nfscarbon_manifest_{args.region}.toml").read_text(), re.M)}
    unmatched = []
    for o in oth:
        want = code(oth_n, o)
        if len(want) < WINDOW:
            continue
        guess = o - delta_at(o)
        if not any(abs(u - guess) <= NEAR for u in by_code.get(want, ())):
            unmatched.append(o)
    in_manifest = [o for o in unmatched if o in manifest]
    print(f"{args.region}: {len(oth)} functions, USA {len(usa)}; {len(unmatched)} without a USA counterpart, "
          f"{len(in_manifest)} of them declared in the manifest")
    for o in in_manifest:
        print(f"  MANIFEST 0x{o:08X}")
    for o in unmatched:
        if o not in manifest:
            print(f"  0x{o:08X}")


if __name__ == "__main__":
    main()
