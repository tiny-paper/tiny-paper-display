#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bitcoin price, live, over a 24-hour candle chart, from Kraken's public market data (no account, no key, standard
library only).

    python btc_ticker.py                        (the first device found)
    python btc_ticker.py COM5 --currency EUR --every 2
    python btc_ticker.py --bluetooth NAME
    python btc_ticker.py --png page.png --size 800x480 [--demo]     (no device; --demo: fixed fake candles)

The price is asked every --every seconds (default 2) and moves the last candle; the candles are fetched again every
minute. Only the rectangles that changed are sent, in 1 bit, so an update is a fast partial refresh (the device's
full_every does the clean-up). Without the network the last page stays up with an OFFLINE mark."""
import argparse
import json
import math
import sys
import time
import urllib.request
from pathlib import Path

import tiny_paper_display as tpd

BLACK, WHITE = 0, 15
API = "https://api.kraken.com/0/public/"
CANDLES = 96  # 24 hours of 15-minute candles
CANDLES_EVERY = 60  # seconds between candle fetches
FAILS = (OSError, ValueError, KeyError, IndexError, TypeError, AttributeError, StopIteration)  # network down, odd answers


def ask(path, timeout=10):
    with urllib.request.urlopen(API + path, timeout=timeout) as r:
        data = json.load(r)
    if data.get("error"):
        raise ValueError(", ".join(data["error"]))
    return next(v for k, v in data["result"].items() if k != "last")


def fetch_candles(cur):
    """[(unix time, open, high, low, close)] of the last 24 hours, oldest first."""
    rows = ask(f"OHLC?pair=XBT{cur}&interval=15")
    return [(int(r[0]), *(float(v) for v in r[1:5])) for r in rows][-CANDLES:]


def fetch_price(cur):
    return float(ask(f"Ticker?pair=XBT{cur}")["c"][0])


def with_price(candles, price):
    """The candles with the live price as the close of the last one (its high and low widened to it)."""
    if not candles:
        return candles
    t, o, h, lo, _ = candles[-1]
    return candles[:-1] + [(t, o, max(h, price), min(lo, price), price)]


def demo_candles(now=1791400000):
    out, p = [], 84000.0
    for i in range(CANDLES):
        c = 84000 + 900 * math.sin(i / 9) + 12 * i
        out.append((now - (CANDLES - i) * 900, p, max(p, c) + 60, min(p, c) - 60, c))
        p = c
    return out


def money(v):
    return f"{v:,.0f}" if v >= 100 else f"{v:,.2f}"


def render(candles, w, h, cur="USD", offline=False, clock=None):
    """The page as a Canvas: header, price and 24-hour change, the candles with their high and low, time marks."""
    c = tpd.Canvas(w, h)
    s = max(2, w // 300)  # text scale: 7 * s px tall
    pad = 4 * s
    c.rect(0, 0, w, 11 * s, BLACK)
    c.text(pad, 2 * s, f"BITCOIN  BTC/{cur}  KRAKEN", s, WHITE)
    stamp = ("OFFLINE " if offline else "") + time.strftime("%H:%M:%S", time.localtime(clock))
    c.text(w - pad - len(stamp) * 6 * s, 2 * s, stamp, s, WHITE)
    if len(candles) < 2:
        c.text(pad, h // 2, "NO PRICES YET", 2 * s)
        return c
    first, last = candles[0][1], candles[-1][4]
    big = 3 * s
    y = 15 * s
    x = c.text(pad, y, money(last), big)
    pct = (last - first) / first * 100 if first else 0.0
    trend = "UP" if pct > 0 else "DOWN" if pct < 0 else "FLAT"  # the font has no plus sign
    c.text(x + 6 * s, y + 7 * big - 14 * s, f"{trend} {abs(pct):.2f} % 24H", 2 * s)

    top, bottom = y + 7 * big + 8 * s, h - 16 * s
    left, right = pad, w - pad - 9 * 6 * s  # room for the price labels on the right
    lo, hi = min(k[3] for k in candles), max(k[2] for k in candles)
    span = (hi - lo) or 1.0

    def yof(v):
        return round(bottom - (v - lo) / span * (bottom - top))

    for frac in (0, 0.5, 1):  # dotted guide lines with their prices
        gy = yof(lo + span * frac)
        for gx in range(left, right, 4 * s):
            c.rect(gx, gy, s, max(1, s // 2), BLACK)
        c.text(right + 2 * s, gy - 3 * s, money(lo + span * frac), s)
    slot = (right - left) / len(candles)
    body = max(1, round(slot * 0.6))
    line = max(1, s // 2)
    for i, (_, o, hi_, lo_, cl) in enumerate(candles):  # rising: hollow body, falling: filled
        cx = left + round(slot * i + (slot - body) / 2)
        c.rect(cx + (body - line) // 2, yof(hi_), line, yof(lo_) - yof(hi_) + 1, BLACK)
        by, bh = yof(max(o, cl)), max(line, abs(yof(o) - yof(cl)) + 1)
        if cl >= o and body > 2 * line:
            c.rect(cx, by, body, bh, WHITE)
            c.frame(cx, by, body, bh, BLACK, line)
        else:
            c.rect(cx, by, body, bh, BLACK)
    for label, frac in (("-24H", 0), ("-12H", 0.5), ("NOW", 1)):  # time marks under the chart
        lx = left + round((right - left) * frac) - round(len(label) * 6 * s * frac)
        c.text(lx, h - 9 * s, label, s)
    return c


def changes(old, new, gap=8):
    """The rectangles (x, y, w, h) that hold every pixel that differs between two canvases of one size: one per run of
    changed rows (runs closer than `gap` rows merged), x and w in whole groups of 8 as the device wants them."""
    w = new.w
    rows = [y for y in range(new.h) if old.px[y * w:(y + 1) * w] != new.px[y * w:(y + 1) * w]]
    runs = []
    for y in rows:
        if runs and y - runs[-1][1] <= gap:
            runs[-1][1] = y
        else:
            runs.append([y, y])
    out = []
    for y0, y1 in runs:
        x0, x1 = w, 0
        for y in range(y0, y1 + 1):
            a, b = old.px[y * w:(y + 1) * w], new.px[y * w:(y + 1) * w]
            if a != b:
                x0 = min(x0, next(i for i in range(w) if a[i] != b[i]))
                x1 = max(x1, next(i for i in range(w - 1, -1, -1) if a[i] != b[i]))
        px0, px1 = x0 // 8 * 8, min((x1 // 8 + 1) * 8, w)
        out.append((px0, y0, px1 - px0, y1 - y0 + 1))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 or COM5 (default: the first device found)")
    ap.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS")
    ap.add_argument("--currency", default="USD", choices=("USD", "EUR", "GBP", "CAD", "JPY", "CHF", "AUD"))
    ap.add_argument("--every", type=float, default=2, help="seconds between price updates (default 2)")
    ap.add_argument("--png", metavar="FILE", help="write the page to a PNG and exit (no device)")
    ap.add_argument("--size", default="800x480", help="PNG size WxH (default 800x480)")
    ap.add_argument("--demo", action="store_true", help="with --png: fixed fake candles instead of the network")
    args = ap.parse_args(argv)
    if args.png:
        w, h = (int(v) for v in args.size.lower().split("x"))
        candles = demo_candles() if args.demo else fetch_candles(args.currency)
        Path(args.png).write_bytes(render(candles, w, h, args.currency, clock=candles[-1][0]).png(1))
        print(f"wrote {args.png} ({w}x{h})")
        return 0
    candles, fetched, shown = [], 0.0, None
    try:
        with tpd.open(args.port, args.bluetooth) as d:
            while True:
                started, offline = time.monotonic(), False
                try:
                    if time.monotonic() - fetched >= CANDLES_EVERY or not candles:
                        candles, fetched = fetch_candles(args.currency), time.monotonic()
                    candles = with_price(candles, fetch_price(args.currency))
                except FAILS as e:
                    print(f"no prices, keeping the last ones: {e}", file=sys.stderr)
                    offline = True
                page = render(candles, d.w, d.h, args.currency, offline)
                try:
                    if shown is None:
                        d.show(page, depth=1)
                    else:
                        for x, y, rw, rh in changes(shown, page):
                            d.area(page.crop(x, y, rw, rh), x, y, depth=1)
                    shown = page
                except tpd.DisplayError as e:
                    print(f"show failed, the whole page goes next time: {e}", file=sys.stderr)
                    shown = None
                time.sleep(max(0.0, args.every - (time.monotonic() - started)))
    except KeyboardInterrupt:
        print()
        return 0


if __name__ == "__main__":
    sys.exit(main())
