#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import sys


def extract_payload(bit_data: bytes) -> bytes:
    """
    Xilinx .bit files are ASCII headers (series of TLV records) followed by the
    raw bitstream payload. The payload record has tag 0x65 ('e'), a zero byte,
    then a four-byte big-endian length. Some headers contain other strings that
    happen to include the two-byte sequence 0x65 0x00, so we scan all matches
    and keep the longest valid payload we find.
    """
    tag = b"\x65\x00"
    idx = 0
    best = None
    while True:
        idx = bit_data.find(tag, idx)
        if idx == -1 or idx + 6 > len(bit_data):
            break
        length = int.from_bytes(bit_data[idx + 2 : idx + 6], "big")
        payload_start = idx + 6
        payload_end = payload_start + length
        if length > 0 and payload_end <= len(bit_data):
            candidate = bit_data[payload_start:payload_end]
            if best is None or len(candidate) > len(best):
                best = candidate
        idx += 1
    if best is not None and len(best) > 1024:
        return best

    sync = b"\xff\xff\xff\xff\xaa\x99\x55\x66"
    sync_idx = bit_data.find(sync)
    if sync_idx == -1:
        raise ValueError("Could not locate payload record (tag 0x65) or sync word in .bit file")
    return bit_data[sync_idx:]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Strip the ASCII header from a Xilinx .bit file to produce a .bin payload suitable for Linux fpga_manager."
    )
    parser.add_argument("bit_file", type=pathlib.Path, help="Input .bit bitstream")
    parser.add_argument(
        "bin_file",
        type=pathlib.Path,
        nargs="?",
        help="Output .bin path (defaults to input with .bin suffix)",
    )
    args = parser.parse_args(argv[1:])

    if not args.bit_file.exists():
        parser.error(f"Input file {args.bit_file} does not exist")

    bit_data = args.bit_file.read_bytes()
    try:
        payload = extract_payload(bit_data)
    except ValueError as exc:
        parser.error(str(exc))

    if args.bin_file is None:
        if args.bit_file.suffix == ".bit":
            args.bin_file = args.bit_file.with_suffix(".bin")
        else:
            args.bin_file = args.bit_file.with_name(args.bit_file.name + ".bin")

    args.bin_file.write_bytes(payload)
    print(f"Wrote {args.bin_file} ({len(payload)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
