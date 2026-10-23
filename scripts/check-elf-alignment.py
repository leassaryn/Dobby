#!/usr/bin/env python3
"""Check 64-bit ELF load segments for Android 16 KB compatibility (not a runtime test)."""
import argparse
from pathlib import Path
import struct


def check(data):
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("expected a little-endian ELF64 binary, not a static archive")
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    if entry_size < 56 or not count or count == 0xffff:
        raise ValueError("invalid or unsupported program header table")
    if offset > len(data) or count * entry_size > len(data) - offset:
        raise ValueError("truncated program header table")
    loads = 0
    for index in range(count):
        kind, _, file_offset, address, _, file_size, memory_size, alignment = struct.unpack_from(
            "<IIQQQQQQ", data, offset + index * entry_size)
        if kind != 1:  # PT_LOAD
            continue
        loads += 1
        if alignment < 16384 or alignment & (alignment - 1):
            raise ValueError(f"PT_LOAD {index}: alignment {alignment} is not a power of two >= 16384")
        if address % alignment != file_offset % alignment:
            raise ValueError(f"PT_LOAD {index}: address and file offset are incongruent")
        if file_size > memory_size or file_offset > len(data) or file_size > len(data) - file_offset:
            raise ValueError(f"PT_LOAD {index}: invalid segment extent")
    if not loads:
        raise ValueError("no PT_LOAD segments")
    return loads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    failed = False
    for path in args.files:
        try:
            count = check(path.read_bytes())
            print(f"PASS {path}: {count} load segments compatible with 16 KB alignment")
        except (OSError, ValueError) as error:
            print(f"FAIL {path}: {error}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
