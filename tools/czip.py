#!/usr/bin/env python3
"""czip - compress a file for the cOSmOS root file system (CZIP container).

  python3 tools/czip.py input output

Layout: "CZIP", u32 original size (little endian), zlib stream (level 9).
cOSmOS programs unpack it with src/user/inflate.c (vplay for videos, the DOOM port for
the WAD), so large data fits in the 4 MiB EXT2 region.
"""
import struct
import sys
import zlib

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)
data = open(sys.argv[1], 'rb').read()
packed = b'CZIP' + struct.pack('<I', len(data)) + zlib.compress(data, 9)
open(sys.argv[2], 'wb').write(packed)
print('czip: %s -> %s: %d -> %d bytes (%.0f%%)' % (sys.argv[1], sys.argv[2], len(data), len(packed),
                                                 100.0 * len(packed) / max(len(data), 1)))
