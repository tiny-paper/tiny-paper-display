# SPDX-License-Identifier: MIT
"""examples/btc_ticker.py without a device or a network: every text drawn has its letters in the font, the page at
both panel sizes for rising, falling, flat and missing prices, the live price on the last candle, the changed
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


def test_fetch_price(monkeypatch):
    answer(monkeypatch, {"error": [], "result": {"XXBTZEUR": {"c": ["73443.6", "0.1"]}}})
    assert bt.fetch_price("EUR") == 73443.6


def test_live_price_moves_the_last_candle():
    got = bt.with_price(series(1), 90000.0)
    assert got[-1][2:] == (90000.0, series(1)[-1][3], 90000.0) and got[:-1] == series(1)[:-1]
    assert bt.with_price([], 1.0) == []


def test_changes_cover_every_changed_pixel():
    a = bt.render(series(1), 800, 480, clock=0)
    b = bt.render(bt.with_price(series(1), series(1)[-1][4] + 3), 800, 480, clock=1)
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
    for call in (bt.fetch_candles, bt.fetch_price):
        with pytest.raises(bt.FAILS):
            call("USD")
