#!/usr/bin/env python3
"""Fails if any dynamic relocation targets a non-writable ELF segment.

hbloader maps NRO segment 1 (.rodata) read-only and only then libnx applies
RELATIVE relocations, so a relocation into .rodata crashes the process.
"""
import re
import subprocess
import sys

READELF = sys.argv[1] if len(sys.argv) > 1 else "readelf"
ELF = sys.argv[2] if len(sys.argv) > 2 else "build/scswitch.elf"

segs = []
out = subprocess.check_output([READELF, "-lW", ELF], text=True)
for line in out.splitlines():
    m = re.match(r"\s*LOAD\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+(\S+)\s", line)
    if m:
        off, vaddr, filesz, flags = m.groups()
        segs.append((int(vaddr, 16), int(vaddr, 16) + int(filesz, 16), flags))
if len(segs) < 3:
    print("check_relocs: expected 3 LOAD segments, found", len(segs), file=sys.stderr)
    sys.exit(1)

bad = []
out = subprocess.check_output([READELF, "-rW", ELF], text=True)
for line in out.splitlines():
    m = re.match(r"\s*([0-9a-f]{8,16})\s+[0-9a-f]+\s+(\S+)", line)
    if not m:
        continue
    offset, kind = int(m.group(1), 16), m.group(2)
    if kind not in ("R_AARCH64_RELATIVE", "R_AARCH64_NONE"):
        print("check_relocs: unsupported relocation", kind, "at", hex(offset), file=sys.stderr)
        sys.exit(1)
    if kind == "R_AARCH64_NONE":
        continue
    seg = next((s for s in segs if s[0] <= offset < s[1]), None)
    if seg is None:
        bad.append((offset, "outside all segments"))
    elif "W" not in seg[2]:
        bad.append((offset, "in %s segment" % seg[2]))

if bad:
    for off, why in bad[:10]:
        print("check_relocs: relocation at %#x %s" % (off, why), file=sys.stderr)
    print("check_relocs: %d bad relocation(s)" % len(bad), file=sys.stderr)
    sys.exit(1)

print("check_relocs: OK (%d segments, all relocations writable)" % len(segs))
