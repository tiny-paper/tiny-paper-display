#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Find a device, show a picture, then a page drawn in code, close:  python quickstart.py photo.png"""
import sys

import tiny_paper_display as tpd

for dev in tpd.devices():
    print("found", dev.port, dev.board, f"{dev.w}x{dev.h}")

with tpd.open() as d:  # the first device found; closed at the end, also after an error
    d.show(sys.argv[1], fit="fit", dither="floyd")
    page = tpd.Canvas(d.w, d.h)  # white
    page.text(8, 8, "HELLO", 4)
    d.show(page)  # a page drawn in code
    page.rect(8, 48, 96, 32, 0)
    d.show(page)  # the device refreshes only what changed: the black box
