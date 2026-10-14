#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Live wire: feeds that push, shown the moment they happen. Wikipedia edits (SSE), earthquakes (USGS poll, EMSC push),
optionally Bluesky posts per second (Jetstream). No account, no key, no web server.

    python live_wire.py                         (the first device found; Ctrl-C to stop)
    python live_wire.py --wikis enwiki,nlwiki --emsc --bluesky
    python live_wire.py --png page.png --demo   (no device: fixed fake events, one picture)
    python live_wire.py --png page.png --seconds 30 --png-every 10    (no device: real feeds, a picture every 10 s)

The page is drawn in memory whenever the numbers changed; at most one Device.show every 2 s (the device refreshes only the
changed rectangle). A feed that is down shows FEED DOWN in its box and is retried with backoff. One dependency
besides tiny-paper-display: aiohttp (SSE and websockets)."""
import argparse
import asyncio
import json
import os
import signal
import sys
import time
import unicodedata
from collections import Counter, deque
from pathlib import Path

import tiny_paper_display as tpd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from coast_data import COAST  # noqa: E402

WIKI_URL = "https://stream.wikimedia.org/v2/stream/recentchange"
USGS_URL = "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_day.geojson"  # 24 h; all_hour has too few dots
EMSC_URL = "wss://www.seismicportal.eu/standing_order/websocket"
BSKY_URL = "wss://jetstream2.us-east.bsky.network/subscribe?wantedCollections=app.bsky.feed.post"
USER_AGENT = ("tiny-paper-display-live-wire/1.0 (https://github.com/tiny-paper/tiny-paper-display; example script) "
              + os.environ.get("LIVE_WIRE_CONTACT", ""))
WINDOW_S, BIN_S, MIN_SEND_S, TICK_S = 600, 10, 2.0, 0.5
MAP_LAT_TOP, MAP_LAT_BOTTOM = 80, -60


# ---------------------------------------------------------------- parsers (pure: text in, values out)
class SSEParser:
    """Server-sent events, line by line: feed(line) returns {"event", "data", "id"} when a blank line ends an event."""

    def __init__(self):
        self.event, self.data, self.id = None, [], None

    def feed(self, line):
        line = line.rstrip("\r\n")
        if line == "":
            out = {"event": self.event or "message", "data": "\n".join(self.data), "id": self.id} if self.data else None
            self.event, self.data = None, []
            return out
        if line.startswith(":"):
            return None
        field, _, value = line.partition(":")
        value = value[1:] if value.startswith(" ") else value
        if field == "data":
            self.data.append(value)
        elif field == "event":
            self.event = value
        elif field == "id":
            self.id = value
        return None


def parse_recentchange(data):
    """One EventStreams recentchange JSON text -> (wiki, title) for an edit, else None (new pages, logs, bad JSON)."""
    try:
        ev = json.loads(data)
        if ev.get("type") == "edit" and isinstance(ev.get("wiki"), str) and isinstance(ev.get("title"), str):
            return ev["wiki"], ev["title"]
    except (ValueError, AttributeError):
        pass
    return None


def _quake(qid, mag, place, t_ms, lon, lat):
    try:
        mag, t_ms, lon, lat = float(mag), float(t_ms), float(lon), float(lat)
    except (TypeError, ValueError):
        return None
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        return None
    return {"id": str(qid), "mag": mag, "place": str(place or ""), "time": t_ms / 1000.0, "lat": lat, "lon": lon}


def parse_usgs(text):
    """USGS GeoJSON -> quakes [{id, mag, place, time (s), lat, lon}]; features without magnitude or position are skipped."""
    out = []
    try:
        feats = json.loads(text).get("features", [])
    except (ValueError, AttributeError):
        return out
    for f in feats if isinstance(feats, list) else []:
        try:
            p, c = f["properties"], f["geometry"]["coordinates"]
            q = _quake(f.get("id"), p["mag"], p.get("place"), p["time"], c[0], c[1])
        except (KeyError, TypeError, IndexError):
            continue
        if q:
            out.append(q)
    return out


def parse_emsc(text):
    """One EMSC websocket message -> a quake or None. Time is ISO 8601 UTC."""
    try:
        msg = json.loads(text)
        p = msg["data"]["properties"]
        t = time.mktime(time.strptime(p["time"][:19], "%Y-%m-%dT%H:%M:%S")) - time.timezone
        return _quake("emsc" + str(msg["data"].get("id")), p["mag"], p.get("flynn_region"), t * 1000, p["lon"], p["lat"])
    except (KeyError, TypeError, ValueError, AttributeError):
        return None


def merge_quakes(have, new):
    """Add `new` quakes to the list: the same id replaces, a near duplicate (3 degrees, 10 min) from another network is dropped."""
    out = {q["id"]: q for q in have}
    for q in new:
        dup = any(o["id"] != q["id"] and abs(o["lat"] - q["lat"]) < 3 and abs(o["lon"] - q["lon"]) < 3
                  and abs(o["time"] - q["time"]) < 600 for o in out.values())
        if q["id"] in out or not dup:
            out[q["id"]] = q
    return sorted(out.values(), key=lambda q: q["time"])


class RateWindow:
    """Events per second over a sliding window, in bins of BIN_S seconds. Memory is bounded by WINDOW_S / BIN_S."""

    def __init__(self, window=WINDOW_S, bin_s=BIN_S):
        self.window, self.bin_s, self.bins = window, bin_s, {}

    def add(self, t, n=1):
        b = int(t // self.bin_s)
        self.bins[b] = self.bins.get(b, 0) + n
        for old in [k for k in self.bins if k < b - self.window // self.bin_s - 1]:
            del self.bins[old]

    def rate(self, now):
        """The current bin and the two before, over the time they cover."""
        b = int(now // self.bin_s)
        n = sum(self.bins.get(b - i, 0) for i in range(3))
        return n / (2 * self.bin_s + (now - b * self.bin_s) or 1)

    def series(self, now):
        """Events per second of each complete bin, oldest first, window / bin_s values."""
        b = int(now // self.bin_s)
        return [round(self.bins.get(k, 0) / self.bin_s, 2) for k in range(b - self.window // self.bin_s, b)]


# ---------------------------------------------------------------- state
class State:
    def __init__(self, wikis, bluesky):
        self.wikis, self.bluesky = list(wikis), bluesky
        self.titles = deque(maxlen=40)          # (wiki, title), newest first
        self.recent = deque()                   # (time, wiki, title) of the chosen wikis, for the most edited
        self.edits, self.posts = RateWindow(), RateWindow()
        self.quakes = []
        self.status = {"wiki": "wait", "quake": "wait", "bsky": "wait"}
        self.good = set()                       # feeds that delivered since their last (re)connect

    def edit(self, wiki, title, now):
        self.edits.add(now)
        if wiki in self.wikis:
            self.titles.appendleft((wiki, title))
            self.recent.append((now, wiki, title))

    def snapshot(self, now):
        while self.recent and self.recent[0][0] < now - WINDOW_S:
            self.recent.popleft()
        top = Counter((w, t) for _, w, t in self.recent).most_common(1)
        quakes = [q for q in self.quakes if q["time"] >= now - 86400]
        latest = max(quakes, key=lambda q: q["time"], default=None)
        multi = len(self.wikis) > 1
        return {"clock": time.strftime("%H:%M", time.localtime(now)), "status": dict(self.status),
                "titles": [((w[:-4] if w.endswith("wiki") else w).upper() + " " if multi else "") + t for w, t in self.titles],
                "rate": round(self.edits.rate(now), 1), "series": self.edits.series(now),
                "top": (top[0][0][1], top[0][1]) if top else None,
                "quakes": [(q["lat"], q["lon"], q["mag"]) for q in quakes],
                "latest": (latest["mag"], latest["place"], int((now - latest["time"]) // 60), latest["lat"], latest["lon"]) if latest else None,
                "bsky": (round(self.posts.rate(now), 1), self.posts.series(now)) if self.bluesky else None}


# ---------------------------------------------------------------- drawing
FONT = tpd.canvas.FONT


def clean(s):
    """Letters the 5x7 font has: accents dropped, everything else a space."""
    s = unicodedata.normalize("NFKD", s).upper()
    return "".join(ch if ch in FONT else " " for ch in s if not unicodedata.combining(ch))


def fmt_age(minutes):
    return "JUST NOW" if minutes < 1 else f"{minutes} MIN AGO" if minutes < 120 else f"{minutes // 60} H AGO"


class Page:
    """A Canvas plus a list of everything drawn: items (kind, (x, y, w, h), parent index or None), for the layout test."""

    def __init__(self, w, h):
        self.c = tpd.Canvas(w, h)
        self.w, self.h, self.items = w, h, []
        self.s = max(1, min(round(w / 360), round(h / 220)))  # glyphs are 7 * s px tall
        self.t = max(1, self.s // 2)

    def add(self, kind, rect, parent=None):
        self.items.append((kind, rect, parent))
        return len(self.items) - 1

    def box(self, x, y, w, h, title):
        self.c.frame(x, y, w, h, 0, self.t)
        i = self.add("box", (x, y, w, h))
        self.text(i, x + 2 * self.s, y + 2 * self.s, title)
        return i

    def text(self, parent, x, y, s, scale=None, color=0, limit=None):
        """Text cut where the box (or `limit` px from x) ends; returns the width drawn."""
        scale = scale or self.s
        bx, _, bw, _ = self.items[parent][1]
        room = limit if limit is not None else bx + bw - 2 * self.s - x
        s = clean(s)[:max(0, (room + scale) // (6 * scale))].rstrip()
        if not s:
            return 0
        self.c.text(x, y, s, scale, color)
        w = 6 * scale * len(s) - scale
        self.add("text", (x, y, w, 7 * scale), parent)
        return w

    def inner(self, b):
        x, y, w, h = b
        return x + 2 * self.s, y + 2 * self.s + 9 * self.s, w - 4 * self.s, h - 4 * self.s - 9 * self.s

    def graph(self, parent, x, y, w, h, values, vmax, slots):
        self.c.frame(x, y, w, h, 0, self.t)
        iw, ih = w - 2 * self.t - 1, h - 2 * self.t - 1
        vals = values[-slots:]
        pts = [(x + self.t + iw - round((len(vals) - 1 - i) * iw / max(slots - 1, 1)),
                y + self.t + ih - round(ih * min(max(v, 0), vmax) / max(vmax, 1e-9))) for i, v in enumerate(vals)]
        self.c.polyline(pts, 0, self.t + (self.s >= 4))
        self.add("graph", (x, y, w, h), parent)

    def down(self, parent, x, y, iw):
        self.text(parent, x, y, "FEED DOWN", 2 * self.s, limit=iw)
        self.text(parent, x, y + 17 * self.s, "RETRYING", self.s, limit=iw)

    def waiting(self, parent, x, y, iw):
        self.text(parent, x, y, "WAITING", self.s, limit=iw)


def rate_row(pg, parent, x, y, iw, ih, rate, series, label):
    """A big number with its label under it and the history graph to its right; returns the height used."""
    s, m = pg.s, 2 * pg.s
    chars = len(f"{rate:.1f}")
    bs = max(1, 8 * s)
    while bs > 1 and (7 * bs + 9 * s > ih or (6 * chars - 1) * bs > iw * 45 // 100):
        bs -= 1
    used = 7 * bs + 2 * s + 7 * s
    nw = pg.text(parent, x, y, f"{rate:.1f}", bs, limit=iw)
    pg.text(parent, x, y + 7 * bs + 2 * s, label, s, limit=max(nw, 6 * s * 8))
    gx = x + max(nw, 6 * s * len(label) - s) + m
    if iw - (gx - x) > 8 * s:
        pg.graph(parent, gx, y, iw - (gx - x), used, series, max(max(series, default=0), 1.0), len(series))
    return used


def coast_lines():
    return [[tuple(int(v) for v in p.split(",")) for p in ln.split(";")] for ln in COAST.split("|")]


_MAP_CACHE = {}


def disc(c, cx, cy, r, level):
    for dy in range(-r, r + 1):
        dx = int((r * r - dy * dy) ** 0.5 + 0.5)
        c.rect(cx - dx, cy + dy, 2 * dx + 1, 1, level)


def draw_map(pg, parent, x, y, w, h, quakes, latest):
    """Equirectangular world map on its own canvas (so nothing leaves the box), one dot per quake, sized by magnitude."""
    s = pg.s
    key = (w, h, pg.t)
    if key not in _MAP_CACHE:
        base = tpd.Canvas(w, h)
        base.frame(0, 0, w, h, 0, pg.t)
        proj = lambda lon, lat: (round((lon + 180) / 360 * (w - 1)), round((MAP_LAT_TOP - lat) / (MAP_LAT_TOP - MAP_LAT_BOTTOM) * (h - 1)))  # noqa: E731
        for line in coast_lines():
            base.polyline([proj(lo, la) for lo, la in line], 0, pg.t)
        _MAP_CACHE.clear()
        _MAP_CACHE[key] = (base, proj)
    base, proj = _MAP_CACHE[key]
    m = tpd.Canvas(w, h)
    m.px[:] = base.px
    pad = pg.t + 1
    for lat, lon, mag in quakes:
        if not (MAP_LAT_BOTTOM <= lat <= MAP_LAT_TOP):
            continue
        cx, cy = proj(lon, lat)
        r = max(1, round(s * (0.5 + 0.5 * max(mag, 0))))
        r = min(r, cx - pad, w - 1 - pad - cx, cy - pad, h - 1 - pad - cy)
        if r < 1:
            continue
        disc(m, cx, cy, r + pg.t, 15)
        disc(m, cx, cy, r, 0)
        pg.add("dot", (x + cx - r, y + cy - r, 2 * r + 1, 2 * r + 1), parent)
    if latest and MAP_LAT_BOTTOM <= latest[3] <= MAP_LAT_TOP:  # the newest one wears a ring
        cx, cy = proj(latest[4], latest[3])
        r = max(1, round(s * (0.5 + 0.5 * max(latest[0], 0)))) + 2 * s
        r = min(r, cx - pad, w - 1 - pad - cx, cy - pad, h - 1 - pad - cy)
        if r >= 2:
            disc(m, cx, cy, r, 0)
            disc(m, cx, cy, r - pg.t - 1, 15)
            disc(m, cx, cy, max(1, round(s * (0.5 + 0.5 * max(latest[0], 0)))), 0)
    for row in range(h):
        pg.c.px[(y + row) * pg.w + x:(y + row) * pg.w + x + w] = m.px[row * w:(row + 1) * w]
    pg.add("map", (x, y, w, h), parent)


def render(snap, w, h):
    pg = Page(w, h)
    c, s = pg.c, pg.s
    m, lh, hh = 2 * s, 9 * s, 11 * s
    c.rect(0, 0, w, hh, 0)
    head = pg.add("box", (0, 0, w, hh))
    right = snap["clock"]
    rw = 6 * s * len(right) - s
    pg.text(head, w - m - rw, 2 * s, right, color=15, limit=rw)
    pg.text(head, m, 2 * s, "LIVE WIRE", color=15, limit=w - 3 * m - rw)
    y0, y1 = hh + m, h - m
    lw = (w - 3 * m) * 45 // 100
    rwid = w - 3 * m - lw
    lx, rx = m, 2 * m + lw
    st = snap["status"]
    bsky = snap["bsky"]

    # left: the newest edited titles
    list_h = (y1 - y0) * 78 // 100 if bsky else y1 - y0
    b = (lx, y0, lw, list_h)
    i = pg.box(*b, "WIKIPEDIA EDITS NOW")
    x, y, iw, ih = pg.inner(b)
    if st["wiki"] == "down":
        pg.down(i, x, y, iw)
    elif not snap["titles"]:
        pg.waiting(i, x, y, iw)
    else:
        pitch = max(lh + s, ih // 12)
        for k, t in enumerate(snap["titles"][:max(1, ih // pitch)]):
            if (k + 1) * pitch > ih + (pitch - lh):
                break
            ty = y + k * pitch + (pitch - 7 * s) // 2
            if k == 0:
                c.rect(x - s, y + k * pitch, iw + 2 * s, pitch - s, 0)
            pg.text(i, x, ty, t, color=15 if k == 0 else 0, limit=iw)
    if bsky:
        b = (lx, y0 + list_h + m, lw, y1 - y0 - list_h - m)
        i = pg.box(*b, "BLUESKY")
        x, y, iw, ih = pg.inner(b)
        if st["bsky"] == "down":
            pg.down(i, x, y, iw)
        elif st["bsky"] == "wait":
            pg.waiting(i, x, y, iw)
        else:
            rate_row(pg, i, x, y, iw, ih, bsky[0], bsky[1], "POSTS/S")

    # right top: edit rate, graph, most edited article
    top_h = (y1 - y0 - m) * 38 // 100
    b = (rx, y0, rwid, top_h)
    i = pg.box(*b, "EDITS/S ALL WIKIS 10 MIN")
    x, y, iw, ih = pg.inner(b)
    if st["wiki"] == "down":
        pg.down(i, x, y, iw)
    elif st["wiki"] == "wait":
        pg.waiting(i, x, y, iw)
    else:
        used = rate_row(pg, i, x, y, iw, ih - 2 * lh, snap["rate"], snap["series"], "EDITS/S")
        if snap["top"] and ih >= used + m + 2 * lh:
            pg.text(i, x, y + used + m, "MOST EDITED 10 MIN", s, limit=iw)
            pg.text(i, x, y + used + m + lh, f"{snap['top'][0]}  X{snap['top'][1]}", s, limit=iw)

    # right bottom: quakes
    b = (rx, y0 + top_h + m, rwid, y1 - y0 - top_h - m)
    i = pg.box(*b, f"EARTHQUAKES 24 H  {len(snap['quakes'])}")
    x, y, iw, ih = pg.inner(b)
    tz = 14 * s + 2 * s + 7 * s + m
    mh_avail = ih - tz - m
    aspect = (MAP_LAT_TOP - MAP_LAT_BOTTOM) / 360
    mw = min(iw, int(mh_avail / aspect))
    if mw >= 20 * s:
        draw_map(pg, i, x, y, mw, int(mw * aspect), snap["quakes"], snap["latest"])
        ty = y + int(mw * aspect) + m
    else:
        ty = y
    if st["quake"] == "down":
        pg.text(i, x, ty, "FEED DOWN", 2 * s, limit=iw)
        pg.text(i, x, ty + 16 * s, "RETRYING", s, limit=iw)
    elif snap["latest"]:
        mag, place, age = snap["latest"][:3]
        pg.text(i, x, ty, f"M{mag:.1f}  {fmt_age(age)}", 2 * s, limit=iw)
        pg.text(i, x, ty + 16 * s, place, s, limit=iw)
    else:
        pg.waiting(i, x, ty, iw) if st["quake"] == "wait" else pg.text(i, x, ty, "NONE IN 24 H", s, limit=iw)
    return pg


def demo_snapshot():
    """Fixed fake events (docs and tests)."""
    titles = ["Kelly Clarkson discography", "2026 Atlantic hurricane season", "Rijksmuseum", "Tokyo", "Photosynthesis",
              "List of Dutch municipalities", "Eddy Merckx", "Quantum computing", "Cafe Central", "Mount Etna",
              "Template:Infobox person", "Zebra finch", "Gent", "Brussels Airport"]
    quakes = [(35.7, 139.7, 4.8), (-33.4, -70.6, 5.6), (61.2, -149.9, 2.1), (38.8, -122.8, 1.4), (-6.2, 106.8, 4.1),
              (19.4, -155.3, 2.9), (36.2, 71.5, 4.4), (-41.3, 174.8, 3.3), (64.1, -21.9, 1.9), (40.8, 14.3, 2.5),
              (28.2, 84.7, 3.8), (-17.8, -178.2, 5.1), (51.5, -0.1, 0.8), (13.7, 100.5, 3.0), (36.5, -121.2, 2.2),
              (-56.0, -26.0, 5.9), (-62.0, -58.0, 4.0), (71.0, 25.0, 6.2)]
    series = [4.5 + 3 * ((i * 7) % 11) / 10 + (i % 5) * 0.4 for i in range(60)]
    return {"clock": "14:32", "status": {"wiki": "ok", "quake": "ok", "bsky": "ok"}, "titles": titles[:12],
            "rate": 7.4, "series": series, "top": ("2026 Atlantic hurricane season", 17), "quakes": quakes,
            "latest": (4.8, "45 km E of Tokyo, Japan", 3, 35.7, 139.7), "bsky": (312.5, [250 + (i * 13) % 90 for i in range(60)])}


# ---------------------------------------------------------------- feeds (aiohttp is imported only here)
def log(*a):
    print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


async def wiki_feed(state, session):
    import aiohttp
    async with session.get(WIKI_URL, headers={"Accept": "text/event-stream"},
                           timeout=aiohttp.ClientTimeout(total=None, connect=15, sock_read=60)) as r:
        r.raise_for_status()
        state.status["wiki"] = "ok"
        p = SSEParser()
        async for raw in r.content:
            ev = p.feed(raw.decode("utf-8", "replace"))
            if ev and ev["event"] == "message":
                state.good.add("wiki")
                got = parse_recentchange(ev["data"])
                if got:
                    state.edit(got[0], got[1], time.time())


async def usgs_feed(state, session):
    while True:
        async with session.get(USGS_URL) as r:
            r.raise_for_status()
            state.quakes = merge_quakes(state.quakes, parse_usgs(await r.text()))
        state.status["quake"] = "ok"
        state.good.add("quake")
        await asyncio.sleep(60)


async def emsc_feed(state, session):
    async with session.ws_connect(EMSC_URL, heartbeat=30) as ws:
        async for msg in ws:
            q = parse_emsc(msg.data) if msg.type.name == "TEXT" else None
            state.good.add("emsc")
            if q:
                state.quakes = merge_quakes(state.quakes, [q])


async def bsky_feed(state, session):
    async with session.ws_connect(BSKY_URL, heartbeat=30, max_msg_size=0) as ws:
        state.status["bsky"] = "ok"
        async for msg in ws:  # posts only counted; the text is never read
            state.good.add("bsky")
            state.posts.add(time.time())


async def keep(name, status_key, fn, state, session, stop):
    """Run a feed forever: on any failure mark it down and reconnect with backoff (1 s doubling to 60 s)."""
    delay = 1
    while not stop.is_set():
        state.good.discard(name)
        try:
            await fn(state, session)
            err = "stream ended"
        except asyncio.CancelledError:
            raise
        except Exception as e:  # a feed must never take the page down
            err = f"{type(e).__name__}: {e}"
        if name in state.good:
            delay = 1
        if status_key:
            state.status[status_key] = "down"
        log(f"feed {name} down ({err}); retry in {delay} s")
        try:
            await asyncio.wait_for(stop.wait(), delay)
        except asyncio.TimeoutError:
            pass
        delay = min(delay * 2, 60)


# ---------------------------------------------------------------- outputs and the update loop
class DeviceOut:
    min_interval = MIN_SEND_S

    def __init__(self, port, depth, verbose):
        self.port, self.depth, self.verbose, self.d = port, depth, verbose, None

    def size(self):
        if self.d is None:
            self.d = tpd.open(self.port)
        return self.d.w, self.d.h

    def send(self, canvas):
        r = self.d.show(canvas, depth=self.depth)
        if self.verbose:
            log(f"show: host {r['prepare_ms']}+{r['transfer_ms']} ms, device {r.get('ms')} ms, {r.get('mode')}, {r.get('why')}")

    def reset(self):
        d, self.d = self.d, None
        try:
            if d:
                d.close()
        except Exception:  # noqa: BLE001 (the link is already gone)
            pass


class PngOut:
    def __init__(self, path, size, every):
        self.path, self.wh, self.min_interval = path, size, every

    def size(self):
        return self.wh

    def send(self, canvas):
        Path(self.path).write_bytes(canvas.png(1))

    def reset(self):
        pass


async def update_loop(state, out, stop):
    """Redraw only when the snapshot changed; send at most every out.min_interval s; a lost device is reopened."""
    last_snap, pending, last_send = None, None, 0.0
    while not stop.is_set():
        try:
            w, h = await asyncio.to_thread(out.size)
            snap = state.snapshot(time.time())
            if snap != last_snap:
                pending = (await asyncio.to_thread(render, snap, w, h)).c
                last_snap = snap
            if pending is not None and time.monotonic() - last_send >= out.min_interval:
                await asyncio.to_thread(out.send, pending)
                pending, last_send = None, time.monotonic()
        except tpd.DisplayError as e:
            log(f"display: {e}")
            if isinstance(e, tpd.Disconnected) or out.__class__ is DeviceOut and out.d is None:
                out.reset()
            await asyncio.sleep(3)
        except OSError as e:
            log(f"display: {e}")
            out.reset()
            await asyncio.sleep(3)
        try:
            await asyncio.wait_for(stop.wait(), TICK_S)
        except asyncio.TimeoutError:
            pass
    if pending is not None and isinstance(out, PngOut):
        out.send(pending)


async def run(args):
    import aiohttp
    state = State([w.strip() for w in args.wikis.split(",") if w.strip()], args.bluesky)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    if args.seconds:
        loop.call_later(args.seconds, stop.set)
    out = PngOut(args.png, tuple(int(v) for v in args.size.lower().split("x")), args.png_every) if args.png \
        else DeviceOut(args.port, 1, args.verbose)
    async with aiohttp.ClientSession(headers={"User-Agent": USER_AGENT}) as session:
        feeds = [("wiki", "wiki", wiki_feed), ("usgs", "quake", usgs_feed)]
        feeds += [("emsc", None, emsc_feed)] if args.emsc else []
        feeds += [("bsky", "bsky", bsky_feed)] if args.bluesky else []
        tasks = [asyncio.create_task(keep(n, k, f, state, session, stop)) for n, k, f in feeds]
        try:
            await update_loop(state, out, stop)
        finally:
            for t in tasks:
                t.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            out.reset()
    return state


def main(argv=None):
    env = os.environ.get
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 (default: the first device found)")
    ap.add_argument("--wikis", default=env("LIVE_WIRE_WIKIS", "enwiki,nlwiki"), help="wikis whose titles are listed (edit rate counts all)")
    ap.add_argument("--emsc", action="store_true", default=env("LIVE_WIRE_EMSC") == "1", help="also EMSC earthquakes by websocket")
    ap.add_argument("--bluesky", action="store_true", default=env("LIVE_WIRE_BLUESKY") == "1", help="Bluesky posts per second")
    ap.add_argument("--verbose", action="store_true", help="a line per show")
    ap.add_argument("--png", metavar="FILE", help="no device: write the page to a PNG (every --png-every s while the feeds run)")
    ap.add_argument("--png-every", type=float, default=10)
    ap.add_argument("--size", default="800x480", help="PNG size WxH")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (default: run until stopped)")
    ap.add_argument("--demo", action="store_true", help="with --png: one picture of fixed fake events, no network")
    args = ap.parse_args(argv)
    tpd.log(None)
    if args.png and args.demo:
        w, h = (int(v) for v in args.size.lower().split("x"))
        Path(args.png).write_bytes(render(demo_snapshot(), w, h).c.png(1))
        print(f"wrote {args.png} ({w}x{h})")
        return 0
    try:
        state = asyncio.run(run(args))
    except KeyboardInterrupt:
        return 0
    if args.png:
        n = len(state.edits.series(time.time()))
        print(f"edit rate at the end: {state.edits.rate(time.time()):.1f}/s; titles seen {len(state.titles)}; "
              f"quakes {len(state.quakes)}; status {state.status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
