#!/usr/bin/env python3
"""Prints the instruction words of a guest image range: python3 tools/dump_words.py IMAGE START END"""
import struct, sys
path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
data = open(path, "rb").read()
for a in range(start, end, 16):
    o = a - 0x82000000
    print(f"{a:08X}: " + " ".join(f"{w:08X}" for w in struct.unpack_from(">4I", data, o)))
