#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bitcoin price, live, over a candle chart of the last hour, from Kraken's public market data (no account, no key).

    python btc_ticker.py                        (the first device found: 1-minute candles, the last hour)
    python btc_ticker.py COM5 --currency EUR --minutes 5 --clean full --clean-every 60
    python btc_ticker.py --bluetooth NAME
    python btc_ticker.py --png page.png --size 800x480 [--demo]     (no device; --demo: fixed fake candles)

The candles come once over HTTP, then Kraken's WebSocket pushes every change of the current one (needs
`pip install websockets`). The page is redrawn when a price moved, at most every --every seconds (default 1), in 1 bit:
the device refreshes only what changed, without a flash. Every --clean-every updates (default 30) one clean-up
refresh removes the ghosting: an inverse flash by default (--clean full: the full refresh). Without the network the
last page stays up with an OFFLINE mark and the program connects again."""
import argparse
import json
import math
import sys
import time
import urllib.request
from datetime import datetime
from pathlib import Path

import tiny_paper_display as tpd

BLACK, WHITE = 0, 15
API = "https://api.kraken.com/0/public/"
WS = "wss://ws.kraken.com/v2"
CANDLES = 60  # candles on the chart: the last hour at the default --minutes 1
FAILS = (OSError, ValueError, KeyError, IndexError, TypeError, AttributeError, StopIteration)  # network down, odd answers


def ask(path, timeout=10):
    with urllib.request.urlopen(API + path, timeout=timeout) as r:
        data = json.load(r)
    if data.get("error"):
        raise ValueError(", ".join(data["error"]))
    return next(v for k, v in data["result"].items() if k != "last")


def fetch_candles(cur, minutes=1):
    """[(unix time, open, high, low, close)] of the last CANDLES candles of `minutes` each, oldest first."""
    rows = ask(f"OHLC?pair=XBT{cur}&interval={minutes}")
    return [(int(r[0]), *(float(v) for v in r[1:5])) for r in rows][-CANDLES:]


def subscribe(cur, minutes=1):
    return json.dumps({"method": "subscribe", "params": {"channel": "ohlc", "symbol": [f"BTC/{cur}"], "interval": minutes}})


def apply(candles, msg):
    """The candles after one WebSocket message: an ohlc update replaces the current candle or starts the next one;
    anything else (heartbeat, status, the subscribe answer) leaves them as they are."""
    if not isinstance(msg, dict) or msg.get("channel") != "ohlc" or msg.get("type") != "update":
        return candles
    out = list(candles)
    for m in msg["data"]:
        k = (int(datetime.fromisoformat(m["interval_begin"][:19] + "+00:00").timestamp()),
             float(m["open"]), float(m["high"]), float(m["low"]), float(m["close"]))
        if out and out[-1][0] == k[0]:
            out[-1] = k
        elif not out or k[0] > out[-1][0]:
            out.append(k)
    return out[-CANDLES:]


def demo_candles(now=1791400000):
    out, p = [], 84000.0
    for i in range(CANDLES):
        c = 84000 + 90 * math.sin(i / 6) + 2 * i
        out.append((now - (CANDLES - i) * 60, p, max(p, c) + 8, min(p, c) - 8, c))
        p = c
    return out


def money(v):
    return f"{v:,.0f}" if v >= 100 else f"{v:,.2f}"


def span(m):
    """Minutes as a label: 30M, 60M, 5H."""
    return f"{m}M" if m < 120 or m % 60 else f"{m // 60}H"


def render(candles, w, h, cur="USD", offline=False, clock=None, minutes=1):
    """The page as a Canvas: header, price and its change over the chart, the candles with their high and low, time marks."""
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
    c.text(x + 6 * s, y + 7 * big - 14 * s, f"{trend} {abs(pct):.2f} % {span(minutes * CANDLES)}", 2 * s)

    top, bottom = y + 7 * big + 8 * s, h - 16 * s
    left, right = pad, w - pad - 9 * 6 * s  # room for the price labels on the right
    lo, hi = min(k[3] for k in candles), max(k[2] for k in candles)
    rng = (hi - lo) or 1.0

    def yof(v):
        return round(bottom - (v - lo) / rng * (bottom - top))

    for frac in (0, 0.5, 1):  # dotted guide lines with their prices
        gy = yof(lo + rng * frac)
        for gx in range(left, right, 4 * s):
            c.rect(gx, gy, s, max(1, s // 2), BLACK)
        c.text(right + 2 * s, gy - 3 * s, money(lo + rng * frac), s)
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
    for label, frac in (("-" + span(minutes * CANDLES), 0), ("-" + span(minutes * CANDLES // 2), 0.5), ("NOW", 1)):  # time marks under the chart
        lx = left + round((right - left) * frac) - round(len(label) * 6 * s * frac)
        c.text(lx, h - 9 * s, label, s)
    return c


def refresh_mode(n, clean_every, clean):
    """The refresh of update n (1, 2, ...): partial, and every clean_every-th one the clean-up (0: never). A named mode
    skips the device's own full_every, so the example counts itself."""
    return clean if clean_every and n % clean_every == 0 else "partial"


def draw(d, page, mode):
    """Send the page; the device refreshes only what changed."""
    try:
        d.show(page, depth=1, mode=mode)
    except tpd.DisplayError as e:
        print(f"show failed, trying again with the next price: {e}", file=sys.stderr)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 or COM5 (default: the first device found)")
    ap.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS")
    ap.add_argument("--currency", default="USD", choices=("USD", "EUR", "GBP", "CAD", "JPY", "CHF", "AUD"))
    ap.add_argument("--minutes", type=int, default=1, choices=(1, 5, 15), help="minutes per candle (default 1: the last hour)")
    ap.add_argument("--clean", default="inverse", choices=("inverse", "full"), help="the clean-up refresh (default inverse)")
    ap.add_argument("--clean-every", type=int, default=30, metavar="N", help="a clean-up every N updates (default 30, 0 never)")
    ap.add_argument("--every", type=float, default=1, help="at most one screen update per this many seconds (default 1)")
    ap.add_argument("--verbose", action="store_true", help="one line per screen update: price, refresh, time")
    ap.add_argument("--png", metavar="FILE", help="write the page to a PNG and exit (no device)")
    ap.add_argument("--size", default="800x480", help="PNG size WxH (default 800x480)")
    ap.add_argument("--demo", action="store_true", help="with --png: fixed fake candles instead of the network")
    args = ap.parse_args(argv)
    if args.png:
        w, h = (int(v) for v in args.size.lower().split("x"))
        candles = demo_candles() if args.demo else fetch_candles(args.currency, args.minutes)
        Path(args.png).write_bytes(render(candles, w, h, args.currency, clock=candles[-1][0], minutes=args.minutes).png(1))
        print(f"wrote {args.png} ({w}x{h})")
        return 0
    try:
        from websockets.exceptions import WebSocketException
        from websockets.sync.client import connect
    except ImportError:
        print("needs the websockets package: pip install websockets", file=sys.stderr)
        return 1
    candles = []
    try:
        with tpd.open(args.port, args.bluetooth) as d:
            n = 0
            while True:
                try:
                    candles = fetch_candles(args.currency, args.minutes)
                    with connect(WS, open_timeout=10, max_queue=None) as ws:  # no limit: pings get through while a page draws
                        ws.send(subscribe(args.currency, args.minutes))
                        dirty, drawn = True, 0.0
                        while True:
                            wait = max(0.0, args.every - (time.monotonic() - drawn)) if dirty else 30
                            try:
                                new = apply(candles, json.loads(ws.recv(timeout=wait)))
                                dirty, candles = dirty or new != candles, new
                            except TimeoutError:
                                pass
                            if dirty and time.monotonic() - drawn >= args.every:
                                t0, n = time.monotonic(), n + 1
                                mode = refresh_mode(n, args.clean_every, args.clean)
                                draw(d, render(candles, d.w, d.h, args.currency, minutes=args.minutes), mode)
                                if args.verbose:
                                    print(f"{candles[-1][4]:,.1f} {mode} in {round((time.monotonic() - t0) * 1000)} ms", flush=True)
                                dirty, drawn = False, time.monotonic()
                except FAILS + (WebSocketException,) as e:
                    print(f"no prices, connecting again in 5 s: {e}", file=sys.stderr)
                    draw(d, render(candles, d.w, d.h, args.currency, offline=True, minutes=args.minutes), "partial")
                    time.sleep(5)
    except KeyboardInterrupt:
        print()
        return 0


if __name__ == "__main__":
    sys.exit(main())
