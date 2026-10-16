#!/usr/bin/env python3
"""Adds function names to a stack-sampling rex_perfil.log.

    python3 tools/symbolize_profile.py rex_perfil.log [app/out/switch/nfscarbon.elf]

Writes rex_perfil_sym.log next to the input: every imagen+0x... gets the function it falls in, e.g.
imagen+0x189cb4[sub_826C6AB0+0x1c4]. Use the ELF of the same build as the NRO that wrote the log
(app/out/switch/nfscarbon.elf, the unstripped one).

Stack sampling is turned on by an empty file perfil_pilas.flag in sdmc:/switch/nfscarbon-nx/logs/rex/.
"""
import os
import re
import subprocess
import sys

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
log = sys.argv[1]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "app", "out", "switch", "nfscarbon.elf")
if not os.path.isfile(elf):
    raise SystemExit(f"ELF not found: {elf}")
dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
nm = os.path.join(dkp, "devkitA64", "bin", "aarch64-none-elf-nm")

# Function symbols sorted by address (nm is much faster than addr2line on an LTO build).
syms = []
out = subprocess.run([nm, "-C", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
for line in out.splitlines():
    parts = line.split(" ", 2)
    if len(parts) == 3 and parts[1] in "tTwW":
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
addrs = [a for a, _ in syms]

import bisect


def name(off):
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0:
        return "?"
    a, n = syms[i]
    return f"{n}+{off - a:#x}"


text = open(log, encoding="utf-8", errors="ignore").read()
text = re.sub(r"imagen\+0x([0-9a-f]+)", lambda m: f"{m.group(0)}[{name(int(m.group(1), 16))}]", text)
dst = os.path.join(os.path.dirname(os.path.abspath(log)), "rex_perfil_sym.log")
open(dst, "w", encoding="utf-8").write(text)
print(f"{len(syms)} symbols; wrote {dst}")
