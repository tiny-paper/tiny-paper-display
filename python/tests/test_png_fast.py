# SPDX-License-Identifier: MIT
"""The whole-row PNG reader (png_image.decode_rgba) gives what the per-pixel reader (decode_png) gives."""
import random
import struct

import pytest

from conftest import png
from tiny_paper_display import png_image


def filtered(rows, stride, bpp, filt):
    """Rows of raw bytes encoded with PNG filter `filt` (0..4)."""
    out, prior = bytearray(), bytes(stride)
    for row in rows:
        line = bytearray()
        for i, v in enumerate(row):
            a, b, c = row[i - bpp] if i >= bpp else 0, prior[i], prior[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) // 2, png_image._paeth(a, b, c))[filt]
            line.append((v - pred) & 255)
        out += bytes([filt]) + line
        prior = row
    return bytes(out)


@pytest.mark.parametrize("filt", range(5))
@pytest.mark.parametrize("color,channels", [(0, 1), (2, 3), (4, 2), (6, 4), (3, 1)])
def test_fast_reader_equals_the_pixel_reader(color, channels, filt):
    rnd = random.Random(color * 10 + filt)
    w, h = 13, 7
    stride = w * channels
    rows = [bytes(rnd.randrange(4 if color == 3 else 256) for _ in range(stride)) for _ in range(h)]
    pal = bytes(rnd.randrange(256) for _ in range(12)) if color == 3 else None
    trns = bytes([0, 100, 255, 7]) if color == 3 else None
    data = png(w, h, filtered(rows, stride, channels, filt), color=color, palette=pal, transparency=trns)
    fw, fh, rgba = png_image.decode_rgba(data)
    ow, oh, pixels = png_image.decode_png(data, keep_alpha=True)
    assert (fw, fh) == (ow, oh) and rgba == bytes(v for p in pixels for v in p)


def test_fast_reader_refuses_what_the_pixel_reader_refuses():
    data = png(2, 1, b"\x00\x05\x09", color=3, bits=8, palette=bytes(6))
    with pytest.raises(ValueError, match="palette index"):
        png_image.decode_rgba(data)
    with pytest.raises(ValueError):
        png_image.decode_rgba(png(1, 1, b"\x09\x00\x00\x00"))


@pytest.mark.parametrize("depth", [1, 2, 4])
@pytest.mark.parametrize("w", [8, 13, 21])
def test_fingerprint_without_decoding_equals_the_slow_path(monkeypatch, depth, w):
    from tiny_paper_display import display_fingerprint as fp, display_fw
    rnd = random.Random(w + depth)
    h, stride = 5, (w * depth + 7) // 8
    rows = bytes(rnd.randrange(256) for _ in range(stride * h))
    rows = b"".join(r[:-1] + bytes([r[-1] & (0xff << (-w * depth % 8) & 0xff)]) for r in (rows[i * stride:(i + 1) * stride] for i in range(h)))
    data = display_fw.write_png(w, h, depth, rows)
    fast = fp.fingerprint(data, {})
    assert fp._gray_rows(data) is not None
    monkeypatch.setattr(fp, "_gray_rows", lambda png: None)
    assert fp.fingerprint(data, {}) == fast


def test_fingerprint_of_a_filtered_gray_png_is_still_right():
    from tiny_paper_display import display_fingerprint as fp, display_fw
    rows = bytes([0x0F, 0xF0, 0xAA, 0x55, 0x12, 0x34])
    plain = display_fw.write_png(8, 6, 1, rows)
    stride = 1
    data = png(8, 6, filtered([rows[i:i + 1] for i in range(6)], stride, 1, 2), color=0, bits=1)
    assert fp._gray_rows(data) is None, "a filtered row is not taken as packed rows"
    assert fp.fingerprint(data, {}) == fp.fingerprint(plain, {})
