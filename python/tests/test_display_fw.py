# SPDX-License-Identifier: MIT
"""The display firmware's path against docs/spec/vectors/usb-display.json, no device: every request is the vector's
bytes, every answer decodes to the vector's map, and show, clear and the refusals run against a device replayed from
the vectors (the same steps the firmware's host test replays)."""
import json
import struct
import sys
import zlib

import pytest

from tiny_paper_display import display_fw as fw
from tiny_paper_display import cli_show as show
from conftest import FakePort, png
from fake_display_fw import FakeDevice
from tiny_paper_display.usb_display import ROOT, Display, DisplayError, codec

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
    for st in list(pairs()) + V["refusals"]:
        assert codec.build_frame(st["seq"], st["cmd"], plain(st["req"])).hex() == st["tx"], st.get("name")
        items = [it for it in codec.LinkParser().feed(b"# a log line\n" + bytes.fromhex(st["rx"])) if it[0] != "text"]
        assert [(it[1], it[2], codec.cbor_decode(it[3])) for it in items] == [(st["seq"], st["cmd"] | 0x80, plain(st["resp"]))]
        n += 1
    assert n > 50


def device(name, **more):
    """The fake firmware with the session's panel."""
    p = session(name)["panel"]
    return FakeDevice(p["w"], p["h"], p["depths"], p["partial_depths"], p["partial_all"], p["full_every"], p["board"],
                      p["fw"], clean_every=p.get("clean_every", 15), **more)


def replay(name, busy_first=0, hello=None):
    """A device with the session's panel behind a port; busy_first: that many requests are answered BUSY."""
    dev = device(name, hello=hello)
    state = {"busy": busy_first, "seen": [], "device": dev}

    def respond(wire):
        seq, cmd = wire[4:6]
        state["seen"].append(cmd)
        if cmd != fw.HELLO and state["busy"]:
            state["busy"] -= 1
            return codec.build_frame(seq, cmd | 0x80, {"err": 3, "msg": "refreshing", "detail": {"retry_ms": 1}})
        return dev.respond(wire)

    return FakePort(respond), state


def test_fake_device_answers_as_the_vectors():
    """The fake firmware of these tests against the gray session: every answer of a step that is not refused is the
    vector's, a refused step is refused with the same code, and its glass is the vector's pixels."""
    dev, shown, refused = device("gray"), 0, 0
    for st in session("gray")["steps"]:
        wire = bytes.fromhex(st["tx"])
        items = [it for it in codec.LinkParser().feed(dev.respond(wire)) if it[0] == "frame"]
        reply, want = codec.cbor_decode(items[0][3]), plain(st["resp"])
        if "err" in want:
            assert reply.get("err") == want["err"], st["name"]
            refused += 1
        else:
            assert reply == want, st["name"]
        if "pixels" in st:
            assert dev.pixels().hex() == st["pixels"], st["name"]
            shown += 1
    assert shown >= 20 and refused >= 30


def picture(name):
    return bytes.fromhex(V["pictures"][name]["png"])


def test_hello_show_clear():
    port, state = replay("gray")
    d = Display(port)
    hello = d.hello()
    assert fw.is_display_firmware(hello) and (hello["w"], hello["h"], hello["depths"]) == (16, 8, [1, 2])
    assert hello["partial_depths"] == [1] and hello["modes"] == list(fw.MODES) and hello["full_every"] == 20
    whole = {"x": 0, "y": 0, "w": 16, "h": 8}
    zero = dict.fromkeys(("recv", "prep", "inflate", "convert", "diff", "fingerprint", "panel", "clean", "total"), 0)
    assert fw.show_png(d, picture("gray2"), hello) == {"depth": 2, "ms": 0, "mode": "full", "why": "depth", **whole, "steps": zero}
    assert fw.clear(d) == {"depth": 1, "ms": 0, "mode": "full", "why": "asked", **whole}
    assert state["seen"] == [fw.HELLO, fw.SHOW_BEGIN, fw.SHOW_CHUNK, fw.SHOW_END, fw.CLEAR]


def test_partial_is_the_default_and_the_modes():
    """A plain show updates what changed; show_area and each mode send the keys of the spec; the answer says what ran."""
    port, state = replay("gray")
    d, dev = Display(port), state["device"]
    hello = d.hello()
    rows = bytearray(b"\xff" * 16)
    assert fw.show_png(d, fw.write_png(16, 8, 1, bytes(rows)), hello)["why"] == "first"
    rows[5] = 0x0f  # the right byte of row 2
    reply = fw.show_png(d, fw.write_png(16, 8, 1, bytes(rows)), hello)
    assert (reply["mode"], reply["why"], reply["x"], reply["y"], reply["w"], reply["h"]) == ("partial", "changed", 8, 2, 8, 1)
    assert fw.describe(reply) == "partial (changed) 8,2 8x1 depth 1 0 ms [steps recv 0, prep 0, inflate 0, convert 0, diff 0, fingerprint 0, panel 0, total 0]"
    assert fw.show_png(d, fw.write_png(16, 8, 1, bytes(rows)), hello)["mode"] == "none"
    box = fw.write_png(8, 4, 1, bytes(4))
    assert fw.show_area(d, box, hello, 0, 4)["mode"] == "partial"
    for mode, passes in (("partial_all", None), ("inverse", None), ("passes", 3), ("full", None), ("partial", None)):
        reply = fw.show_area(d, box, hello, 8, 0, mode, passes, force=True)
        assert reply["mode"] == ("none" if mode == "partial" else mode), mode
        begin = [p for c, p in dev.requests if c == fw.SHOW_BEGIN][-1]
        assert list(begin) == ["size", "x", "y", "mode"] + ["passes"] * bool(passes) and begin["mode"] == mode
    assert dev.pixels()[:16] == b"\x01" * 8 + bytes(8) and dev.pixels()[16 * 4:16 * 4 + 16] == bytes(8) + b"\x01" * 8
    assert fw.full_every(d) == 20 and fw.full_every(d, 3) == 3 and fw.full_every(d) == 3
    with pytest.raises(fw.Refused) as e:
        fw.full_every(d, 251)
    assert e.value.err == 2


def test_modes_the_board_cannot_do_are_not_sent():
    """The library refuses before sending what hello says the board cannot do; the device refuses it too."""
    dev = device("bwry")
    dev.depths = [1]
    d = Display(FakePort(dev.respond))
    hello = d.hello()
    png1 = fw.write_png(16, 8, 1, bytes(16))
    before = len(dev.requests)
    for kw in ({"mode": "partial"}, {"mode": "inverse"}, {"x": 0, "y": 0}, {"mode": "passes", "passes": 2}):
        with pytest.raises(DisplayError, match="does not do|no partial refresh"):
            fw.show_png(d, png1, hello, **kw)
    for kw, text in (({"x": 0}, "come together"), ({"mode": "fast"}, "mode is one of"), ({"passes": 2}, "only with it"),
                     ({"mode": "passes"}, "only with it")):
        with pytest.raises(DisplayError, match=text):
            fw.show_png(d, png1, hello, **kw)
    assert len(dev.requests) == before
    with pytest.raises(fw.Refused) as e:  # the device itself, asked behind the library's back
        fw.request(d, fw.SHOW_BEGIN, {"size": len(png1), "mode": "partial"})
    assert e.value.err == 36 and "MODE 36" in str(e.value)
    assert fw.show_png(d, png1, hello)["why"] == "board"
    # a firmware from before the modes: a plain show still works, anything else is refused here
    old = {k: v for k, v in hello.items() if k not in ("modes", "partial_depths", "full_every")}
    assert fw.target(old) == {} and fw.target(old, mode="full") == {}
    with pytest.raises(DisplayError, match="update it"):
        fw.target(old, mode="partial")


def test_changed_rect_and_crop():
    a = bytearray(b"\xff" * (4 * 6))  # 32 x 6 at 1 bit
    b = bytearray(a)
    assert fw.changed_rect(bytes(a), bytes(b), 32, 6, 1) is None
    b[1 * 4 + 2] = 0xfe
    b[3 * 4 + 3] = 0x7f
    rect = fw.changed_rect(bytes(a), bytes(b), 32, 6, 1)
    assert rect == (16, 1, 16, 3)
    w, h, bits, rows = fw.png_rows(fw.crop_png(bytes(b), 32, 1, rect))
    assert (w, h, bits, rows) == (16, 3, 1, b"\xfe\xff\xff\xff\xff\x7f")
    # 4 bit: a byte is two pixels, the rectangle still starts and ends on a multiple of 8
    a4, b4 = bytes(16 * 2), bytearray(16 * 2)
    b4[16 + 5] = 0x10
    assert fw.changed_rect(a4, bytes(b4), 32, 2, 4) == (8, 1, 8, 1)


def test_busy_guard():
    port, state = replay("gray", busy_first=2)
    assert fw.clear(Display(port))["mode"] == "full"
    assert state["seen"] == [fw.CLEAR] * 3
    port, state = replay("gray", busy_first=5)
    with pytest.raises(fw.Refused) as e:
        fw.request(Display(port), fw.CLEAR, busy_wait=0)
    assert e.value.err == 3 and state["seen"] == [fw.CLEAR]


def test_refusal_guard():
    port, _ = replay("gray")
    d = Display(port)
    hello = d.hello()
    with pytest.raises(fw.Refused) as e:
        fw.show_png(d, picture("gray4"), hello)
    assert e.value.err == 34 and "DEPTH 34" in str(e.value)
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


def test_capabilities_with_and_without_the_new_fields():
    port, state = replay("gray")
    caps = fw.capabilities(Display(port))
    assert (caps.w, caps.h, caps.depths, caps.partial_depths) == (16, 8, [1, 2], [1])
    assert caps.depth_modes == {1: ["full", "partial", "partial_all", "inverse", "passes"], 2: ["full"]}
    assert caps.max_png == state["device"].max_png and caps.colors == []
    assert caps.full_every == 20 and caps.full_every_set is False
    # an older firmware: no depth_modes, no full_every_set; nothing invented
    old = {k: v for k, v in state["device"].hello().items() if k not in ("depth_modes", "full_every_set")}
    caps = fw.parse_capabilities(old)
    assert caps.depth_modes is None and caps.full_every_set is None and caps.modes == list(fw.MODES)
    assert fw.parse_capabilities({k: v for k, v in old.items() if k != "full_every"}).full_every is None


def test_full_every_clear():
    port, state = replay("gray")
    d, dev = Display(port), state["device"]
    assert fw.full_every(d, 7) == 7 and dev.every_set
    assert fw.full_every(d, clear=True) == 20 and not dev.every_set  # the board's default again
    begin = [p for c, p in dev.requests if c == fw.SET][-1]
    assert begin == {"full_every": None}
    with pytest.raises(DisplayError):
        fw.full_every(d, 3, clear=True)
