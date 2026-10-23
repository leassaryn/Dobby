#!/usr/bin/env python3
"""Compare actual internal assembler output with LLVM MC; neither is reimplemented here."""
import argparse
import re
import subprocess
p = argparse.ArgumentParser()
p.add_argument('emitter')
p.add_argument('--llvm-mc', default='llvm-mc')
a = p.parse_args()
rows = [line.split('|', 1) for line in subprocess.check_output([a.emitter], text=True).splitlines()]
assembly = '\n'.join(row[1] for row in rows) + '\n'
r = subprocess.run([a.llvm_mc, '-triple=aarch64', '-show-encoding'], input=assembly, text=True, capture_output=True, check=True)
encodings = re.findall(r'encoding: \[([^\]]+)\]', r.stdout)
assert len(encodings) == len(rows), (len(encodings), len(rows))
for (expected, instruction), encoded in zip(rows, encodings):
    actual = int.from_bytes(bytes(int(x.strip(), 16) for x in encoded.split(',')), 'little')
    assert actual == int(expected, 16), (instruction, expected, hex(actual))
print(f'PASS {len(rows)} assembler encodings against LLVM MC; invalid operand tests passed')
