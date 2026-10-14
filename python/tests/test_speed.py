# SPDX-License-Identifier: MIT
"""Times of the host steps on a panel the size of the TRMNL X (1872x1404), printed (run with -s). The limits are asserted
unless the environment variable CI is set (a slow shared machine only prints)."""
import os
import random
import time

import pytest

import tiny_paper_display as tpd
from tiny_paper_display import images, picture

W, H = 1872, 1404
STRICT = not os.environ.get("CI")


def timed(label, limit, work):
    t0 = time.perf_counter()
    out = work()
    took = time.perf_counter() - t0
    print(f"speed {label}: {took:.2f} s (limit {limit} s)")
    if STRICT:
        assert took < limit, f"{label} took {took:.2f} s, limit {limit} s"
    return out


@pytest.fixture(scope="module")
def source():
    rnd = random.Random(3)
    rgba = bytearray((bytes(rnd.randrange(256) for _ in range(4096)) * (W * H * 4 // 4096 + 1))[:W * H * 4])
    rgba[3::4] = b"\xff" * (W * H)
    return picture.Source(W, H, bytes(rgba))


def test_fit_and_4_bit_levels_without_dithering(source):
    timed("fit + 4 bit, no dither", 1.0, lambda: picture.prepare(source, W, H, depth=4, dither="none"))


def test_floyd_steinberg(source):
    timed("fit + 4 bit, Floyd-Steinberg", 3.0, lambda: picture.prepare(source, W, H, depth=4, dither="floyd"))


@pytest.mark.parametrize("depth", [1, 2, 4])
def test_canvas_to_png(depth):
    c = tpd.Canvas(W, H)
    c.text(10, 10, "SPEED", 8)
    timed(f"canvas -> {depth} bit PNG", 0.5, lambda: c.png(depth))


def test_decode_a_png(tmp_path):
    from conftest import png
    rnd = random.Random(4)
    line = bytes(rnd.randrange(256) for _ in range(W * 3))
    data = png(W, H, b"".join(b"\x00" + line for _ in range(H)))
    timed("decode an RGB PNG (filter none)" + (" with Pillow" if images.have_pillow() else ""), 1.0, lambda: images.from_bytes(data))
