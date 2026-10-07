# SPDX-License-Identifier: MIT
# Decode bounded PNGs to RGB pixels for USB picture conversion.
"""Small bounded PNG reader, no image-library dependency.

Non-interlaced gray/indexed 1/2/4/8-bit and RGB/GA/RGBA 8-bit only.
Alpha is composited over white. Other PNG encodings fail explicitly.
"""
import struct
import zlib

MAX_BYTES = 16 * 1024 * 1024
MAX_PIXELS = 4 * 1024 * 1024


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def decode_png(data):
    if len(data) > MAX_BYTES or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG or over 16 MiB")
    off, header, palette, transparency = 8, None, None, b""
    compressed = bytearray()
    ended = False
    while off < len(data):
        if off + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        n = int.from_bytes(data[off:off + 4], "big")
        kind = data[off + 4:off + 8]
        end = off + 8 + n
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        value = data[off + 8:end]
        if zlib.crc32(kind + value) != int.from_bytes(data[end:end + 4], "big"):
            raise ValueError("PNG chunk CRC mismatch")
        if header is None and kind != b"IHDR":
            raise ValueError("PNG must start with IHDR")
        if kind == b"IHDR":
            if header is not None or n != 13:
                raise ValueError("invalid PNG header")
            header = struct.unpack(">IIBBBBB", value)
        elif kind == b"PLTE":
            if not n or n % 3 or n > 768:
                raise ValueError("invalid PNG palette")
            palette = [tuple(value[i:i + 3]) for i in range(0, n, 3)]
        elif kind == b"tRNS":
            transparency = value
        elif kind == b"IDAT":
            compressed.extend(value)
        elif kind == b"IEND":
            if n or end + 4 != len(data):
                raise ValueError("invalid PNG end")
            ended = True
            break
        elif not kind[0] & 32:
            raise ValueError("unsupported critical PNG chunk")
        off = end + 4
    if not ended or header is None:
        raise ValueError("PNG has no end/header")
    w, h, bits, color, compression, filtering, interlace = header
    if not w or not h or w * h > MAX_PIXELS:
        raise ValueError("PNG dimensions exceed 4 megapixels")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if (not channels or bits not in (1, 2, 4, 8) or (color not in (0, 3) and bits != 8)
            or compression or filtering or interlace):
        raise ValueError("unsupported PNG encoding: use non-interlaced 8-bit RGB/RGBA")
    if color == 3 and palette is None:
        raise ValueError("indexed PNG has no palette")
    if transparency and ((color == 0 and len(transparency) != 2)
                         or (color == 2 and len(transparency) != 6)
                         or (color == 3 and len(transparency) > len(palette))
                         or color in (4, 6)):
        raise ValueError("invalid PNG transparency")
    stride = (w * channels * bits + 7) // 8
    expected = h * (stride + 1)
    decompressor = zlib.decompressobj()
    try:
        raw = decompressor.decompress(compressed, expected + 1)
    except zlib.error as exc:
        raise ValueError(f"invalid PNG compressed data: {exc}") from exc
    if len(raw) != expected or not decompressor.eof or decompressor.unused_data:
        raise ValueError("wrong PNG decoded size")
    pixels, prior = [], bytearray(stride)
    step = max(1, (channels * bits + 7) // 8)
    transparent = tuple(int.from_bytes(transparency[i:i + 2], "big")
                        for i in range(0, len(transparency), 2)) if color in (0, 2) else ()
    for y in range(h):
        base = y * (stride + 1)
        filt = raw[base]
        if filt > 4:
            raise ValueError("invalid PNG row filter")
        row = bytearray(raw[base + 1:base + stride + 1])
        for i in range(stride):
            a, b, c = row[i - step] if i >= step else 0, prior[i], prior[i - step] if i >= step else 0
            predictor = (0, a, b, (a + b) // 2, _paeth(a, b, c))[filt]
            row[i] = (row[i] + predictor) & 255
        for x in range(w):
            alpha = 255
            if color in (0, 3):
                bit = x * bits
                sample = (row[bit // 8] >> (8 - bits - bit % 8)) & ((1 << bits) - 1)
                if color == 0:
                    rgb = (sample * 255 // ((1 << bits) - 1),) * 3
                    if transparent == (sample,):
                        alpha = 0
                else:
                    if sample >= len(palette):
                        raise ValueError("PNG palette index out of range")
                    rgb = palette[sample]
                    alpha = transparency[sample] if sample < len(transparency) else 255
            else:
                sample = tuple(row[x * channels:(x + 1) * channels])
                rgb = sample[:3] if color in (2, 6) else (sample[0],) * 3
                if color in (4, 6):
                    alpha = sample[-1]
                elif sample == transparent:
                    alpha = 0
            pixels.append(tuple((v * alpha + 255 * (255 - alpha) + 127) // 255 for v in rgb))
        prior = row
    return w, h, pixels
