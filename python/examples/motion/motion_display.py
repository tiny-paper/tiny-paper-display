#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Room motion on the display: one box per room (MOTION or QUIET, time since the change, today's count, the last
60 minutes as a strip), fed by MQTT (ESPectre through ESPHome/Home Assistant style topics).

    python motion_display.py                       (config.yml or the ROOMS variable; first device found)
    python motion_display.py --png page.png --size 800x480 --demo      (no device, no broker: fixed fake state)

Settings: config.yml (see config.yml) or the environment: MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD, CONFIG,
ROOMS="Living=espectre/living/motion,Kitchen=espectre/kitchen/motion|motion" (name=topic, then "|field" for a JSON field).
The page is drawn in memory and sent whole with Device.show; the device refreshes only what changed. It is redrawn when a
room changes and once a minute (clock, times, strip). The display and the broker are reconnected with a growing pause."""
import argparse
import json
import logging
import os
import sys
import threading
import time
from pathlib import Path

import tiny_paper_display as tpd

SLOTS = 60  # the strip: the last 60 minutes, one slot per minute
ON_WORDS = {"on", "true", "1", "motion", "detected", "occupied", "active", "yes"}
OFF_WORDS = {"off", "false", "0", "clear", "quiet", "none", "inactive", "no"}


# ---------------------------------------------------------------- payload to state (pure)
def dig(obj, path):
    """obj["a"]["b"] for "a.b"; None when a step is missing."""
    for key in path.split("."):
        if isinstance(obj, dict) and key in obj:
            obj = obj[key]
        else:
            return None
    return obj


def to_state(value, on=(), off=()):
    """One value (bool, number or text) -> True, False or None when it means neither. `on`/`off` are extra words."""
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return None if value not in (0, 1) else bool(value)
    if isinstance(value, str):
        v = value.strip().lower()
        if v in {w.lower() for w in on}:
            return True
        if v in {w.lower() for w in off}:
            return False
        return True if v in ON_WORDS else False if v in OFF_WORDS else None
    return None


def parse_payload(payload, field=None, on=(), off=()):
    """MQTT payload (bytes or str) -> True, False or None. Plain ON/OFF/true/false/1/0, or JSON with `field` ("a.b")."""
    text = payload.decode("utf-8", "replace") if isinstance(payload, (bytes, bytearray)) else str(payload)
    text = text.strip()
    if field:
        try:
            return to_state(dig(json.loads(text), field), on, off)
        except ValueError:
            return None
    state = to_state(text, on, off)
    if state is None:
        try:
            state = to_state(json.loads(text), on, off)
        except ValueError:
            pass
    return state


# ---------------------------------------------------------------- per-room state (pure; time is passed in)
class Room:
    def __init__(self, name, topic, field=None, on=(), off=()):
        self.name, self.topic, self.field, self.on, self.off = name, topic, field, tuple(on), tuple(off)
        self.state = None  # None: nothing heard yet
        self.since = None  # epoch seconds of the last change
        self.count, self.day = 0, None  # rising edges on `day` (local date)
        self.minutes = set()  # minute numbers (epoch // 60) in which motion was on
        self.last_mark = None

    def roll(self, now):
        """Day change resets the count; old minutes leave the strip; a running motion fills the minutes up to now."""
        day = time.strftime("%Y-%m-%d", time.localtime(now))
        if day != self.day:
            self.day, self.count = day, 0
        cur = int(now // 60)
        if self.state and self.last_mark is not None:
            self.minutes.update(range(self.last_mark, cur + 1))
        if self.state:
            self.last_mark = cur
        self.minutes = {m for m in self.minutes if m > cur - SLOTS}

    def update(self, state, now):
        """A new reading; returns True when the page must be redrawn (a change)."""
        self.roll(now)
        if state is None or state == self.state:
            return False
        if state:
            self.count += 1
            self.minutes.add(int(now // 60))
            self.last_mark = int(now // 60)
        self.state, self.since = state, now
        return True

    def timeline(self, now):
        """SLOTS booleans, oldest first, the last one is the current minute."""
        self.roll(now)
        cur = int(now // 60)
        return [(cur - SLOTS + 1 + i) in self.minutes for i in range(SLOTS)]

    def snapshot(self, now):
        return {"name": self.name, "state": self.state, "since_s": None if self.since is None else max(0, now - self.since),
                "count": self.count, "timeline": self.timeline(now)}


def fmt_since(sec):
    if sec is None:
        return ""
    sec = int(sec)
    if sec < 60:
        return f"{sec}S"
    if sec < 3600:
        return f"{sec // 60}M"
    if sec < 86400:
        return f"{sec // 3600}H {sec % 3600 // 60}M"
    return f"{sec // 86400}D {sec % 86400 // 3600}H"


# ---------------------------------------------------------------- drawing
class Page:
    """A Canvas plus a list of everything drawn: items (kind, (x, y, w, h), parent index or None), for the layout test."""

    def __init__(self, w, h):
        self.c = tpd.Canvas(w, h)
        self.w, self.h, self.items = w, h, []
        self.s = max(1, min(round(w / 360), round(h / 220)))  # text scale: glyphs 7 * s px tall
        self.t = max(1, self.s // 2)  # frame thickness

    def add(self, kind, rect, parent=None):
        self.items.append((kind, rect, parent))
        return len(self.items) - 1

    def text(self, parent, x, y, s, scale, color=0, limit=None):
        """Text cut where the box (or `limit` px) ends; returns the width drawn."""
        bx, _, bw, _ = self.items[parent][1]
        room = limit if limit is not None else bx + bw - self.s - x
        s = "".join(ch if ch.upper() in tpd.canvas.FONT else " " for ch in s)[:max(0, (room + scale) // (6 * scale))]
        if not s:
            return 0
        self.c.text(x, y, s, scale, color)
        self.add("text", (x, y, 6 * scale * len(s) - scale, 7 * scale), parent)
        return 6 * scale * len(s) - scale


def grid(n, w, h, top, m):
    """Rectangles for n rooms below `top`: one column up to 3 rooms, else two columns."""
    cols = 1 if n <= 3 else 2
    rows = -(-n // cols)
    bw, bh = (w - (cols + 1) * m) // cols, (h - top - (rows + 1) * m) // rows
    return [(m + (i % cols) * (bw + m), top + m + (i // cols) * (bh + m), bw, bh) for i in range(n)]


def draw_room(pg, snap, rect):
    s, (x, y, w, h) = pg.s, rect
    motion = snap["state"] is True
    ink, paper = (15, 0) if motion else (0, 15)
    pg.c.rect(x, y, w, h, paper)
    pg.c.frame(x, y, w, h, 0, pg.t)
    box = pg.add("box", rect)
    pad = 2 * s
    ix, iw = x + pad, w - 2 * pad
    lh = 9 * s
    cy = y + pad
    label = snap["name"]
    state = "MOTION" if motion else "QUIET" if snap["state"] is False else "NO DATA"
    # bottom strip first: it takes a fixed share, the text takes what is left
    strip_h = max(3 * s, (h - 2 * pad) // 6)
    slot = iw // SLOTS
    show_strip = slot >= 1 and h >= 2 * pad + lh + 7 * s + strip_h + 2 * s
    top_h = h - 2 * pad - (strip_h + 2 * s + (lh if h >= 2 * pad + lh * 3 + strip_h else 0) if show_strip else 0)
    # name (small, top left) and "since" (small, top right)
    pg.text(box, ix, cy, label, s, ink, limit=iw * 6 // 10)
    if snap["since_s"] is not None:
        st = f"{fmt_since(snap['since_s'])}  TODAY {snap['count']}"
        pg.text(box, ix + iw - (6 * s * len(st) - s) if 6 * s * len(st) - s < iw * 4 // 10 else ix + iw * 6 // 10 + s, cy, st, s, ink,
                limit=iw * 4 // 10 - s)
    body_y, body_h = cy + lh, top_h - lh
    if body_h >= 7:
        big = max(1, min((iw) // (6 * len(state)), body_h // 7, 8 * s))
        if 6 * big * len(state) - big <= iw:
            pg.text(box, ix, body_y + (body_h - 7 * big) // 2, state, big, ink, limit=iw)
    if show_strip:
        sy = y + h - pad - strip_h - (lh if h >= 2 * pad + lh * 3 + strip_h else 0)
        sw = slot * SLOTS
        pg.c.rect(ix - pg.t, sy - pg.t, sw + 2 * pg.t, strip_h + 2 * pg.t, 15)  # the strip is always black on white, also in a MOTION box
        pg.c.frame(ix, sy, sw, strip_h, 0, pg.t)
        gap = 1 if slot >= 4 else 0
        for i, on in enumerate(snap["timeline"]):
            if on:
                pg.c.rect(ix + i * slot + gap, sy + pg.t, slot - gap, strip_h - 2 * pg.t, 0)
        pg.add("strip", (ix, sy, sw, strip_h), box)
        if h >= 2 * pad + lh * 3 + strip_h:
            ly = sy + strip_h + s
            pg.text(box, ix, ly, "-60M", s, ink, limit=sw // 2)
            pg.text(box, ix + sw - (6 * s * 3 - s), ly, "NOW", s, ink, limit=6 * s * 3)


def render(rooms, w, h, clock):
    """rooms: list of snapshots; returns the Page."""
    pg = Page(w, h)
    s = pg.s
    hh = 11 * s
    pg.c.rect(0, 0, w, hh, 0)
    head = pg.add("header", (0, 0, w, hh))
    right = clock
    rw = 6 * s * len(right) - s
    pg.text(head, w - 2 * s - rw, 2 * s, right, s, 15, limit=rw)
    pg.text(head, 2 * s, 2 * s, "ROOM MOTION", s, 15, limit=w - 6 * s - rw)
    for snap, rect in zip(rooms, grid(len(rooms), w, h, hh, 2 * s)):
        draw_room(pg, snap, rect)
    return pg


def demo_rooms(now=None):
    """Fixed fake state for the picture and the tests: three rooms."""
    now = 1_760_000_000 if now is None else now
    def tl(*mins):
        return [i in mins for i in range(SLOTS)]
    return [{"name": "Living room", "state": True, "since_s": 190, "count": 14, "timeline": tl(*range(40, 60), 5, 6, 7, 20, 21)},
            {"name": "Kitchen", "state": False, "since_s": 1500, "count": 6, "timeline": tl(10, 11, 12, 30, 31, 32, 33)},
            {"name": "Bedroom", "state": False, "since_s": 5 * 3600 + 12 * 60, "count": 2, "timeline": tl()}]


# ---------------------------------------------------------------- configuration
def load_config(path=None, env=None):
    """-> (mqtt dict, [Room]) from config.yml (if the file exists) or the ROOMS variable."""
    env = os.environ if env is None else env
    path = Path(path or env.get("CONFIG", "config.yml"))
    cfg = {}
    if path.is_file():
        import yaml  # only needed with a config file
        cfg = yaml.safe_load(path.read_text()) or {}
    mq = {"host": "mosquitto", "port": 1883, "user": None, "password": None, **(cfg.get("mqtt") or {})}
    for key, var in (("host", "MQTT_HOST"), ("port", "MQTT_PORT"), ("user", "MQTT_USER"), ("password", "MQTT_PASSWORD")):
        if env.get(var):
            mq[key] = env[var]
    mq["port"] = int(mq["port"])
    rooms = []
    for r in cfg.get("rooms") or []:
        topic = r.get("topic") or r.get("state_topic")
        if not r.get("name") or not topic:
            raise ValueError(f"a room needs a name and a topic: {r}")
        rooms.append(Room(r["name"], topic, r.get("field"), _words(r.get("payload_on")), _words(r.get("payload_off"))))
    if env.get("ROOMS"):
        rooms = []
        for item in env["ROOMS"].split(","):
            name, _, rest = item.strip().partition("=")
            topic, _, field = rest.partition("|")
            if not name or not topic:
                raise ValueError(f"ROOMS entry needs name=topic: {item!r}")
            rooms.append(Room(name.strip(), topic.strip(), field.strip() or None))
    if not rooms:
        raise ValueError("no rooms: write them in config.yml or set ROOMS")
    return mq, rooms


def _words(v):
    return [] if v is None else [str(x) for x in v] if isinstance(v, (list, tuple)) else [str(v)]


# ---------------------------------------------------------------- the program
class App:
    def __init__(self, mq, rooms):
        self.mq, self.rooms = mq, rooms
        self.lock, self.wake, self.dirty = threading.Lock(), threading.Event(), True

    def on_message(self, topic, payload, now=None):
        now = time.time() if now is None else now
        with self.lock:
            for r in self.rooms:
                if r.topic == topic:
                    state = parse_payload(payload, r.field, r.on, r.off)
                    if state is None:
                        logging.getLogger("tiny_paper_display").warning("room %s: unreadable payload on %s", r.name, topic)
                    elif r.update(state, now):
                        self.dirty = True
                        self.wake.set()

    def snapshots(self, now):
        with self.lock:
            return [r.snapshot(now) for r in self.rooms]

    def start_mqtt(self):
        import paho.mqtt.client as mqtt
        try:
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        except AttributeError:  # paho 1.x
            client = mqtt.Client()
        if self.mq.get("user"):
            client.username_pw_set(self.mq["user"], self.mq.get("password"))
        client.reconnect_delay_set(1, 60)
        log = logging.getLogger("tiny_paper_display")

        def on_connect(c, userdata, flags, rc, *rest):
            log.info("mqtt connected to %s:%s (%s)", self.mq["host"], self.mq["port"], rc)
            for r in self.rooms:
                c.subscribe(r.topic)

        client.on_connect = on_connect
        client.on_disconnect = lambda *a: log.warning("mqtt disconnected, retrying")
        client.on_message = lambda c, u, m: self.on_message(m.topic, m.payload)
        client.connect_async(self.mq["host"], self.mq["port"])  # a broker that is not up yet is retried
        client.loop_start()
        return client

    def run(self):
        log = logging.getLogger("tiny_paper_display")
        client = self.start_mqtt()
        pause, d = 1, None
        try:
            while True:
                try:
                    if d is None:
                        d = tpd.open()
                        log.info("display %sx%s", d.w, d.h)
                    now = time.time()
                    page = render(self.snapshots(now), d.w, d.h, time.strftime("%H:%M", time.localtime(now)))
                    d.show(page.c)
                    pause = 1
                    self.dirty = False
                except (tpd.DisplayError, OSError) as e:
                    log.warning("display problem, retrying in %s s: %s", pause, e)
                    if d is not None:
                        try:
                            d.close()
                        except Exception:  # noqa: BLE001 (the port may be gone already)
                            pass
                        d = None
                    time.sleep(pause)
                    pause = min(pause * 2, 60)
                    continue
                self.wake.clear()
                self.wake.wait(60 - time.time() % 60 + 0.2)  # a change wakes it; else the next full minute
        except KeyboardInterrupt:
            return 0
        finally:
            client.loop_stop()
            if d is not None:
                d.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--png", metavar="FILE", help="write the page to a PNG and exit (no device, no broker)")
    ap.add_argument("--size", default="800x480", help="PNG size WxH (default 800x480)")
    ap.add_argument("--demo", action="store_true", help="with --png: fixed fake state")
    ap.add_argument("--config", help="config file (default: CONFIG or config.yml)")
    ap.add_argument("--log", default="info", help="log level (default info)")
    args = ap.parse_args(argv)
    tpd.log(args.log)
    if args.png:
        w, h = (int(v) for v in args.size.lower().split("x"))
        rooms = demo_rooms()
        Path(args.png).write_bytes(render(rooms, w, h, "14:32").c.png(1))
        print(f"wrote {args.png} ({w}x{h})")
        return 0
    mq, rooms = load_config(args.config)
    return App(mq, rooms).run()


if __name__ == "__main__":
    sys.exit(main())
