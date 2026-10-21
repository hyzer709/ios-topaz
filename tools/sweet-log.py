#!/usr/bin/env python3
"""Decode the SWEET persistent console log (kernel patch 0047).

  sweet-log.py AREA.bin [OUT.txt]

AREA.bin is the last (1 + ring/4096) device blocks of the userdata
partition as read by tools/sweet-log-pull.sh: block 0 is the header
("SWEETLOG", u32 version, u32 ring size, u64 total bytes, u64 flush seq),
the ring follows. Byte n of the console stream is at ring offset n % size.
"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
# topaz (UFS, 4 KiB blocks): header at 0, ring at 4096. ginkgo (eMMC, 512-byte
# blocks): header one 512-byte block before the ring, a few KiB into the dump
# (where exactly depends on how the partition end lines up with 4 KiB).
if data[:8] == b'SWEETLOG':
    hdr, blk = 0, 4096
else:
    hdr = next((o for o in range(512, 0x10000, 512) if data[o:o + 8] == b'SWEETLOG'), None)
    if hdr is None:
        sys.exit('no SWEETLOG header (log never flushed, or wrong area)')
    blk = 512
magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', data, hdr)
ring = data[hdr + blk:hdr + blk + size]
if total <= size:
    text = ring[:total]
else:
    start = total % size
    text = ring[start:] + ring[:start]
out = text.decode('utf-8', 'replace').replace('\r', '')
print(f'# version {version}, ring {size}, total {total} bytes, flush #{seq}'
      f'{" (wrapped: oldest bytes lost)" if total > size else ""}', file=sys.stderr)
if len(sys.argv) > 2:
    open(sys.argv[2], 'w').write(out)
else:
    sys.stdout.write(out)
