#!/usr/bin/env python3
"""Packs an app folder into one zlib-compressed stream for the payload.

usage: pack-app.py <app folder> <out.pack> <out-id.txt>

Stream (before compression): for each file, u32 path length, the path
(relative, '/'-separated), u64 size, the bytes; then a u32 zero.
The id file gets a short hash of the stream, so GameVault rewrites the
home-screen app only when it changed.
"""
import hashlib
import os
import struct
import sys
import zlib


def main():
    src, out, id_out = sys.argv[1], sys.argv[2], sys.argv[3]
    raw = bytearray()
    files = []
    for top, dirs, names in os.walk(src):
        dirs.sort()
        for name in sorted(names):
            full = os.path.join(top, name)
            files.append((os.path.relpath(full, src).replace(os.sep, '/'), full))
    for rel, full in files:
        data = open(full, 'rb').read()
        path = rel.encode()
        raw += struct.pack('<I', len(path)) + path + struct.pack('<Q', len(data)) + data
    raw += struct.pack('<I', 0)
    packed = zlib.compress(bytes(raw), 9)
    open(out, 'wb').write(packed)
    open(id_out, 'w').write(hashlib.sha256(raw).hexdigest()[:16] + '\n')
    print(f'packed {len(files)} files: {len(raw)/1e6:.1f} MB -> {len(packed)/1e6:.1f} MB')


if __name__ == '__main__':
    main()
