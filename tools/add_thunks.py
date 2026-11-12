#!/usr/bin/env python3
"""Declares an edition's this-adjust thunks in its manifest.

A this-adjust thunk is two instructions, `addi r3,r3,-N` and `b target`, reached through a vtable. The
linker packs them in runs whose order differs between editions, so a thunk carried from another
edition by its address can land on the wrong slot, and the code generator does not always find them
by itself: the French disc called the unfound thunk 0x82541988 (unregistered_targets.txt) and could
not start a race. This lists every thunk that sits in a run of thunks and is referenced from data,
so none depends on being found.

    python3 tools/add_thunks.py --region fr [--image assets/guest_image_fr.bin]
"""
import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x82000000
MARK = "# This-adjust thunks (tools/add_thunks.py)"


def find_thunks(image):
    d = Path(image).read_bytes()
    n = len(d) // 4
    w = struct.unpack(">%dI" % n, d[: n * 4])
    referenced = {x for x in w if BASE <= x < BASE + len(d) and x % 4 == 0}
    thunks = {}
    for a in referenced:
        o = (a - BASE) // 4
        if o + 1 >= n:
            continue
        i1, i2 = w[o], w[o + 1]
        if (i1 >> 16) == 0x3863 and (i2 >> 26) == 18 and not (i2 & 3):
            disp = i2 & 0x3FFFFFC
            if disp & 0x2000000:
                disp -= 0x4000000
            imm = i1 & 0xFFFF
            if imm & 0x8000:
                imm -= 0x10000
            thunks[a] = (imm, a + 4 + disp)
    # Only thunks in runs: a lone match could be a switch case inside a function.
    return {a: t for a, t in thunks.items() if a - 8 in thunks or a + 8 in thunks}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--region", required=True)
    ap.add_argument("--image", help="default: assets/guest_image_<region>.bin")
    args = ap.parse_args()
    image = args.image or ROOT / f"assets/guest_image_{args.region}.bin"
    manifest = ROOT / "app" / f"nfscarbon_manifest_{args.region}.toml"
    text = manifest.read_text()
    if MARK in text:
        text = text[: text.index(MARK)].rstrip("\n") + "\n"
    have = {int(m, 16) for m in re.findall(r"^(0x[0-9A-Fa-f]{8})\s*=", text, re.M)}
    thunks = find_thunks(image)
    new = sorted(a for a in thunks if a not in have)
    lines = [MARK]
    lines += [f"0x{a:08X} = {{}}  # r3 {thunks[a][0]:+d}, to 0x{thunks[a][1]:08X}" for a in new]
    manifest.write_text(text.rstrip("\n") + "\n" + "\n".join(lines) + "\n")
    print(f"{manifest.name}: {len(thunks)} thunks in runs, {len(new)} added")


if __name__ == "__main__":
    main()
