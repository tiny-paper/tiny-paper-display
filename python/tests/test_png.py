# SPDX-License-Identifier: MIT
import pytest

from conftest import png
from tinypaper_display.png_image import decode_png


@pytest.mark.parametrize("filt,encoded", [(0, b"\x0a\x14"), (1, b"\x0a\x0a"),
                                        (2, b"\x09\x12"), (3, b"\x0a\x0e"), (4, b"\x09\x0a")])
def test_all_row_filters(filt, encoded):
    picture = png(2, 2, b"\0\x01\x02" + bytes([filt]) + encoded, color=0)
    assert decode_png(picture)[2] == [(1,) * 3, (2,) * 3, (10,) * 3, (20,) * 3]


@pytest.mark.parametrize("bits,packed", [(1, b"\x40"), (2, b"\x30"), (4, b"\x0f"), (8, b"\0\xff")])
def test_gray_bit_depths(bits, packed):
    assert decode_png(png(2, 1, b"\0" + packed, color=0, bits=bits))[2] == [(0,) * 3, (255,) * 3]


def test_alpha_and_indexed_transparency():
    assert decode_png(png(1, 1, b"\0\0\0\0\0", color=6))[2] == [(255,) * 3]
    assert decode_png(png(1, 1, b"\0\0\x80", color=4))[2] == [(127,) * 3]
    palette = b"\xff\0\0\0\0\0"
    assert decode_png(png(2, 1, b"\0\x01", color=3, bits=4, palette=palette,
                          transparency=b"\xff\0"))[2] == [(255, 0, 0), (255,) * 3]


@pytest.mark.parametrize("picture,match", [
    (b"invalid", "not a PNG"),
    (png(1, 1, b"\0\0\0\0", interlace=1), "encoding"),
    (png(1, 1, b"\0\0\0\0\0"), "decoded size"),
    (png(1, 1, b"\5\0\0\0"), "row filter"),
    (png(99999, 99999, b""), "dimensions"),
    (png(1, 1, b"\0\x03", color=3, palette=b"\0\0\0"), "palette index"),
    (png(1, 1, b"\0\0", color=3), "no palette"),
])
def test_bad_png_clean_error(picture, match):
    with pytest.raises(ValueError, match=match):
        decode_png(picture)


def test_truncated_and_bad_crc():
    picture = png(1, 1, b"\0\0\0\0")
    with pytest.raises(ValueError, match="truncated"):
        decode_png(picture[:-3])
    damaged = bytearray(picture)
    damaged[20] ^= 1
    with pytest.raises(ValueError, match="CRC"):
        decode_png(damaged)
