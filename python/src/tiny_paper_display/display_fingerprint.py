# SPDX-License-Identifier: MIT
"""Canonical panel fingerprint shared with usbdisp.c and docs/spec/vectors/usb-display.json."""
import struct
import zlib

from .png_image import decode_png


def fingerprint_rows(w, h, depth, rows, color=False):
    value = 0xcbf29ce484222325
    stride = (w * depth + 7) // 8
    padding = (8 - w * depth % 8) % 8
    for b in struct.pack('>HHBB', w, h, depth, color):
        value = ((value ^ b) * 0x100000001b3) & 0xffffffffffffffff
    for y in range(h):
        for x in range(stride):
            b = rows[y * stride + x]
            if x + 1 == stride:
                b &= (0xff << padding) & 0xff
            value = ((value ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return f'{value:016x}'


def _gray_rows(png):
    """The packed rows of a non-interlaced gray 1, 2 or 4 bit PNG whose rows all use filter None (what this package and
    the web page write), without decoding it to pixels and packing them again; None for any other PNG."""
    if len(png) < 33 or png[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h, bits, ctype, comp, filt, lace = struct.unpack(">IIBBBBB", png[16:29])
    if ctype != 0 or bits not in (1, 2, 4) or comp or filt or lace or not w or not h:
        return None
    data, at = bytearray(), 8
    while at + 8 <= len(png):
        n, kind = struct.unpack(">I4s", png[at:at + 8])
        if kind == b"IDAT":
            data += png[at + 8:at + 8 + n]
        elif kind != b"IHDR" and not kind[0] & 32:  # a critical chunk we do not know (PLTE, tRNS ...): the slow path
            if kind != b"IEND":
                return None
        at += 12 + n
    stride = (w * bits + 7) // 8
    try:
        raw = zlib.decompress(bytes(data))
    except zlib.error:
        return None
    if len(raw) != (stride + 1) * h or any(raw[y * (stride + 1)] for y in range(h)):
        return None
    return w, h, bits, b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def fingerprint(png, hello):
    fast = None if hello.get("colors") else _gray_rows(png)
    if fast:
        w, h, bits, rows = fast
        return fingerprint_rows(w, h, bits, rows)
    w, h, pixels = decode_png(png, gray_transparency=False)
    bits, ctype = png[24:26]
    color = bool(hello.get('colors'))
    if (not color and (ctype != 0 or bits not in (1, 2, 4))
            or color and not (ctype == 3 or ctype == 0 and bits == 1)):
        raise ValueError('PNG is not at a supported panel depth')
    depth = 2 if color else bits
    codes = {(0, 0, 0): 0, (255, 0, 0): 1, (255, 255, 0): 2, (255, 255, 255): 3}
    stride = (w * depth + 7) // 8
    rows = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            rgb = pixels[y * w + x]
            sample = codes[rgb] if color else rgb[0] * ((1 << depth) - 1) // 255
            bit = x * depth
            rows[y * stride + bit // 8] |= sample << (8 - depth - bit % 8)
    return fingerprint_rows(w, h, depth, rows, color)
