# SPDX-License-Identifier: MIT
import struct
import sys
import zlib
from pathlib import Path



def png(w, h, rows, color=2, bits=8, palette=None, transparency=None, interlace=0):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bits, color, 0, 0, interlace))
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if transparency is not None:
        out += chunk(b"tRNS", transparency)
    return out + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


class FakePort:
    def __init__(self, respond):
        self.respond, self.rx, self.writes = respond, bytearray(), []

    def write(self, data):
        self.writes.append(data)
        self.rx.extend(self.respond(data))
        return len(data)

    def read(self, size):
        part = bytes(self.rx[:3])  # every reply fragmented, including its sync/header
        del self.rx[:3]
        return part
