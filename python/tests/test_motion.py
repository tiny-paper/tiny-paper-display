# SPDX-License-Identifier: MIT
"""examples/motion without a device or a broker: the page layout (no overlap, text inside its box) for 1, 3 and 6 rooms
on both panel sizes, the payload mapping, the 60-minute strip, the config, and that the compose file parses."""
import importlib.util
import json
import time
from pathlib import Path

import pytest

yaml = pytest.importorskip("yaml", reason="pyyaml: pip install -e './python[test]'")

DIR = Path(__file__).resolve().parents[1] / "examples" / "motion"
spec = importlib.util.spec_from_file_location("motion_display", DIR / "motion_display.py")
md = importlib.util.module_from_spec(spec)
spec.loader.exec_module(md)

SIZES = [(1872, 1404), (800, 480)]


def inside(a, b):
    return a[0] >= b[0] and a[1] >= b[1] and a[0] + a[2] <= b[0] + b[2] and a[1] + a[3] <= b[1] + b[3]


def apart(a, b):
    return a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]


def rooms(n):
    base = md.demo_rooms()
    states = [True, False, None]
    return [{**base[i % 3], "name": f"Room number {i + 1}", "state": states[i % 3],
             "since_s": None if states[i % 3] is None else base[i % 3]["since_s"]} for i in range(n)]


@pytest.mark.parametrize("w,h", SIZES)
@pytest.mark.parametrize("n", [1, 3, 6])
def test_layout(n, w, h):
    pg = md.render(rooms(n), w, h, "14:32")
    items = pg.items
    boxes = [(i, r) for i, (k, r, p) in enumerate(items) if p is None]
    assert len(boxes) == n + 1  # the header and one box per room
    for a_i, (i, a) in enumerate(boxes):
        assert inside(a, (0, 0, w, h)), f"box {i} {a} leaves the canvas"
        for j, b in boxes[a_i + 1:]:
            assert apart(a, b), f"boxes {a} and {b} overlap"
    for i, (kind, r, parent) in enumerate(items):
        if parent is None:
            continue
        assert inside(r, items[parent][1]), f"{kind} {r} leaves its box {items[parent][1]}"
        for j, (k2, r2, p2) in enumerate(items):
            if j > i and p2 == parent:
                assert apart(r, r2), f"{kind} {r} overlaps {k2} {r2}"
    texts = [r for k, r, p in items if k == "text" and p is not None and items[p][0] == "box"]
    assert len(texts) >= n  # at least one text per room box survives at the smallest size


def test_long_name_is_cut_inside_its_box():
    snap = rooms(6)
    snap[0]["name"] = "A very long room name that does not fit anywhere at all"
    pg = md.render(snap, 800, 480, "14:32")
    for kind, r, parent in pg.items:
        if parent is not None:
            assert inside(r, pg.items[parent][1])


def test_png_renders():
    assert md.render(md.demo_rooms(), 800, 480, "14:32").c.png(1).startswith(b"\x89PNG")


@pytest.mark.parametrize("payload,field,want", [
    (b"ON", None, True), (b"OFF", None, False), (b"on", None, True), (b"true", None, True), (b"False", None, False),
    (b"1", None, True), (b"0", None, False), (b"garbage", None, None), (b"", None, None),
    (b'{"motion": true}', "motion", True), (b'{"motion": false}', "motion", False),
    (b'{"a": {"b": "ON"}}', "a.b", True), (b'{"motion": 1}', "motion", True), (b'{"other": 1}', "motion", None),
    (b"not json", "motion", None), (b'{"motion": 7}', "motion", None), (b"true", "motion", None),
])
def test_payload_mapping(payload, field, want):
    assert md.parse_payload(payload, field) is want


def test_custom_words():
    assert md.parse_payload(b"detected_x", None, on=["detected_x"]) is True
    assert md.parse_payload(b"calm", None, off=["calm"]) is False


def test_timeline_slots_and_count():
    t0 = 1_760_000_040.0  # 40 s into a minute
    r = md.Room("x", "t")
    assert r.update(True, t0) and r.count == 1
    assert not r.update(True, t0 + 1) and r.count == 1  # same state: no redraw, no second count
    assert r.update(False, t0 + 30) and r.count == 1
    tl = r.timeline(t0 + 30)
    assert len(tl) == 60 and tl[-1] and sum(tl) == 1
    assert r.update(True, t0 + 600) and r.count == 2  # ten minutes later
    tl = r.timeline(t0 + 600)
    assert tl[-1] and tl[-11] and sum(tl) == 2
    # motion stays on across three minutes with no new message: the minutes in between are filled
    tl = r.timeline(t0 + 600 + 180)
    assert all(tl[-4:]) and sum(tl) == 5
    # an hour later the old slots have left the strip
    r.update(False, t0 + 600 + 200)
    assert sum(r.timeline(t0 + 600 + 200 + 3700)) == 0


def test_count_resets_on_a_new_day():
    day1 = time.mktime((2026, 10, 14, 23, 50, 0, 0, 0, -1))
    r = md.Room("x", "t")
    r.update(True, day1)
    r.update(False, day1 + 10)
    assert r.count == 1
    r.update(True, day1 + 1200)  # past midnight
    assert r.count == 1  # the new day's first, the old one is gone


def test_unreadable_payload_changes_nothing():
    app = md.App({}, [md.Room("x", "t")])
    app.dirty = False
    app.on_message("t", b"???", now=1e9)
    assert app.rooms[0].state is None and not app.dirty
    app.on_message("t", b"ON", now=1e9)
    assert app.rooms[0].state is True and app.dirty


def test_config_file_and_env(tmp_path):
    mq, rs = md.load_config(DIR / "config.yml", env={})
    assert mq["host"] == "mosquitto" and [r.name for r in rs] == ["Living room", "Kitchen", "Bedroom"]
    assert rs[1].field == "motion"
    mq, rs = md.load_config(tmp_path / "none.yml", env={"ROOMS": "A=x/a,B=x/b|m.n", "MQTT_HOST": "h", "MQTT_PORT": "1884"})
    assert mq["host"] == "h" and mq["port"] == 1884 and rs[1].field == "m.n" and rs[0].topic == "x/a"
    with pytest.raises(ValueError):
        md.load_config(tmp_path / "none.yml", env={})
    bad = tmp_path / "bad.yml"
    bad.write_text("rooms:\n  - name: NoTopic\n")
    with pytest.raises(ValueError):
        md.load_config(bad, env={})


def test_demo_topics_match_config():
    """The simulator publishes where config.yml listens, in the style each room expects."""
    spec2 = importlib.util.spec_from_file_location("simulator", DIR / "simulator.py")
    pytest.importorskip("paho.mqtt.client")
    sim = importlib.util.module_from_spec(spec2)
    spec2.loader.exec_module(sim)
    _, rs = md.load_config(DIR / "config.yml", env={})
    assert {r.topic for r in rs} == set(sim.ROOMS)
    for r in rs:
        for on in (True, False):
            assert md.parse_payload(sim.ROOMS[r.topic](on), r.field, r.on, r.off) is on


def test_compose_file_parses():
    c = yaml.safe_load((DIR / "docker-compose.yml").read_text())
    s = c["services"]
    assert set(s) == {"mosquitto", "motion-display", "simulator"}
    assert s["simulator"]["profiles"] == ["demo"]
    assert "/dev/ttyACM0:/dev/ttyACM0" in s["motion-display"]["devices"]
    assert s["motion-display"]["restart"] == "unless-stopped"
    assert "mosquitto-data" in c["volumes"]
    root = DIR.parents[2]
    for name in ("motion-display", "simulator"):
        b = s[name]["build"]
        assert (DIR / b["context"]).resolve() == root
        assert (root / b["dockerfile"]).is_file()
    for v in s["mosquitto"]["volumes"] + s["motion-display"]["volumes"]:
        src = v.split(":")[0]
        assert src in c["volumes"] or (DIR / src).exists(), src
    assert (DIR / "Dockerfile").read_text().startswith("# Build from") and "python:3.12-slim" in (DIR / "Dockerfile").read_text()
    json.dumps(c)


class FakeMqtt:
    """paho's client as far as the simulator uses it: a publish before the connection exists is dropped (as paho does
    with no broker), a publish after it reaches the display's message handler."""
    CallbackAPIVersion = type("V", (), {"VERSION2": 2})

    def __init__(self, deliver):
        self.deliver, self.connected, self.on_connect = deliver, False, None

    def Client(self, *a):  # noqa: N802 (mirrors paho)
        return self

    def reconnect_delay_set(self, *a):
        pass

    def connect_async(self, *a):
        pass

    def loop_start(self):
        pass

    def publish(self, topic, payload, retain=False, **kw):
        if self.connected:
            self.deliver(topic, payload.encode() if isinstance(payload, str) else payload)

    def connect_now(self):  # the broker answers some time after the simulator started
        self.connected = True
        if self.on_connect:
            self.on_connect(self, None, {}, 0)


def test_simulator_reaches_every_room_when_the_broker_comes_up_late(monkeypatch):
    """The simulator's publishing and the display's handler on the same config: no room stays NO DATA. Seen on a PC
    with docker compose --profile demo: all rooms NO DATA for 60 s, the first publishes were made before the connection."""
    sspec = importlib.util.spec_from_file_location("simulator", DIR / "simulator.py")
    pytest.importorskip("paho.mqtt.client")
    sim = importlib.util.module_from_spec(sspec)
    sspec.loader.exec_module(sim)
    mq, rs = md.load_config(DIR / "config.yml", env={})
    app = md.App(mq, rs)
    fake = FakeMqtt(app.on_message)
    monkeypatch.setattr(sim, "mqtt", fake)
    calls = []

    class Stop(Exception):
        pass

    def fake_sleep(_):
        calls.append(1)
        if len(calls) == 1:
            fake.connect_now()
        elif len(calls) > 2:
            raise Stop

    monkeypatch.setattr(sim.time, "sleep", fake_sleep)
    with pytest.raises(Stop):
        sim.main()
    states = [s["state"] for s in app.snapshots(time.time())]
    assert all(st is not None for st in states), states
