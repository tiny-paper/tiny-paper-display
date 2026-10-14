#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A test page drawn in code, then a box changed in every refresh mode the device offers, at every gray depth.

    python test_page.py                         (the first device found)
    python test_page.py COM5                    (or /dev/ttyACM0)
    python test_page.py --bluetooth NAME
    python test_page.py --depth 1 --keep

Per depth the device reports: the page as a full refresh, then one numbered box per mode (device's choice, partial,
partial_all, inverse, passes 1 and 2), each drawn into the page and the whole page sent: the device refreshes what
changed. At the end the page is drawn once more with every
timing in it and the same table is printed. The reference example of the API (tiny_paper_display); no Pillow needed."""
import argparse
import sys
import time

import tiny_paper_display as tpd
from tiny_paper_display import display_fw

BLACK, WHITE = 0, 15


def draw_page(c, caps, board, depth, slots, results):
    s = max(2, c.w // 400)  # text scale: 7 * s px tall
    pad = 4 * s
    c.rect(0, 0, c.w, c.h, WHITE)
    c.rect(0, 0, c.w, 11 * s, BLACK)
    c.text(pad, 2 * s, f"TINY-PAPER DISPLAY TEST PAGE  {board}  {c.w}X{c.h}  {depth} BIT", s, WHITE)
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
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 or COM5 (default: the first device found)")
    ap.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS")
    ap.add_argument("--depth", type=int, action="append", help="only this gray depth (repeatable)")
    ap.add_argument("--keep", action="store_true", help="leave the last box test on the panel (no summary page)")
    args = ap.parse_args()
    with tpd.open(args.port, args.bluetooth) as d:
        caps, w, h = d.caps, d.w, d.h
        modes = [m for m in display_fw.MODES if m in (caps.modes or ["full"])]
        partial = sorted(caps.partial_depths)
        depths = [x for x in sorted(caps.depths) if not args.depth or x in args.depth]
        tests = ["AUTO"] + [m.upper() for m in modes if m not in ("full", "passes")]
        tests += [f"PASSES {n}" for n in (1, 2) if "passes" in modes]
        print(f"{d.board}: {w}x{h}, depths {caps.depths}, partial at {partial}, modes {modes}")
        slots = plan_slots(w, h, tests)
        c = tpd.Canvas(w, h)
        results, table = {}, []

        def send(label, depth, picture, **kw):
            t0 = time.monotonic()
            try:
                reply = d.show(picture, depth=depth, force=True, **kw)
                line = display_fw.describe(reply)
                results[(depth, label)] = f"{reply.get('mode')} {reply.get('ms')} MS"
            except tpd.DisplayError as e:
                line = f"refused: {e}"
                results[(depth, label)] = "REFUSED"
            total = time.monotonic() - t0
            table.append((depth, label, line, total))
            print(f"  {depth} bit  {label:<12} {line}; {total:.1f} s")

        for depth in depths:
            print(f"{depth} bit:")
            draw_page(c, caps, d.board, depth, slots, results)
            send("PAGE", depth, c, mode="full")
            if depth not in partial:
                print("  no partial refresh at this depth")
                continue
            # depth_modes (newer firmware): only the modes this depth does; without it try and report the refusal
            can = caps.depth_modes.get(depth) if caps.depth_modes else None
            for n, (name, x, y, sw, sh) in enumerate(slots, 1):
                mode = "passes" if name.startswith("PASSES") else name.lower()
                if can is not None and name != "AUTO" and mode not in can:
                    print(f"  {depth} bit  {name:<12} not at this depth (depth_modes)")
                    continue
                s = max(2, w // 400)
                bx, by, bw, bh = x + 8, y + 12 * s, (sw - 16) // 8 * 8, sh - 24 * s
                c.rect(bx, by, bw, bh, WHITE)
                c.checker(bx, by, bw // 3 // 8 * 8, bh, 2)
                c.rect(bx + bw // 3, by, bw // 3, bh, 15 - (n * 3) % 16)
                c.text(bx + 2 * bw // 3 + 2 * s, by + bh // 3, str(n), 2 * s)
                kw = {}
                if name.startswith("PASSES"):
                    kw.update(mode="passes", passes=int(name.split()[1]))
                elif name != "AUTO":
                    kw["mode"] = name.lower()
                send(name, depth, c, **kw)

        if not args.keep and depths:
            print("summary:")
            draw_page(c, caps, d.board, depths[-1], slots, results)
            send("SUMMARY", depths[-1], c, mode="full")
        print("\n| depth | test | refresh | seconds |\n|---|---|---|---|")
        for depth, label, line, total in table:
            print(f"| {depth} | {label} | {line} | {total:.1f} |")


if __name__ == "__main__":
    sys.exit(main())
