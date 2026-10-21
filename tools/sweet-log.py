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
# blocks): the area is 1 + ring/512 blocks, so in the 4 KiB-block dump of
# sweet-log-pull.sh the header sits 3584 bytes in and the ring follows 512 later.
for hdr, blk in ((0, 4096), (3584, 512)):
    if data[hdr:hdr + 8] == b'SWEETLOG':
        break
else:
    sys.exit('no SWEETLOG header (log never flushed, or wrong area)')
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
