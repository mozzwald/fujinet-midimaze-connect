#!/usr/bin/env python3
import argparse
import os
import struct
import sys


RUNAD_START = 0x02E0
RUNAD_END = 0x02E1
INITAD_START = 0x02E2
INITAD_END = 0x02E3
XEX_MARKER = 0xFFFF


def parse_segments(data):
    offset = 0
    if len(data) < 2:
        raise ValueError("File too small for XEX")
    marker = struct.unpack_from("<H", data, offset)[0]
    if marker != XEX_MARKER:
        raise ValueError("Missing XEX marker 0xFFFF at start")
    while offset + 2 <= len(data):
        marker = struct.unpack_from("<H", data, offset)[0]
        if marker == XEX_MARKER:
            offset += 2
            continue
        if offset + 4 > len(data):
            break
        start, end = struct.unpack_from("<HH", data, offset)
        seg_len = end - start + 1
        if seg_len < 0:
            raise ValueError("Invalid segment length")
        yield offset, start, end
        offset += 4 + seg_len


def fix_runad(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())

    matches = []
    for header_off, start, end in parse_segments(data):
        if start == RUNAD_START and end == RUNAD_END:
            matches.append(header_off)

    if not matches:
        raise ValueError("No RUNAD segment found")

    header_off = matches[-1]
    struct.pack_into("<HH", data, header_off, INITAD_START, INITAD_END)

    tmp_path = path + ".tmp"
    with open(tmp_path, "wb") as f:
        f.write(data)
    os.replace(tmp_path, path)


def main():
    parser = argparse.ArgumentParser(description="Convert RUNAD to INITAD in XEX file.")
    parser.add_argument("xex", help="Path to XEX file to modify in place")
    args = parser.parse_args()
    try:
        fix_runad(args.xex)
    except Exception as exc:
        print(f"fix_runad.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
