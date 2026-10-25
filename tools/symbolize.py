#!/usr/bin/env python3
"""Resolve raw return PCs against the original ELF modules, outside the target."""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import struct
import subprocess
import sys


def resolve(text):
    lines = text.splitlines()
    groups = defaultdict(dict)
    answers = {}
    for i, line in enumerate(lines):
        match = re.fullmatch(r"FRAME pc=(0x[0-9a-f]+) base=(0x[0-9a-f]+|\(nil\)) symbol=\S+ module=(.*)", line)
        if not match:
            continue
        pc, base, module = match.groups()
        try:
            with open(module, "rb") as f:
                header = f.read(20)
            if header[:4] != b"\x7fELF" or header[5] not in (1, 2):
                raise ValueError("not an ELF module")
            kind = struct.unpack("<H" if header[5] == 1 else ">H", header[16:18])[0]
            if kind not in (2, 3):
                raise ValueError("unsupported ELF type")
            if kind == 3 and base == "(nil)":
                raise ValueError("missing module base")
            # Return addresses point after the call. ET_EXEC uses virtual PCs;
            # PIE/shared ELF (ET_DYN) uses PCs relative to dladdr's module base.
            address = int(pc, 16) - (int(base, 16) if kind == 3 else 0) - 1
            groups[module].setdefault(hex(address), []).append(i)
        except (OSError, ValueError, IndexError, struct.error) as error:
            answers[i] = f"  SOURCE unavailable: {error}"
    for module, addresses in groups.items():
        keys = list(addresses)
        for start in range(0, len(keys), 256):
            batch = keys[start:start + 256]
            try:
                proc = subprocess.run(["addr2line", "-f", "-C", "-e", module, *batch],
                                      text=True, capture_output=True, timeout=30, check=True)
                output = proc.stdout.splitlines()
                if len(output) != len(batch) * 2:
                    raise ValueError("unexpected addr2line output")
                for j, address in enumerate(batch):
                    for index in addresses[address]:
                        answers[index] = f"  SOURCE {output[j * 2]} at {output[j * 2 + 1]}"
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                for address in batch:
                    for index in addresses[address]:
                        answers[index] = f"  SOURCE unavailable: {error}"
    for i, line in enumerate(lines):
        yield line
        if i in answers:
            yield answers[i]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    args = parser.parse_args()
    text = args.snapshot.read_text(errors="replace")
    # A source line is only meaningful for the exact module build captured.
    for line in resolve(text):
        print(line)
    if not re.search(r"(?:^|\n)END seq=\d+\s*\Z", text):
        print("Warning: snapshot file ends without a complete END marker", file=sys.stderr)


if __name__ == "__main__":
    main()
