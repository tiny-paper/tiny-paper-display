# SPDX-License-Identifier: MIT
"""examples/btc_ticker.py without a device or a network: every text drawn has its letters in the font, the page at
both panel sizes for rising, falling, flat and missing prices, the WebSocket updates, the changed
rectangles, and the Kraken answer parsing (data, error, bad data)."""
import importlib.util
import io
import json
from pathlib import Path

import pytest

import tiny_paper_display as tpd
from tiny_paper_display.canvas import FONT

SRC = Path(__file__).resolve().parents[1] / "examples" / "btc_ticker.py"
spec = importlib.util.spec_from_file_location("btc_ticker", SRC)
bt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bt)

SIZES = [(1872, 1404), (800, 480)]


@pytest.fixture
def drawn(monkeypatch):
    """The texts drawn, with a check that the font has every letter (it draws a blank for one it lacks)."""
    seen = []
    real = tpd.Canvas.text

    def text(self, x, y, s, scale, level=0):
        missing = {ch for ch in s.upper() if ch not in FONT}
        assert not missing, f"{s!r}: no glyph for {missing}"
        assert x >= 0 and y >= 0 and x + len(s) * 6 * scale <= self.w + scale and y + 7 * scale <= self.h, s
        seen.append(s)
        return real(self, x, y, s, scale, level)

    monkeypatch.setattr(tpd.Canvas, "text", text)
    return seen


def series(step):
    return [(1791400000 + i * 900, 80000.0 + step * i, 80030.0 + step * (i + 1), 79970.0 + step * i,
             80000.0 + step * (i + 1)) for i in range(bt.CANDLES)]


@pytest.mark.parametrize("w,h", SIZES)
@pytest.mark.parametrize("step", [25.0, -25.0, 0.0])
def test_page(drawn, w, h, step):
    c = bt.render(series(step), w, h, "EUR", clock=1791400000)
    assert (c.w, c.h) == (w, h) and 0 in c.px and 15 in c.px
    change = next(s for s in drawn if "24H" in s)
    assert change.split()[0] == ("UP" if step > 0 else "DOWN" if step < 0 else "FLAT")
    assert any("BTC/EUR" in s for s in drawn)


@pytest.mark.parametrize("prices", [[], series(1)[:1]])
def test_too_few_prices(drawn, prices):
    bt.render(prices, 800, 480)
    assert "NO PRICES YET" in drawn


def test_offline_mark(drawn):
    bt.render(series(1), 800, 480, offline=True)
    assert any(s.startswith("OFFLINE") for s in drawn)


def answer(monkeypatch, body):
    monkeypatch.setattr(bt.urllib.request, "urlopen", lambda url, timeout: io.BytesIO(json.dumps(body).encode()))


def test_fetch_takes_the_last_day(monkeypatch):
    rows = [[1791000000 + i * 900, "1", "2", "0", str(100 + i), "1", "1", 1] for i in range(720)]
    answer(monkeypatch, {"error": [], "result": {"XXBTZUSD": rows, "last": 1}})
    got = bt.fetch_candles("USD")
    assert len(got) == bt.CANDLES and got[-1] == (rows[-1][0], 1.0, 2.0, 0.0, 819.0)


def update(*rows):
    return {"channel": "ohlc", "type": "update", "data": [
        {"interval_begin": f"2026-10-14T15:{m:02d}:00.000000000Z", "open": 1, "high": 3, "low": 0.5, "close": c}
        for m, c in rows]}


def test_websocket_updates_move_the_candles():
    start = [(int(bt.datetime.fromisoformat("2026-10-14T15:30:00+00:00").timestamp()), 1.0, 2.0, 1.0, 2.0)]
    got = bt.apply(start, update((30, 2.5)))
    assert got == [(start[0][0], 1.0, 3.0, 0.5, 2.5)]  # the current candle replaced
    got = bt.apply(got, update((45, 4.0)))
    assert len(got) == 2 and got[-1][0] == start[0][0] + 900 and got[-1][4] == 4.0  # the next one started
    assert bt.apply(got, update((15, 9.0))) == got  # an older candle is ignored
    full = bt.apply(series(1), update((59, 1.0)))
    assert len(full) == bt.CANDLES  # still 24 hours


@pytest.mark.parametrize("msg", [{"channel": "heartbeat"}, {"method": "subscribe", "success": True},
                                 {"channel": "ohlc", "type": "snapshot", "data": []}, [], None, "x"])
def test_other_messages_change_nothing(msg):
    assert bt.apply(series(1), msg) == series(1)


@pytest.mark.parametrize("msg", [{"channel": "ohlc", "type": "update"}, {"channel": "ohlc", "type": "update", "data": [{}]},
                                 update((30, "x")), {"channel": "ohlc", "type": "update", "data": [{"interval_begin": "no"}]}])
def test_bad_updates_end_in_a_known_error(msg):
    with pytest.raises(bt.FAILS):
        bt.apply(series(1), msg)


def test_changes_cover_every_changed_pixel():
    a = bt.render(series(1), 800, 480, clock=0)
    last = series(1)[-1]
    b = bt.render(series(1)[:-1] + [last[:4] + (last[4] + 3,)], 800, 480, clock=1)
    rects = bt.changes(a, b)
    assert bt.changes(a, a) == [] and 1 < len(rects) <= 4
    for x, y, w, h in rects:
        assert x % 8 == 0 and w % 8 == 0 and x + w <= 800 and y + h <= 480
        for yy in range(y, y + h):
            a.px[yy * 800 + x:yy * 800 + x + w] = b.px[yy * 800 + x:yy * 800 + x + w]
    assert a.px == b.px


@pytest.mark.parametrize("body", [{"error": ["EQuery:Unknown asset pair"]}, {"error": [], "result": {"last": 1}},
                                  {"result": {"X": [["t"]]}}, {"result": {"X": [None]}}, {"result": {"X": "abc"}},
                                  {"result": []}, {}, [], None, "down"])
def test_bad_answers_end_in_a_known_error(monkeypatch, body):
    """main keeps the last page on these (FAILS); anything else would end the program."""
    answer(monkeypatch, body)
    with pytest.raises(bt.FAILS):
        bt.fetch_candles("USD")
