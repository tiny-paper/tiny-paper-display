# SPDX-License-Identifier: MIT
"""The display firmware over Bluetooth with a fake bleak and a fake firmware behind it: frames split over writes and
notifications, lost bytes, an unbonded peer, `bt` only over the cable, and the vectors' "bluetooth" session."""
import json
import random
import sys

import pytest

from tiny_paper_display import ble_port
from tiny_paper_display import display_fw as fw
from tiny_paper_display import cli_show as show
from fake_ble import Air, install
from fake_display_fw import FakeDevice
from test_display_fw import V, plain
from tiny_paper_display.usb_display import CableOnly, Display, DisplayError, NotPaired, codec, open_port

NAME = "tp-display-VEC0"
NOT_PAIRED = "not paired: run setup with the cable and press the device's button"


def noisy(w, h, bits, seed=1):
    """A PNG that does not compress: its frame is longer than a piece, and for 128 x 128 at 2 bit than a chunk."""
    rng = random.Random(seed)
    rows = bytes(rng.randrange(256) for _ in range((w * bits + 7) // 8 * h))
    return fw.write_png(w, h, bits, rows), rows


def make(monkeypatch, w=16, h=8, bt="on", **air):
    dev = FakeDevice(w, h, (1, 2), (1,), True, 20, "vec_gray", "0.0.0+vec", bt=bt, name=NAME)
    return dev, install(monkeypatch, Air(dev, name=NAME, **air))


def quick(display, timeout=0.3):
    """The device's answers wait 3 s by default; a test that loses one should not."""
    ask = display.link.request
    display.link.request = lambda cmd, data, _t, retries: ask(cmd, data, timeout, retries)
    return display


@pytest.mark.parametrize("mtu", [23, 247])
def test_frame_is_split_at_mtu_minus_3_and_only_the_last_piece_has_a_response(monkeypatch, mtu):
    dev, air = make(monkeypatch, 64, 64, mtu=mtu)
    png, rows = noisy(64, 64, 1)
    with open_port(bluetooth=NAME) as port:
        display = Display(port)
        hello = display.hello()
        reply = fw.show_png(display, png, hello)
    assert reply["mode"] == "full" and dev.glass == rows
    assert max(len(p) for p, _ in air.writes) == mtu - 3  # a long frame fills the pieces
    frames = air.frames()
    assert len(frames) == 4 and all(not r for f in frames for _, r in f[:-1]) and all(f[-1][1] for f in frames)
    assert all(len(p) <= mtu - 3 for p, _ in air.writes) and max(len(f) for f in frames) > 1
    assert all(len(b"".join(p for p, _ in f)) == 8 + int.from_bytes(f[0][0][2:4], "little") for f in frames)


def test_answer_split_over_notifications_at_odd_places(monkeypatch):
    dev, air = make(monkeypatch, notify_sizes=(1, 2, 7, 3, 1, 19, 5))
    png, rows = noisy(16, 8, 1)
    with open_port(bluetooth=NAME) as port:
        display = Display(port)
        hello = display.hello()
        assert fw.bluetooth(hello) == "on" and fw.is_display_firmware(hello)
        assert fw.show_png(display, png, hello)["why"] == "first"
    assert dev.glass == rows


def test_lost_answer_is_asked_again_once(monkeypatch):
    dev, air = make(monkeypatch)
    air.lose_answers = {1: 1}
    with open_port(bluetooth=NAME) as port:
        hello = quick(Display(port)).hello()
    assert hello["bt"] == "on" and [c for c, _ in dev.requests] == [1, 1]
    air.lose_answers, air.lose_requests = {1: 2}, {}  # lost twice: an error, not a loop
    with open_port(bluetooth=NAME) as port, pytest.raises(DisplayError, match="no reply"):
        quick(Display(port)).hello()


def test_lost_request_is_asked_again_once(monkeypatch):
    dev, air = make(monkeypatch)
    air.lose_requests = {0x0B: 1}
    with open_port(bluetooth=NAME) as port:
        display = quick(Display(port))
        fw.clear(display)
    assert dev.refreshes == 1


def test_show_chunk_whose_answer_was_lost_goes_on_at_next(monkeypatch):
    dev, air = make(monkeypatch, 128, 128)
    png, rows = noisy(128, 128, 2)
    assert 4096 < len(png) < dev.max_png
    air.lose_answers = {fw.SHOW_CHUNK: 1}
    with open_port(bluetooth=NAME) as port:
        display = quick(Display(port))
        reply = fw.show_png(display, png, display.hello())
    assert reply["depth"] == 2 and dev.glass == rows
    chunks = [p["off"] for c, p in dev.requests if c == fw.SHOW_CHUNK]
    assert chunks == [0, 0, 4096]  # sent, sent again (answered SEQUENCE next 4096), the rest


def test_a_sequence_refusal_is_not_resumed_over_usb(monkeypatch):
    """Over the cable the old behaviour stays: no resend, a SEQUENCE is an error."""
    from conftest import FakePort
    dev = FakeDevice(128, 128, (1, 2), (1,), True, 20, "vec_gray", "0.0.0+vec")
    first = {"lost": True}

    def respond(wire):
        if wire[5] == fw.SHOW_CHUNK and first.pop("lost", None):
            dev.respond(wire)
            return b""
        return dev.respond(wire)

    png, _ = noisy(128, 128, 2)
    display = quick(Display(FakePort(respond)))
    with pytest.raises(DisplayError, match="no reply"):
        fw.show_png(display, png, dev.hello())
    assert [p["off"] for c, p in dev.requests if c == fw.SHOW_CHUNK] == [0]


def test_unbonded_peer_gets_auth_and_nothing_is_drawn(monkeypatch):
    dev, air = make(monkeypatch, secure=False)
    png, _ = noisy(16, 8, 1)
    with open_port(bluetooth=NAME) as port:
        display = Display(port)
        with pytest.raises(NotPaired, match=NOT_PAIRED):
            display.hello()
        with pytest.raises(NotPaired, match=NOT_PAIRED):
            fw.show_png(display, png, dev.hello())
        with pytest.raises(NotPaired, match=NOT_PAIRED):
            fw.clear(display)
    assert dev.glass is None and dev.refreshes == 0 and dev.requests == []
    assert dev.auth_refused == 3


def test_show_command_over_bluetooth(monkeypatch, tmp_path, capsys):
    dev, air = make(monkeypatch, 64, 64)
    png, rows = noisy(64, 64, 1)
    (tmp_path / "p.png").write_bytes(png)
    assert show.main(["--bluetooth", NAME, str(tmp_path / "p.png")]) == 0
    assert dev.glass == rows
    assert show.main(["--bluetooth", air.address, "--clear"]) == 0 and dev.glass == b"\xff" * 512
    air.secure = False
    assert show.main(["--bluetooth", NAME, "--clear"]) == 1
    assert NOT_PAIRED in capsys.readouterr().err


def test_bt_over_bluetooth_is_cable(monkeypatch):
    dev, air = make(monkeypatch, bt="off")
    with open_port(bluetooth=NAME) as port:
        with pytest.raises(CableOnly, match="USB cable"):
            fw.bt(Display(port), on=True)
    assert dev.bt == "off"


def test_no_room_write_is_sent_again(monkeypatch):
    dev, air = make(monkeypatch)
    air.no_room = 2
    with open_port(bluetooth=NAME) as port:
        assert Display(port).hello()["bt"] == "on"
    assert len(air.writes) == 1


def test_lost_link_is_a_clean_error(monkeypatch):
    dev, air = make(monkeypatch)
    with open_port(bluetooth=NAME) as port:
        display = Display(port)
        display.hello()
        air.client.drop_link()
        with pytest.raises(DisplayError, match="Bluetooth link is gone"):
            display.hello()


def test_find_and_connect_by_address_and_missing_device(monkeypatch):
    dev, air = make(monkeypatch)
    assert ble_port.find(0.1) == [(NAME, air.address)]  # the other device does not advertise the service
    with open_port(bluetooth=air.address.lower()) as port:
        assert Display(port).hello()["bt"] == "on"
    air.advertises = False
    with pytest.raises(DisplayError, match="no display named"):
        ble_port.BlePort(NAME, scan_s=0.1)
    assert not [t for t in __import__("threading").enumerate() if t.name.startswith("Thread") and t.daemon and
                t.is_alive() and "run_forever" in repr(getattr(t, "_target", ""))]


def test_without_bleak_the_error_says_how_to_install_it(monkeypatch):
    monkeypatch.setitem(sys.modules, "bleak", None)
    for call in (lambda: ble_port.BlePort(NAME), lambda: ble_port.find(), lambda: open_port(bluetooth=NAME)):
        with pytest.raises(DisplayError, match=r"pip install tiny-paper-display\[bluetooth\]"):
            call()


def test_vectors_bluetooth_session(monkeypatch):
    """The fake firmware answers every step of the vectors' bluetooth session with the vector's bytes, and the
    host sends the vector's request bytes."""
    b = V["bluetooth"]
    p = b["panel"]
    dev = FakeDevice(p["w"], p["h"], p["depths"], p["partial_depths"], p["partial_all"], p["full_every"], p["board"],
                     p["fw"], bt="off", name=b["name"], clean_every=p.get("clean_every", 15))
    for st in b["steps"]:
        assert codec.build_frame(st["seq"], st["cmd"], plain(st["req"])).hex() == st["tx"], st["name"]
        got = dev.respond(bytes.fromhex(st["tx"]), link=st["link"], secure=st.get("secure", False))
        assert got.hex() == st["rx"], st["name"]
    assert dev.bt == "off" and dev.glass is None
