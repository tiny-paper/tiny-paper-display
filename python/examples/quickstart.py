#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find a device, show a picture, update a rectangle, close:  python quickstart.py photo.png"""
import sys

import tiny_paper_display as tpd

for dev in tpd.devices():
    print("found", dev.port, dev.board, f"{dev.w}x{dev.h}")

with tpd.open() as d:  # the first device found; closed at the end, also after an error
    d.show(sys.argv[1], fit="fit", dither="floyd")
    box = tpd.Canvas(48, 24, 0)  # a black box ...
    box.text(6, 5, "HELLO", 1, 15)
    d.area(box, 8, 8)  # ... in the top left corner, without redrawing the rest (x a multiple of 8)
