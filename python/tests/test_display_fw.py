# SPDX-License-Identifier: MIT
"""The display firmware's path against docs/spec/vectors/usb-display.json, no device: every request is the vector's
bytes, every answer decodes to the vector's map, and show, clear and the refusals run against a device replayed from
the vectors (the same steps the firmware's host test replays)."""
import json
import struct
import sys
import zlib

import pytest

from tinypaper_display import display_fw as fw
from tinypaper_display import show
from conftest import FakePort, png
from tinypaper_display.usb_display import ROOT, Display, DisplayError, codec

V = json.loads((ROOT / "docs/spec/vectors/usb-display.json").read_text())


def plain(v):
    """The vector file's readable payload back to values (bytes are {"hex": ...})."""
    if isinstance(v, dict):
        return bytes.fromhex(v["hex"]) if set(v) == {"hex"} else {k: plain(x) for k, x in v.items()}
    return v


def readable(v):
    if isinstance(v, (bytes, bytearray)):
        return {"hex": bytes(v).hex()}
    if isinstance(v, dict):
        return {k: readable(x) for k, x in v.items()}
    return v


def session(name):
    return next(s for s in V["sessions"] if s["name"] == name)


def pairs():
    for s in V["sessions"]:
        for st in s["steps"]:
            yield st
            yield from st.get("during", [])


def test_requests_and_answers_are_the_vector_bytes():
    n = 0
    for st in pairs():
        assert codec.build_frame(st["seq"], st["cmd"], plain(st["req"])).hex() == st["tx"], st.get("name")
        items = [it for it in codec.LinkParser().feed(b"# a log line\n" + bytes.fromhex(st["rx"])) if it[0] != "text"]
        assert [(it[1], it[2], codec.cbor_decode(it[3])) for it in items] == [(st["seq"], st["cmd"] | 0x80, plain(st["resp"]))]
        n += 1
    assert n > 50


def replay(name, busy_first=0, hello=None):
    """A device that answers as the session's steps do, whatever seq the client uses."""
    s = session(name)
    steps = {(st["cmd"], json.dumps(st["req"], sort_keys=True)): st for st in s["steps"]}
    # show_end has no payload: its answer is the one that followed the same PNG bytes in the session
    ends, sent, state = {}, b"", {"data": b"", "busy": busy_first, "seen": []}
    for st in s["steps"]:
        if st["cmd"] == fw.SHOW_BEGIN:
            sent = b""
        elif st["cmd"] == fw.SHOW_CHUNK and "err" not in st:
            sent += plain(st["req"])["data"]
        elif st["cmd"] == fw.SHOW_END:
            ends[sent] = plain(st["resp"])

    def respond(wire):
        seq, cmd = wire[4:6]
        payload = codec.cbor_decode(wire[6:-2]) if len(wire) > 8 else {}
        state["seen"].append(cmd)
        if cmd == fw.HELLO:
            resp = hello or plain(s["steps"][0]["resp"])
        elif state["busy"]:
            state["busy"] -= 1
            resp = {"err": 3, "msg": "refreshing", "detail": {"retry_ms": 1}}
        elif cmd == fw.SHOW_END:
            resp = ends[state["data"]]
        else:
            key = (cmd, json.dumps(readable(payload), sort_keys=True))
            assert key in steps, f"a request the vectors do not have: {key}"
            resp = plain(steps[key]["resp"])
            if cmd == fw.SHOW_BEGIN:
                state["data"] = b""
            elif cmd == fw.SHOW_CHUNK and "err" not in resp:
                state["data"] += payload["data"]
        return codec.build_frame(seq, cmd | 0x80, resp)

    return FakePort(respond), state


def picture(name):
    return bytes.fromhex(V["pictures"][name]["png"])


def test_hello_show_clear():
    port, state = replay("gray")
    d = Display(port)
    hello = d.hello()
    assert fw.is_display_firmware(hello) and (hello["w"], hello["h"], hello["depths"]) == (16, 8, [1, 2])
    assert fw.show_png(d, picture("gray2"), hello) == {"depth": 2, "ms": 0}
    assert fw.clear(d) == {"depth": 1, "ms": 0}
    assert state["seen"] == [fw.HELLO, fw.SHOW_BEGIN, fw.SHOW_CHUNK, fw.SHOW_END, fw.CLEAR]


def test_busy_guard():
    port, state = replay("gray", busy_first=2)
    assert fw.clear(Display(port)) == {"depth": 1, "ms": 0}
    assert state["seen"] == [fw.CLEAR] * 3
    port, state = replay("gray", busy_first=5)
    with pytest.raises(fw.Refused) as e:
        fw.request(Display(port), fw.CLEAR, busy_wait=0)
    assert e.value.err == 3 and state["seen"] == [fw.CLEAR]


def test_refusal_guard():
    port, _ = replay("bwry")
    d = Display(port)
    hello = d.hello()
    assert hello["colors"] == ["black", "white", "red", "yellow"]
    with pytest.raises(fw.Refused) as e:
        fw.show_png(d, picture("gray2"), hello)
    assert e.value.err == 34 and "indexed PNG" in e.value.msg and "DEPTH 34" in str(e.value)
    with pytest.raises(DisplayError, match="the device takes"):
        fw.show_png(d, bytes(hello["max_png"] + 1), hello)


def test_reply_timeout_guard():
    d = Display(FakePort(lambda wire: b""))
    with pytest.raises(DisplayError, match="no reply"):
        fw.request(d, fw.SHOW_END, timeout=0.05)
    assert len(d.link.ser.port.writes) == 1  # not sent again: the device would run it twice


def rows_of(data):
    return zlib.decompress(data[data.index(b"IDAT") + 4:-16])


def test_panel_png_guard():
    gray, bwry = plain(session("gray")["steps"][0]["resp"]), plain(session("bwry")["steps"][0]["resp"])
    for name in ("gray1", "gray2"):
        assert fw.png_fits(picture(name), gray) and fw.panel_png(picture(name), gray) == picture(name)
    for other in (picture("gray4"), picture("bwry"), png(8, 8, bytes(8 * 25)), b"not a png"):
        assert not fw.png_fits(other, gray)
    # a picture of another size and kind: fitted on white at the panel's deepest depth, a PNG the device takes
    rgb = png(4, 2, b"".join(b"\x00" + bytes([0, 0, 0, 255, 255, 255, 255, 0, 0, 40, 40, 40]) for _ in range(2)))
    out = fw.panel_png(rgb, gray)
    assert fw.png_fits(out, gray) and struct.unpack(">IIBB", out[16:26]) == (16, 8, 2, 0)
    assert rows_of(out) == bytes([0, 0x00, 0xFF, 0x55, 0x00]) * 8  # a filter byte, then 4 pixels each: black, white, dark gray, black
    out = fw.panel_png(rgb, bwry)
    assert struct.unpack(">IIBB", out[16:26]) == (16, 8, 2, 3) and fw.BWRY in out
    assert rows_of(out) == bytes([0, 0x00, 0xFF, 0x55, 0x00]) * 8  # codes: black 0, white 3, red 1, black 0
    with pytest.raises(DisplayError):
        fw.write_png(16, 8, 2, bytes(3))


class Port(FakePort):
    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


@pytest.mark.parametrize("argv, want", [(["bars.png"], [1, 8, 9, 10]), (["--clear"], [1, 11])])
def test_show_command_on_the_display_firmware(monkeypatch, capsys, argv, want):
    port, state = replay("gray")
    port.__class__ = Port
    monkeypatch.setattr(codec, "open_serial", lambda name: port)
    monkeypatch.setattr(fw, "panel_png", lambda data, hello: picture("gray1"))
    monkeypatch.setattr(sys, "argv", ["show.py", "port"] + [str(ROOT / "python/examples" / a) if a[0] != "-" else a for a in argv])
    assert show.main() == 0
    out = capsys.readouterr().out
    assert "vec_gray display firmware" in out and '"ms": 0' in out and state["seen"] == want


def test_clear_needs_the_display_firmware(monkeypatch, capsys):
    port, _ = replay("gray", hello={"proto": 1, "board": "sim", "album": 1, "max_payload": 6144})
    port.__class__ = Port
    monkeypatch.setattr(codec, "open_serial", lambda name: port)
    monkeypatch.setattr(sys, "argv", ["show.py", "port", "--clear"])
    assert show.main() == 1
    assert "needs the display firmware" in capsys.readouterr().err
