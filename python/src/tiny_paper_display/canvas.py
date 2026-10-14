# SPDX-License-Identifier: MIT
# Gray-level drawing in code, moved from examples/test_page.py.
"""Canvas: draw rectangles, frames, text and checkers in 16 gray levels, show it on a display (Device.show / .area)."""
from . import display_fw
from .usb_display import DisplayError

# 5x7 glyphs, one byte per column, low bit at the top (the classic LCD font).
FONT = {
    " ": "0000000000", "%": "2313086462", "(": "001c224100", ")": "0041221c00", ",": "0050300000",
    "-": "0808080808", ".": "0060600000", "/": "2010080402", ":": "0036360000", "=": "1414141414",
    "0": "3e5149453e", "1": "00427f4000", "2": "4261514946", "3": "2141454b31", "4": "1814127f10",
    "5": "2745454539", "6": "3c4a494930", "7": "0171090503", "8": "3649494936", "9": "064949291e",
    "A": "7e1111117e", "B": "7f49494936", "C": "3e41414122", "D": "7f4141221c", "E": "7f49494941",
    "F": "7f09090101", "G": "3e41415132", "H": "7f0808087f", "I": "00417f4100", "J": "2040413f01",
    "K": "7f08142241", "L": "7f40404040", "M": "7f0204027f", "N": "7f0408107f", "O": "3e4141413e",
    "P": "7f09090906", "Q": "3e4151215e", "R": "7f09192946", "S": "4649494931", "T": "01017f0101",
    "U": "3f4040403f", "V": "1f2040201f", "W": "7f2018207f", "X": "6314081463", "Y": "0304780403",
    "Z": "6151494543",
}
BLACK, WHITE = 0, 15


class Canvas:
    """Gray levels 0 (black) to 15 (white), packed to 1, 2 or 4 bits for the device."""

    def __init__(self, w, h, level=WHITE):
        if w < 1 or h < 1:
            raise DisplayError("a canvas has at least one pixel")
        self.w, self.h = w, h
        self.px = bytearray([level]) * (w * h)

    def rect(self, x, y, w, h, level):
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, self.w), min(y + h, self.h)
        for yy in range(y0, y1):
            self.px[yy * self.w + x0:yy * self.w + x1] = bytes([level]) * max(0, x1 - x0)

    def frame(self, x, y, w, h, level, t):
        for r in ((x, y, w, t), (x, y + h - t, w, t), (x, y, t, h), (x + w - t, y, t, h)):
            self.rect(*r, level)

    def text(self, x, y, s, scale, level=BLACK):
        """Upper-case letters, digits and a few signs, 5x7 at `scale`; returns the x after the text."""
        for ch in s.upper():
            cols = bytes.fromhex(FONT.get(ch, FONT[" "]))
            for cx, col in enumerate(cols):
                for cy in range(7):
                    if col >> cy & 1:
                        self.rect(x + cx * scale, y + cy * scale, scale, scale, level)
            x += 6 * scale
        return x

    def checker(self, x, y, w, h, cell):
        for yy in range(max(y, 0), min(y + h, self.h)):
            for xx in range(max(x, 0), min(x + w, self.w)):
                self.px[yy * self.w + xx] = BLACK if (xx // cell + yy // cell) % 2 else WHITE

    def crop(self, x, y, w, h):
        """A new Canvas of this rectangle (for Device.area)."""
        if x < 0 or y < 0 or w < 1 or h < 1 or x + w > self.w or y + h > self.h:
            raise DisplayError("the rectangle is not inside the canvas")
        out = Canvas(w, h)
        for j in range(h):
            out.px[j * w:(j + 1) * w] = self.px[(y + j) * self.w + x:(y + j) * self.w + x + w]
        return out

    def png(self, depth, x=0, y=0, w=None, h=None):
        """The area as a gray PNG of `depth` bits (levels rounded to what the depth has)."""
        w, h = w or self.w, h or self.h
        top = (1 << depth) - 1
        per = 8 // depth
        rows = bytearray()
        for yy in range(y, y + h):
            line = self.px[yy * self.w + x:yy * self.w + x + w]
            for i in range(0, w, per):
                byte = 0
                for v in line[i:i + per]:
                    byte = byte << depth | (v * top + 7) // 15
                rows.append(byte << depth * (per - len(line[i:i + per])))
        return display_fw.write_png(w, h, depth, bytes(rows))

    def rgba(self):
        """The canvas as RGBA bytes (level * 17 on each channel), for the picture preparation."""
        return b"".join(bytes((v * 17,) * 3 + (255,)) for v in self.px)
