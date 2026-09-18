#!/usr/bin/env python3
"""Unit tests for the radiometric raw-frame helper."""

from __future__ import annotations

import array
import tempfile
import unittest
from pathlib import Path

import raw_to_pgm


class RawTemperatureTest(unittest.TestCase):
    def test_kelvin_x100_conversion(self) -> None:
        self.assertAlmostEqual(raw_to_pgm.kelvin_x100_to_celsius(27315), 0.0)
        self.assertAlmostEqual(raw_to_pgm.kelvin_x100_to_celsius(30000), 26.85)
        self.assertAlmostEqual(raw_to_pgm.kelvin_x100_to_celsius(31015), 37.0)

    def test_little_endian_frame_read(self) -> None:
        values = array.array("H", [30000] * (raw_to_pgm.WIDTH * raw_to_pgm.HEIGHT))
        host_little = array.array("H", [1]).tobytes()[0] == 1
        if not host_little:
            values.byteswap()

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.y16"
            path.write_bytes(values.tobytes())
            pixels = raw_to_pgm.read_pixels(path, "little")

        self.assertEqual(len(pixels), raw_to_pgm.WIDTH * raw_to_pgm.HEIGHT)
        self.assertEqual(min(pixels), 30000)
        self.assertEqual(max(pixels), 30000)

    def test_wrong_frame_size_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "short.y16"
            path.write_bytes(b"\0" * 10)
            with self.assertRaises(SystemExit):
                raw_to_pgm.read_pixels(path, "little")


if __name__ == "__main__":
    unittest.main()
