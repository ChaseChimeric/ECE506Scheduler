#!/usr/bin/env python3
"""
Prepare FPGA manager-ready bitstreams by stripping the .bit header and
byte-swapping the payload into .bin files.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

SYNC_PATTERN = bytes.fromhex("FFFFFFFF AA995566".replace(" ", ""))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert Xilinx .bit files into byte-swapped .bin files that the "
            "FPGA manager accepts, and copy them into the target directory."
        )
    )
    parser.add_argument(
        "bitstreams",
        nargs="*",
        help="Optional explicit list of .bit files. "
             "If omitted, all *.bit under --src-dir are converted.",
    )
    parser.add_argument(
        "--src-dir",
        default="bitstreams",
        help="Directory to scan for .bit files when none are listed explicitly "
             "(default: %(default)s)",
    )
    parser.add_argument(
        "--dst-dir",
        default="/lib/firmware/bitstreams",
        help="Directory where .bin files are written (default: %(default)s)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing .bin files without prompting.",
    )
    return parser.parse_args()


def find_bitstreams(args: argparse.Namespace) -> list[Path]:
    if args.bitstreams:
        return [Path(p).resolve() for p in args.bitstreams]
    src = Path(args.src_dir).resolve()
    return sorted(src.glob("*.bit"))


def load_bitstream(bit_path: Path) -> bytes:
    data = bit_path.read_bytes()
    idx = data.find(SYNC_PATTERN)
    if idx == -1:
        raise ValueError(f"{bit_path}: sync word AA995566 not found")
    payload = data[idx:]
    if len(payload) % 4 != 0:
        raise ValueError(f"{bit_path}: payload size {len(payload)} is not 32-bit aligned")
    return payload


def byte_swap_words(payload: bytes) -> bytes:
    swapped = bytearray(len(payload))
    for i in range(0, len(payload), 4):
        swapped[i:i + 4] = payload[i:i + 4][::-1]
    return bytes(swapped)


def convert_one(bit_path: Path, dst_dir: Path, force: bool) -> Path:
    payload = load_bitstream(bit_path)
    swapped = byte_swap_words(payload)
    dst_path = dst_dir / (bit_path.stem + ".bin")
    if dst_path.exists() and not force:
        raise FileExistsError(f"{dst_path} exists (use --force to overwrite)")
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst_path.write_bytes(swapped)
    return dst_path


def main() -> int:
    args = parse_args()
    bit_files = find_bitstreams(args)
    if not bit_files:
        print("No .bit files found to convert.", file=sys.stderr)
        return 1

    dst_dir = Path(args.dst_dir).resolve()
    failures = []
    cwd = Path.cwd()

    def pretty(path: Path) -> Path:
        try:
            return path.relative_to(cwd)
        except ValueError:
            return path

    for bit_path in bit_files:
        try:
            dst = convert_one(bit_path, dst_dir, args.force)
            print(f"{pretty(bit_path)} -> {pretty(dst)}")
        except Exception as exc:
            failures.append((bit_path, exc))
            print(f"ERROR: {bit_path}: {exc}", file=sys.stderr)

    if failures:
        print(f"\n{len(failures)} conversion(s) failed.", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
