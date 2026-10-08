# SPDX-License-Identifier: MIT
"""Canonical panel fingerprint shared with usbdisp.c and docs/spec/vectors/usb-display.json."""
import struct

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


def fingerprint(png, hello):
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
