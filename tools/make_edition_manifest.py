#!/usr/bin/env python3
"""Writes the manifest and code generator options of a new edition from the ones already done.

A new edition needs the functions the analysis misses declared by hand (USA: app/nfscarbon_manifest.toml,
found on the console over weeks) and setjmp for the register options (app/overrides.toml). This maps
them into the new edition's image with tools/map_edition.py, from USA and from every edition already
done (their manifests hold what was found on their consoles: the Europe thunk table, for one), and
keeps only unique matches. What is left over shows up on the console in unregistered_targets.txt.

Needs the images: tools/codegen.sh writes assets/guest_image_<region>.bin for each edition it runs (also
when the analysis then fails, as it will for a new edition without a manifest).

    python3 tools/make_edition_manifest.py --region pal2 --disc "Need for Speed - Carbon (Europe) (...)"
        -> app/nfscarbon_manifest_pal2.toml, app/overrides_pal2.toml
"""
import argparse
import re
import subprocess
import sys
import tomllib
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "app"
GEN = APP / "generated"


def read_map(path):
    rows = []
    for line in path.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        if p[1] == "?":
            continue
        how = " ".join(p[3:-1])
        rows.append((int(p[0], 16), int(p[1], 16), how, p[-1]))
    return rows


def good(how):
    m = re.match(r"closest of (\d+)", how)
    if m:
        return int(m.group(1)) <= 4
    return how.startswith("window")  # not "short window": that is how the one wrong Europe guess was made


def source_overrides(src):
    """setjmp, functions declared with an end, and switch tables of an edition's options file."""
    path = APP / ("overrides.toml" if src == "usa" else f"overrides_{src}.toml")
    cfg = tomllib.loads(path.read_text()) if path.is_file() else {}
    setjmps, ends = set(), {}
    for key, val in cfg.get("functions", {}).items():
        a = int(key, 16)
        if val.get("share_registers"):
            setjmps.add(a)
        if "end" in val:
            ends[a] = val["end"]
    return setjmps, ends, cfg.get("switch_tables", [])


def load_norm(region):
    sys.path.insert(0, str(ROOT / "tools"))
    from map_edition import normalize
    data = np.fromfile(ROOT / f"assets/guest_image_{region}.bin", dtype=">u4").astype(np.uint32)
    return normalize(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--region", required=True)
    ap.add_argument("--disc", default="", help="disc name for the header comment")
    args = ap.parse_args()
    r = args.region
    if not (ROOT / f"assets/guest_image_{r}.bin").is_file():
        sys.exit(f"assets/guest_image_{r}.bin not found: run REGION={r} bash tools/codegen.sh once first")

    sources = ["usa"] + sorted(p.name[len("nfscarbon_manifest_"):-len(".toml")]
                               for p in APP.glob("nfscarbon_manifest_*.toml")
                               if p.name != f"nfscarbon_manifest_{r}.toml")
    sources = [s for s in sources if (ROOT / f"assets/guest_image_{s}.bin").is_file()]
    found = {}       # address in the new edition -> (source, source address)
    setjmp = None
    bounded = {}     # function start in the new edition -> (end, [switch tables], source, source address)
    new_n = load_norm(r)
    for src in sources:
        setjmps, ends, switches = source_overrides(src)
        src_n = load_norm(src) if ends else None
        subprocess.run([sys.executable, str(ROOT / "tools/map_edition.py"), "--from", src, "--region", r],
                       check=True)
        rows = read_map(GEN / f"map_{src}_to_{r}.txt")
        counts = {}
        for _, dst, _, _ in rows:
            counts[dst] = counts.get(dst, 0) + 1
        for a, dst, how, where in rows:
            if not where.startswith("manifest:"):
                continue
            if where.startswith("manifest:overrides"):
                if a in setjmps and setjmp is None and good(how):
                    setjmp = (dst, src, a)
                if a in ends and dst not in bounded and good(how):
                    # A function whose bounds and switch tables were set by hand: carry them over
                    # when the whole function is the same code here, moved by one delta.
                    d, e = dst - a, ends[a]
                    i, j, k = (a - 0x82000000) // 4, (e - 0x82000000) // 4, (dst - 0x82000000) // 4
                    if np.array_equal(src_n[i:j], new_n[k:k + j - i]):
                        tabs = [{"address": t["address"] + d, "register": t["register"],
                                 "labels": [x + d for x in t["labels"]]}
                                for t in switches if a <= t["address"] < e]
                        bounded[dst] = (e + d, tabs, src, a)
                    else:
                        print(f"  {src} 0x{a:08X}: bounded function differs here, add it by hand")
                continue
            if counts[dst] > 1 or not good(how):
                continue
            found.setdefault(dst, (src, a))

    header = f"# nfscarbon-nx: {args.disc or r} edition.\n" if args.disc else f"# nfscarbon-nx: {r} edition.\n"
    lines = [header.rstrip(),
             f"# Written by tools/make_edition_manifest.py from {', '.join(sources)}: the hand-declared functions",
             "# of those editions, found here by their instructions (unique matches only). Add what the console",
             "# logs in unregistered_targets.txt by hand below.",
             "",
             "[project]", 'name = "nfscarbon"', 'sdk_version = "0.10.0"', 'game_root = "../assets/game_root"', "",
             "[entrypoint]", 'file_path = "../assets/game_root/default.xex"', 'out_directory_path = "generated/default"',
             f'includes = ["overrides_{r}.toml"]', "", "[entrypoint.functions]"]
    for dst in sorted(found):
        src, a = found[dst]
        lines.append(f"0x{dst:08X} = {{}}  # {src} 0x{a:08X}")
    (APP / f"nfscarbon_manifest_{r}.toml").write_text("\n".join(lines) + "\n")

    if setjmp:
        ov = f"""# nfscarbon-nx: code generator options for the {r} edition (see overrides.toml for what they do).
# setjmp found by tools/make_edition_manifest.py ({setjmp[1]} 0x{setjmp[2]:08X}).

cr_as_local = true
non_volatile_as_local = false
xer_as_local = true
ctr_as_local = true

[functions]
# setjmp: share_registers so the jmp_buf gets the caller's cr.
"0x{setjmp[0]:08X}" = {{ share_registers = true }}
"""
    else:
        ov = f"""# nfscarbon-nx: code generator options for the {r} edition. setjmp was not found, so the
# register-as-local options stay off (slower, but safe).

cr_as_local = false
non_volatile_as_local = false
xer_as_local = false
ctr_as_local = false
"""
    for dst in sorted(bounded):
        end, tabs, src, a = bounded[dst]
        if "[functions]" not in ov:
            ov += "\n[functions]\n"
        ov = ov.replace("[functions]\n", f"[functions]\n# Bounds set by hand ({src} 0x{a:08X}): a switch table the scanner misses.\n"
                        f'"0x{dst:08X}" = {{ end = 0x{end:08X} }}\n', 1)
        for t in tabs:
            labels = ", ".join(f"0x{x:08X}" for x in t["labels"])
            ov += f"\n[[switch_tables]]\naddress = 0x{t['address']:08X}\nregister = {t['register']}\nlabels = [{labels}]\n"
    (APP / f"overrides_{r}.toml").write_text(ov)
    print(f"{r}: {len(bounded)} functions with hand-set bounds carried over")
    print(f"{r}: {len(found)} functions -> app/nfscarbon_manifest_{r}.toml; setjmp "
          f"{'0x%08X' % setjmp[0] if setjmp else 'not found'} -> app/overrides_{r}.toml")


if __name__ == "__main__":
    main()
