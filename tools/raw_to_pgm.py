#!/usr/bin/env python3
"""Convert one 160x120 uint16 Lepton raw frame to optional 16-bit PGM."""

from __future__ import annotations

import argparse
import array
from pathlib import Path


WIDTH = 160
HEIGHT = 120
FRAME_BYTES = WIDTH * HEIGHT * 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Inspect a 160x120 uint16 Lepton raw frame and optionally write 16-bit PGM."
    )
    parser.add_argument("input", type=Path, help="Input .gray frame")
    parser.add_argument("--output", "-o", type=Path, help="Output 16-bit PGM path")
    parser.add_argument(
        "--endian",
        choices=("little", "big"),
        default="little",
        help="Input uint16 byte order. Default: little.",
    )
    return parser.parse_args()


def read_pixels(path: Path, endian: str) -> array.array:
    data = path.read_bytes()
    if len(data) != FRAME_BYTES:
        raise SystemExit(f"{path}: expected {FRAME_BYTES} bytes, got {len(data)}")

    pixels = array.array("H")
    pixels.frombytes(data)
    host_little = array.array("H", [1]).tobytes()[0] == 1
    if (endian == "little") != host_little:
        pixels.byteswap()
    return pixels


def write_pgm(path: Path, pixels: array.array) -> None:
    # PGM P5 with maxval > 255 stores samples big-endian.
    out = array.array("H", pixels)
    host_little = array.array("H", [1]).tobytes()[0] == 1
    if host_little:
        out.byteswap()
    header = f"P5\n{WIDTH} {HEIGHT}\n65535\n".encode("ascii")
    path.write_bytes(header + out.tobytes())


def main() -> None:
    args = parse_args()
    pixels = read_pixels(args.input, args.endian)
    print(f"file={args.input}")
    print(f"width={WIDTH}")
    print(f"height={HEIGHT}")
    print(f"endian={args.endian}")
    print(f"min={min(pixels)}")
    print(f"max={max(pixels)}")
    if args.output:
        write_pgm(args.output, pixels)
        print(f"pgm={args.output}")


if __name__ == "__main__":
    main()
