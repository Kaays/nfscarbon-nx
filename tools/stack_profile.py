#!/usr/bin/env python3
"""Inclusive and self time per function from the raw stack samples (rex_pilas.bin).

    python3 tools/stack_profile.py rex_pilas.bin app/out/switch-collector_pal/nfscarbon [options]

Writes rex_pilas_report.txt next to the input (and prints it). The profiler writes rex_pilas.bin in
sdmc:/switch/nfscarbon-nx/logs/rex/ when perfil_pilas.flag is there; use the unstripped ELF of the same
build (the file without extension, not nfscarbon.stripped.elf).

"Inclusive" counts a sample for every distinct function on its stack (the leaf and the return addresses
of the frame-pointer chain, up to 16 deep), so a game system shows up with all the time spent under it,
even when that time is spread over hundreds of small functions. "Self" counts only the leaf.

Options:
  --threads A,B      only threads whose name contains one of these (default: the four busiest)
  --fps-max F        only report intervals with game fps <= F (e.g. 18 for crowded race starts)
  --fps-min F        only report intervals with game fps >= F
  --top N            rows per table (default 60)
  --focus NAME       also break down the time under NAME by the function it calls next
  --graph            also print the call graph: every function with at least --graph-min % inclusive and
                     the functions it calls, with the share of the thread spent through each call
  --graph-min P      threshold for --graph (default 1.0)
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess


def load_symbols(elf):
    dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    nm = os.path.join(dkp, "devkitA64", "bin", "aarch64-none-elf-nm")
    if not os.path.isfile(nm):
        nm = "nm"
    out = subprocess.run([nm, "-C", "--defined-only", elf], capture_output=True, text=True,
                         check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), parts[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def short(name):
    # Template-heavy C++ names: keep them readable in a table.
    name = name.replace("PPCContext&, unsigned char*", "...")
    return name if len(name) <= 110 else name[:107] + "..."


def read_samples(path):
    """Yields (interval, samples) with interval = (seconds, fps, {slot: (name, cpu)})."""
    data = open(path, "rb").read()
    if data[:4] != b"RXPL":
        raise SystemExit("not a rex_pilas.bin file")
    pos = 16
    interval = None
    while pos < len(data):
        kind = data[pos]
        pos += 1
        if kind == 1:
            secs, fps, count = struct.unpack_from("<ffH", data, pos)
            pos += 10
            threads = {}
            for _ in range(count):
                slot, cpu, length = struct.unpack_from("<HfB", data, pos)
                pos += 7
                threads[slot] = (data[pos:pos + length].decode("utf-8", "replace"), cpu)
                pos += length
            interval = (secs, fps, threads)
        elif kind == 2:
            (count,) = struct.unpack_from("<I", data, pos)
            pos += 4
            samples = []
            for _ in range(count):
                slot, nf, _zero, pc, lr = struct.unpack_from("<HBBII", data, pos)
                pos += 12
                frames = struct.unpack_from(f"<{nf}I", data, pos)
                pos += 4 * nf
                samples.append((slot, pc, lr, frames))
            yield interval, samples
        else:
            raise SystemExit(f"bad record type {kind} at {pos - 1}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin")
    ap.add_argument("elf")
    ap.add_argument("--threads")
    ap.add_argument("--fps-max", type=float)
    ap.add_argument("--fps-min", type=float)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--focus")
    ap.add_argument("--graph", action="store_true")
    ap.add_argument("--graph-min", type=float, default=1.0)
    args = ap.parse_args()

    addrs, names = load_symbols(args.elf)
    cache = {}

    def func(off):
        if off == 0xFFFFFFFF:
            return None
        f = cache.get(off)
        if f is None:
            i = bisect.bisect_right(addrs, off) - 1
            f = names[i] if i >= 0 else "?"
            cache[off] = f
        return f

    per_thread = collections.defaultdict(lambda: {"n": 0, "incl": collections.Counter(),
                                                  "self": collections.Counter(),
                                                  "focus": collections.Counter(), "focus_n": 0,
                                                  "edges": collections.Counter(),
                                                  "cpu": [], "name": ""})
    used_intervals = 0
    for (secs, fps, threads), samples in read_samples(args.bin):
        if args.fps_max is not None and fps > args.fps_max:
            continue
        if args.fps_min is not None and fps < args.fps_min:
            continue
        used_intervals += 1
        for slot, (name, cpu) in threads.items():
            per_thread[name]["cpu"].append(cpu)
        for slot, pc, lr, frames in samples:
            name = threads.get(slot, ("slot %d" % slot, 0.0))[0]
            t = per_thread[name]
            t["name"] = name
            stack = [func(pc)] + [func(a) for a in frames]
            stack = [s for s in stack if s]
            if not stack:
                continue
            t["n"] += 1
            t["self"][stack[0]] += 1
            for f in set(stack):
                t["incl"][f] += 1
            if args.graph:
                # stack is leaf first: stack[k + 1] called stack[k]. Each edge once per sample.
                t["edges"].update({(stack[k + 1], stack[k]) for k in range(len(stack) - 1)
                                   if stack[k + 1] != stack[k]})
            if args.focus:
                # Stack is leaf first: the function called by the focus one sits just before it.
                for k, f in enumerate(stack):
                    if f == args.focus or f.startswith(args.focus + "("):
                        t["focus_n"] += 1
                        t["focus"][stack[k - 1] if k > 0 else "(itself)"] += 1
                        break

    by_cpu = sorted(per_thread.values(),
                    key=lambda t: -(sum(t["cpu"]) / len(t["cpu"]) if t["cpu"] else 0))
    if args.threads:
        wanted = [w.strip() for w in args.threads.split(",")]
        chosen = [t for t in by_cpu if any(w in t["name"] for w in wanted)]
    else:
        chosen = [t for t in by_cpu if t["n"]][:4]

    lines = [f"{used_intervals} intervals used"
             + (f", fps <= {args.fps_max}" if args.fps_max is not None else "")
             + (f", fps >= {args.fps_min}" if args.fps_min is not None else "")]
    for t in chosen:
        if not t["n"]:
            continue
        cpu = sum(t["cpu"]) / len(t["cpu"]) if t["cpu"] else 0.0
        lines.append("")
        lines.append(f"==== {t['name']}: {t['n']} samples, {cpu:.1f} % of a core on average")
        lines.append("  inclusive (time spent in the function or anything it calls):")
        for f, c in t["incl"].most_common(args.top):
            lines.append(f"   {100.0 * c / t['n']:6.2f}%  {short(f)}")
        lines.append("  self (time in the function itself):")
        for f, c in t["self"].most_common(args.top // 2):
            lines.append(f"   {100.0 * c / t['n']:6.2f}%  {short(f)}")
        if args.graph:
            callees = collections.defaultdict(list)
            for (caller, callee), c in t["edges"].items():
                callees[caller].append((c, callee))
            lines.append(f"  call graph (functions with at least {args.graph_min}% inclusive; under each, what "
                         f"it calls and the share of the thread spent through that call):")
            for f, c in t["incl"].most_common():
                pct = 100.0 * c / t["n"]
                if pct < args.graph_min:
                    break
                lines.append(f"   {pct:6.2f}% incl {100.0 * t['self'][f] / t['n']:5.2f}% self  {short(f)}")
                for cc, g in sorted(callees.get(f, []), reverse=True)[:12]:
                    if 100.0 * cc / t["n"] < args.graph_min / 2:
                        break
                    lines.append(f"            -> {100.0 * cc / t['n']:6.2f}%  {short(g)}")
        if args.focus and t["focus_n"]:
            lines.append(f"  under {args.focus} ({100.0 * t['focus_n'] / t['n']:.2f}% of the thread), "
                         f"by the function it calls:")
            for f, c in t["focus"].most_common(args.top // 2):
                lines.append(f"   {100.0 * c / t['n']:6.2f}%  {short(f)}")
    text = "\n".join(lines) + "\n"
    dst = os.path.join(os.path.dirname(os.path.abspath(args.bin)), "rex_pilas_report.txt")
    open(dst, "w", encoding="utf-8").write(text)
    print(text)
    print(f"written {dst}")


if __name__ == "__main__":
    main()
