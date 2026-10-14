#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A test page drawn in code, then an area update in every refresh mode the device offers, at every gray depth.

    python test_page.py /dev/ttyACM0            (or COM5)
    python test_page.py --bluetooth NAME
    python test_page.py /dev/ttyACM0 --depth 1 --keep

Per depth the device reports: the page as a full refresh, then one numbered box per mode (device's choice, partial,
partial_all, inverse, passes 1 and 2), each sent as a rectangle. At the end the page is drawn once more with every
timing in it and the same table is printed. Only the package itself is needed (no Pillow)."""
import argparse
import sys
import time

from tiny_paper_display import display_fw
from tiny_paper_display.usb_display import Display, DisplayError, open_port

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
        self.w, self.h = w, h
        self.px = bytearray([level]) * (w * h)

    def rect(self, x, y, w, h, level):
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, self.w), min(y + h, self.h)
        for yy in range(y0, y1):
            self.px[yy * self.w + x0:yy * self.w + x1] = bytes([level]) * (x1 - x0)

    def frame(self, x, y, w, h, level, t):
        for r in ((x, y, w, t), (x, y + h - t, w, t), (x, y, t, h), (x + w - t, y, t, h)):
            self.rect(*r, level)

    def text(self, x, y, s, scale, level=BLACK):
        for ch in s.upper():
            cols = bytes.fromhex(FONT.get(ch, FONT[" "]))
            for cx, col in enumerate(cols):
                for cy in range(7):
                    if col >> cy & 1:
                        self.rect(x + cx * scale, y + cy * scale, scale, scale, level)
            x += 6 * scale
        return x

    def checker(self, x, y, w, h, cell):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.px[yy * self.w + xx] = BLACK if (xx // cell + yy // cell) % 2 else WHITE

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


def draw_page(c, hello, depth, slots, results):
    s = max(2, c.w // 400)  # text scale: 7 * s px tall
    pad = 4 * s
    c.rect(0, 0, c.w, c.h, WHITE)
    c.rect(0, 0, c.w, 11 * s, BLACK)
    c.text(pad, 2 * s, f"TINY-PAPER DISPLAY TEST PAGE  {hello.get('board', '')}  {c.w}X{c.h}  {depth} BIT", s, WHITE)
    top = 14 * s
    wedge_h = c.h // 8
    step = c.w // 16
    for i in range(16):
        c.rect(i * step, top, step, wedge_h, i)
        c.text(i * step + pad, top + wedge_h + s, str(i), s)
    c.frame(0, top, step * 16, wedge_h, BLACK, max(1, s // 2))
    y = top + wedge_h + 10 * s
    third = c.w // 3
    for i, cell in enumerate((1, 2, 4)):
        c.checker(i * third + pad, y, third - 2 * pad, c.h // 14, cell)
    y += c.h // 14 + 2 * s
    for i, t in enumerate((1, 2, 4, 8)):
        c.rect(pad, y + i * 12 * s, c.w - 2 * pad, t, BLACK)
    y += 50 * s
    for k in (1, 2, 4):
        if y + 8 * s * k < slots[0][2] and pad + 21 * 6 * s * k <= c.w:
            c.text(pad, y, "SHARP TEXT 0123456789", s * k)
            y += 10 * s * k
    for (name, x, sy, w, h) in slots:
        c.frame(x, sy, w, h, BLACK, s)
        c.text(x + 2 * s, sy + 2 * s, name, s)
        res = results.get((depth, name))
        if res:
            c.text(x + 2 * s, sy + h - 9 * s, res, s)


def plan_slots(w, h, tests):
    cols = 3
    sw = (w // cols) // 8 * 8
    sh = h // 7
    y0 = h - 2 * sh - 16
    return [(name, (i % cols) * sw + 8, y0 + (i // cols) * sh, sw - 16, sh - 8) for i, name in enumerate(tests)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 or COM5")
    ap.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS")
    ap.add_argument("--depth", type=int, action="append", help="only this gray depth (repeatable)")
    ap.add_argument("--keep", action="store_true", help="leave the last box test on the panel (no summary page)")
    args = ap.parse_args()
    with open_port(args.port, args.bluetooth) as port:
        display = Display(port)
        hello = display.hello()
        if not display_fw.is_display_firmware(hello):
            raise SystemExit("this device does not run the display firmware")
        w, h = hello["w"], hello["h"]
        modes = [m for m in display_fw.MODES if m in (hello.get("modes") or ["full"])]
        partial = sorted(hello.get("partial_depths") or [])
        depths = [d for d in sorted(hello["depths"]) if not args.depth or d in args.depth]
        tests = ["AUTO"] + [m.upper() for m in modes if m not in ("full", "passes")]
        tests += [f"PASSES {n}" for n in (1, 2) if "passes" in modes]
        print(f"{hello.get('board')}: {w}x{h}, depths {hello['depths']}, partial at {partial}, modes {modes}")
        slots = plan_slots(w, h, tests)
        c = Canvas(w, h)
        results, table = {}, []

        def send(label, depth, png, **kw):
            t0 = time.monotonic()
            try:
                reply = display_fw.show_png(display, png, hello, force=True, **kw)
                line = display_fw.describe(reply)
                results[(depth, label)] = f"{reply.get('mode')} {reply.get('ms')} MS"
            except DisplayError as e:
                line = f"refused: {e}"
                results[(depth, label)] = "REFUSED"
            total = time.monotonic() - t0
            table.append((depth, label, line, total))
            print(f"  {depth} bit  {label:<12} {line}; {total:.1f} s")

        for depth in depths:
            print(f"{depth} bit:")
            draw_page(c, hello, depth, slots, results)
            send("PAGE", depth, c.png(depth), mode="full")
            if depth not in partial:
                print("  no partial refresh at this depth")
                continue
            for n, (name, x, y, sw, sh) in enumerate(slots, 1):
                s = max(2, w // 400)
                bx, by, bw, bh = x + 8, y + 12 * s, (sw - 16) // 8 * 8, sh - 24 * s
                c.rect(bx, by, bw, bh, WHITE)
                c.checker(bx, by, bw // 3 // 8 * 8, bh, 2)
                c.rect(bx + bw // 3, by, bw // 3, bh, 15 - (n * 3) % 16)
                c.text(bx + 2 * bw // 3 + 2 * s, by + bh // 3, str(n), 2 * s)
                kw = {"x": bx, "y": by}
                if name.startswith("PASSES"):
                    kw.update(mode="passes", passes=int(name.split()[1]))
                elif name != "AUTO":
                    kw["mode"] = name.lower()
                send(name, depth, c.png(depth, bx, by, bw, bh), **kw)

        if not args.keep and depths:
            print("summary:")
            draw_page(c, hello, depths[-1], slots, results)
            send("SUMMARY", depths[-1], c.png(depths[-1]), mode="full")
        print("\n| depth | test | refresh | seconds |\n|---|---|---|---|")
        for depth, label, line, total in table:
            print(f"| {depth} | {label} | {line} | {total:.1f} |")


if __name__ == "__main__":
    sys.exit(main())
