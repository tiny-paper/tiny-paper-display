# SPDX-License-Identifier: MIT
"""examples/live_wire without a device or a network: the page layout from fixed fake events on both panel sizes (no
overlap, text inside its box, FEED DOWN states), the SSE line parser, the edit-rate window, the quake parsers, the
feed backoff survives a failing feed, and that the compose file parses."""
import asyncio
import importlib.util
import json
from pathlib import Path

import pytest

DIR = Path(__file__).resolve().parents[1] / "examples" / "live_wire"
spec = importlib.util.spec_from_file_location("live_wire", DIR / "live_wire.py")
lw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lw)

SIZES = [(1872, 1404), (800, 480)]


def inside(a, b):
    return a[0] >= b[0] and a[1] >= b[1] and a[0] + a[2] <= b[0] + b[2] and a[1] + a[3] <= b[1] + b[3]


def apart(a, b):
    return a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]


def check_layout(pg):
    items = pg.items
    boxes = [(i, r) for i, (k, r, p) in enumerate(items) if p is None]
    for n, (i, a) in enumerate(boxes):
        assert inside(a, (0, 0, pg.w, pg.h)), f"box {i} {a} leaves the canvas"
        for j, b in boxes[n + 1:]:
            if j != 0 and i != 0:  # item 0 is the header bar
                assert apart(a, b), f"boxes {i} {a} and {j} {b} overlap"
    for i, (k, r, p) in enumerate(items):
        if p is not None:
            assert inside(r, items[p][1]), f"{k} {i} {r} leaves its box {items[p][1]}"
    for p, (k, _, pp) in enumerate(items):
        kids = [r for kk, r, q in items if q == p and kk != "dot"]
        for n, a in enumerate(kids):
            for b in kids[n + 1:]:
                assert apart(a, b), f"two things in box {p} overlap: {a} {b}"
    return items


@pytest.mark.parametrize("w,h", SIZES)
@pytest.mark.parametrize("bsky", [True, False])
def test_layout(w, h, bsky):
    snap = lw.demo_snapshot()
    if not bsky:
        snap["bsky"] = None
    items = check_layout(lw.render(snap, w, h))
    inband = sum(1 for la, _, _ in snap["quakes"] if lw.MAP_LAT_BOTTOM <= la <= lw.MAP_LAT_TOP)
    assert inband - 3 <= sum(1 for k, _, _ in items if k == "dot") <= inband  # a dot on the very edge may be dropped
    assert any(k == "map" for k, _, _ in items) and any(k == "graph" for k, _, _ in items)


@pytest.mark.parametrize("w,h", SIZES)
def test_feed_down_and_empty(w, h):
    snap = lw.demo_snapshot()
    snap["status"] = {"wiki": "down", "quake": "down", "bsky": "down"}
    texts = []
    pg = lw.render(snap, w, h)
    check_layout(pg)
    assert sum(1 for k, r, p in pg.items if k == "text" and r[2] >= 6 * pg.s * 8) >= 3  # FEED DOWN in three boxes
    empty = {"clock": "00:00", "status": {"wiki": "wait", "quake": "wait", "bsky": "wait"}, "titles": [], "rate": 0.0,
             "series": [0.0] * 60, "top": None, "quakes": [], "latest": None, "bsky": (0.0, [0.0] * 60)}
    check_layout(lw.render(empty, w, h))
    empty["status"]["wiki"] = "ok"
    check_layout(lw.render(empty, w, h))


def test_long_and_odd_titles_stay_inside():
    snap = lw.demo_snapshot()
    snap["titles"] = ["W" * 300, "東京都", "Café été " * 20] + snap["titles"]
    snap["latest"] = (7.1, "X" * 400, 0, 0.0, 0.0)
    for w, h in SIZES:
        check_layout(lw.render(snap, w, h))


def test_sse_parser():
    p = lw.SSEParser()
    lines = [":ok", "event: message", "id: [{\"a\":1}]", "data: {\"x\":", "data: 1}", "", "event: ping", "", "data: two\r", "\r"]
    got = [e for e in (p.feed(ln) for ln in lines) if e]
    assert got == [{"event": "message", "data": "{\"x\":\n1}", "id": "[{\"a\":1}]"},
                   {"event": "message", "data": "two", "id": "[{\"a\":1}]"}]  # the id sticks (SSE rule); a comment and an empty event give nothing
    assert p.feed("data:nospace") is None and p.feed("")["data"] == "nospace"


def test_recentchange():
    ok = json.dumps({"type": "edit", "wiki": "enwiki", "title": "Gent", "bot": False})
    assert lw.parse_recentchange(ok) == ("enwiki", "Gent")
    for bad in (json.dumps({"type": "new", "wiki": "enwiki", "title": "A"}), json.dumps({"type": "edit", "wiki": "enwiki"}),
                "not json", "[]", "null"):
        assert lw.parse_recentchange(bad) is None


def test_rate_window():
    r = lw.RateWindow()
    for i in range(200):  # 10 edits per second for 20 s, from t=1000
        r.add(1000 + i / 10)
    assert r.rate(1020.0) == pytest.approx(10.0, rel=0.01)
    assert r.rate(1500.0) == 0.0  # all quiet later
    s = r.series(1020.0)
    assert len(s) == 60 and s[-1] == pytest.approx(10.0) and s[-2] == pytest.approx(10.0) and sum(s[:-2]) == 0
    for t in range(0, 5000, 5):  # memory stays bounded
        r.add(2000 + t)
    assert len(r.bins) <= 62
    assert r.series(9000) == [0.0] * 60


def test_state_snapshot_top_and_window():
    st = lw.State(["enwiki", "nlwiki"], False)
    for t in ("A", "B", "A", "A"):
        st.edit("enwiki", t, 1000.0)
    st.edit("dewiki", "Z", 1000.0)  # counted in the rate, not listed
    st.edit("nlwiki", "Gent", 1001.0)
    s = st.snapshot(1002.0)
    assert s["top"] == ("A", 3) and s["titles"][0] == "NL Gent" and "Z" not in " ".join(s["titles"]) and s["bsky"] is None
    assert st.snapshot(1000.0 + lw.WINDOW_S + 5)["top"] is None  # forgotten after 10 minutes


USGS = json.dumps({"type": "FeatureCollection", "features": [
    {"type": "Feature", "id": "us1", "properties": {"mag": 4.6, "place": "10 km N of Somewhere", "time": 1791984672420},
     "geometry": {"type": "Point", "coordinates": [139.7, 35.7, 10]}},
    {"type": "Feature", "id": "nc2", "properties": {"mag": None, "place": "no magnitude", "time": 1791984672420},
     "geometry": {"type": "Point", "coordinates": [1, 2, 3]}},
    {"type": "Feature", "id": "bad", "properties": {"mag": 2, "time": 1}, "geometry": {"type": "Point", "coordinates": [999, 0]}},
    {"type": "Feature", "id": "nogeo", "properties": {"mag": 2, "time": 1}},
    {"type": "Feature", "id": "ak3", "properties": {"mag": -0.4, "place": None, "time": 1791984000000},
     "geometry": {"type": "Point", "coordinates": [-149.9, 61.2, 1]}}]})


def test_usgs_parser():
    q = lw.parse_usgs(USGS)
    assert [x["id"] for x in q] == ["us1", "ak3"]
    assert q[0] == {"id": "us1", "mag": 4.6, "place": "10 km N of Somewhere", "time": 1791984672.42, "lat": 35.7, "lon": 139.7}
    for bad in ("", "{}", "[]", '{"features": 5}', "null"):
        assert lw.parse_usgs(bad) == []


def test_emsc_and_merge():
    msg = json.dumps({"action": "create", "data": {"id": "20261014_0000099", "properties": {
        "mag": 4.5, "flynn_region": "JAPAN", "time": "2026-10-14T10:00:00.0Z", "lat": 35.8, "lon": 139.6}}})
    e = lw.parse_emsc(msg)
    assert e and e["mag"] == 4.5 and e["place"] == "JAPAN" and abs(e["lat"] - 35.8) < 1e-9
    assert lw.parse_emsc("{}") is None and lw.parse_emsc("nope") is None
    u = {"id": "us1", "mag": 4.6, "place": "x", "time": e["time"] + 60, "lat": 35.7, "lon": 139.7}
    assert len(lw.merge_quakes([u], [e])) == 1  # the same quake from the other network is one dot
    assert len(lw.merge_quakes([u], [dict(e, time=e["time"] + 7200)])) == 2
    assert lw.merge_quakes([u], [dict(u, mag=4.7)])[0]["mag"] == 4.7  # the same id is updated


def test_failing_feed_marks_down_and_retries():
    async def go():
        st = lw.State(["enwiki"], False)
        stop, calls = asyncio.Event(), []

        async def boom(state, session):
            calls.append(1)
            raise ConnectionError("no network")
        lw.log = lambda *a: None
        task = asyncio.create_task(lw.keep("wiki", "wiki", boom, st, None, stop))
        await asyncio.sleep(1.5)
        stop.set()
        await asyncio.wait_for(task, 3)
        return st, calls
    st, calls = asyncio.run(go())
    assert st.status["wiki"] == "down" and len(calls) >= 2


def test_compose_parses():
    yaml = pytest.importorskip("yaml", reason="pyyaml: pip install pyyaml")
    c = yaml.safe_load((DIR / "docker-compose.yml").read_text())
    svc = c["services"]["live-wire"]
    assert "/dev/ttyACM0:/dev/ttyACM0" in svc["devices"]
    assert (DIR / "Dockerfile").exists() and svc["build"]["dockerfile"] == "python/examples/live_wire/Dockerfile"
    assert "python:3.12-slim" in (DIR / "Dockerfile").read_text()
    assert (DIR.parents[2] / svc["build"]["dockerfile"]).exists()
