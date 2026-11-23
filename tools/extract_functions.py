#!/usr/bin/env python3
"""Copies the recompiled C++ of some game functions out of app/generated/default, for analysis.

    python3 tools/extract_functions.py sub_826B0CF8 sub_822B6B30 ... > functions.txt

Each function is printed from its definition line to the closing brace at the start of a line. Used to
study a hot loop before writing a native version of it (app/src/audio_native.cpp). The output is
derived from the game's code: keep it local, it is not for the repository.
"""
import re
import sys
from pathlib import Path

names = sys.argv[1:]
if not names:
    raise SystemExit(__doc__)
gen = Path(__file__).resolve().parent.parent / "app" / "generated" / "default"
wanted = {n: re.compile(r"^\S.*\b(?:__imp__)?" + re.escape(n) + r"\b.*\{\s*$") for n in names}
found = set()
for path in sorted(gen.glob("*.cpp")):
    lines = path.read_text(errors="replace").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        hit = next((n for n, rx in wanted.items() if n not in found and rx.match(line)
                    and "extern" not in line), None)
        if hit is None:
            i += 1
            continue
        j = i
        while j < len(lines) and lines[j] != "}":
            j += 1
        print(f"// ---- {hit} ({path.name}:{i + 1})")
        print("\n".join(lines[i:j + 1]))
        print()
        found.add(hit)
        i = j + 1
for n in names:
    if n not in found:
        print(f"// ---- {n}: not found", file=sys.stderr)
