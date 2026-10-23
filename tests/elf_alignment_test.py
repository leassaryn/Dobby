"""Regression checks for the build-time 16 KB ELF gate."""
from pathlib import Path
import runpy
import struct
import unittest

check = runpy.run_path(str(Path(__file__).resolve().parents[1] / "scripts/check-elf-alignment.py"))["check"]


def binary(alignment=16384, address=0, file_offset=0, kind=1):
    data = bytearray(120)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", data, 64, kind, 5, file_offset, address, 0, 0, 16384, alignment)
    return data


class AlignmentTest(unittest.TestCase):
    def test_supported(self):
        for alignment in (16384, 65536):
            self.assertEqual(check(binary(alignment)), 1)

    def test_bad_alignment(self):
        for alignment in (0, 4096, 24576):
            with self.assertRaises(ValueError):
                check(binary(alignment))

    def test_incongruent(self):
        with self.assertRaises(ValueError):
            check(binary(address=4096))

    def test_truncated(self):
        data = binary()
        for size in range(len(data)):
            with self.assertRaises(ValueError):
                check(data[:size])

    def test_invalid_extent(self):
        with self.assertRaises(ValueError):
            check(binary(file_offset=16384))

    def test_no_load(self):
        with self.assertRaises(ValueError):
            check(binary(kind=2))


if __name__ == "__main__":
    unittest.main()
