#!/usr/bin/env python3
"""Lets a Switch program use the fourth CPU core (core 3) by patching its main.npdm.

A Switch application normally may use cores 0-2: the ThreadInfo kernel capability of its NPDM says
"highest core 2", and Horizon keeps core 3 for the system. Raising that to 3, in both kernel
capability tables (ACI0, the program's own, and ACID, the signed limit), gives the process the core
mask 0xF. nfscarbon-nx then runs its threads on all four cores (RexSwitchUseAllProcessCores,
sdk/src/core/threading_switch.cpp). Nothing else changes; this is the same edit as
koren699/GTA4NX-Core3-Fix for its forwarder.

Use it on the main.npdm of an NSP forwarder you made for nfscarbon-nx, then put the result at
    sdmc:/atmosphere/contents/<forwarder title id>/exefs/main.npdm
and reboot. Remove that file to undo. The ACID is signed, so Atmosphère must accept a modified one
(the same signature patches that installed forwarders need).

Core 3 is where the system's own services run: HOME menu, overlays and background services share it
with the game. Report what you see (stability, overlays) with the logs.

Usage: python3 tools/npdm_core3.py main.npdm [-o patched.npdm] [--core 3]
"""
import argparse
import struct
import sys


def thread_info_offsets(data, kc_offset, kc_size):
    """Offsets of the ThreadInfo descriptors (low 4 bits 0b0111) in a kernel capability table."""
    found = []
    for off in range(kc_offset, kc_offset + kc_size, 4):
        (value,) = struct.unpack_from("<I", data, off)
        if value & 0xF == 0x7:
            found.append(off)
    return found


def patch(data, max_core):
    if data[0:4] != b"META":
        raise ValueError("not an NPDM (no META magic)")
    aci0_off, aci0_size, acid_off, acid_size = struct.unpack_from("<IIII", data, 0x70)
    tables = []
    if data[aci0_off:aci0_off + 4] != b"ACI0":
        raise ValueError("ACI0 not found where the META header says")
    kc_off, kc_size = struct.unpack_from("<II", data, aci0_off + 0x30)
    tables.append(("ACI0", aci0_off + kc_off, kc_size))
    if data[acid_off + 0x200:acid_off + 0x204] != b"ACID":
        raise ValueError("ACID not found where the META header says")
    kc_off, kc_size = struct.unpack_from("<II", data, acid_off + 0x230)
    tables.append(("ACID", acid_off + kc_off, kc_size))

    out = bytearray(data)
    for name, start, size in tables:
        offsets = thread_info_offsets(out, start, size)
        if len(offsets) != 1:
            raise ValueError(f"{name}: expected one ThreadInfo capability, found {len(offsets)}")
        (value,) = struct.unpack_from("<I", out, offsets[0])
        lowest_prio = (value >> 4) & 0x3F
        highest_prio = (value >> 10) & 0x3F
        min_core = (value >> 16) & 0xFF
        old_max = (value >> 24) & 0xFF
        new_value = (value & 0x00FFFFFF) | (max_core << 24)
        struct.pack_into("<I", out, offsets[0], new_value)
        print(f"{name}: ThreadInfo priorities {highest_prio}-{lowest_prio}, cores {min_core}-{old_max}"
              f" -> {min_core}-{max_core}")
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("npdm")
    ap.add_argument("-o", "--output", help="where to write (default: <input>.core3.npdm)")
    ap.add_argument("--core", type=int, default=3, help="highest core allowed (default 3)")
    args = ap.parse_args()
    if not 0 <= args.core <= 3:
        sys.exit("--core must be 0-3")
    data = open(args.npdm, "rb").read()
    try:
        patched = patch(data, args.core)
    except ValueError as e:
        sys.exit(f"error: {e}")
    out = args.output or args.npdm + ".core3.npdm"
    open(out, "wb").write(patched)
    print(f"written: {out}")


if __name__ == "__main__":
    main()
