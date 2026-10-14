# SPDX-License-Identifier: MIT
"""The simple API (tiny_paper_display.api) against the fake display firmware: finding, opening, closing by itself,
the area rule, several devices, the connection's defaults."""

import pytest

import tiny_paper_display as tpd
from conftest import FakePort, png
from fake_display_fw import FakeDevice
from tiny_paper_display import api, usb_display


class Port(FakePort):
    def __init__(self, respond):
        super().__init__(respond)
        self.closed = 0

    def close(self):
        self.closed += 1


@pytest.fixture
def bench(monkeypatch):
    """Ports by name -> FakeDevice (None: silent); every opened Port is kept in .opened."""
    class Bench:
        devices, opened = {}, []

    def open_serial(name, ui=None):
        dev = Bench.devices[name]
        port = Port(dev.respond if dev else (lambda data: b""))
        Bench.opened.append((name, port))
        return port

    monkeypatch.setattr(usb_display.codec, "open_serial", open_serial)
    monkeypatch.setattr(api, "list_ports", lambda: [(n, None) for n in Bench.devices])
    monkeypatch.setattr(api, "PROBE_TIMEOUT", 0.01)
    monkeypatch.setattr(usb_display.codec.LinkClient, "request", _fast(usb_display.codec.LinkClient.request))
    return Bench


def _fast(real):
    def request(self, cmd, data, timeout, retries=0):
        return real(self, cmd, data, 0.01, 0)
    return request


def test_devices_lists_only_the_display_firmware(bench):
    album = FakeDevice(hello={"proto": 1, "album": 1, "fw": "x", "board": "album", "w": 8, "h": 8, "max_payload": 6144})
    bench.devices = {"A": FakeDevice(board="one"), "B": album, "C": None, "D": FakeDevice(w=24, h=16, board="two")}
    found = tpd.devices()
    assert [(f.port, f.board, f.w, f.h) for f in found] == [("A", "one", 16, 8), ("D", "two", 24, 16)]
    assert all(p.closed >= 1 for n, p in bench.opened), "every probed port is closed again"


def test_open_takes_the_first_display_and_skips_a_port_that_is_not(bench):
    bench.devices = {"A": FakeDevice(hello={"proto": 1, "album": 1, "board": "album", "w": 8, "h": 8}), "B": FakeDevice(board="two")}
    with tpd.open() as d:
        assert (d.name, d.board, d.caps.w) == ("B", "two", 16)
    with pytest.raises(tpd.DisplayError, match="no device"):
        bench.devices = {"A": None}
        tpd.open()


def test_the_connection_closes_after_an_exception(bench):
    bench.devices = {"A": FakeDevice()}
    with pytest.raises(RuntimeError):
        with tpd.open("A") as d:
            port = d.port
            raise RuntimeError("boom")
    assert port.closed == 1 and d.port is None
    with pytest.raises(tpd.DisplayError, match="closed"):
        d.clear()


def test_the_connection_closes_when_the_device_refuses(bench):
    bench.devices = {"A": FakeDevice()}
    with pytest.raises(tpd.DisplayError):
        with tpd.open("A") as d:
            d.show(tpd.Canvas(16, 8), depth=4)  # the fake has no 4 bit
    assert d.port is None


def test_open_a_port_that_is_not_the_display_firmware_closes_it(bench):
    bench.devices = {"A": FakeDevice(hello={"proto": 1, "album": 1, "board": "album", "w": 8, "h": 8})}
    with pytest.raises(tpd.DisplayError, match="display firmware"):
        tpd.open("A")
    assert bench.opened[-1][1].closed == 1


def test_area_refuses_x_not_a_multiple_of_8_before_sending(bench):
    dev = bench.devices["A"] = FakeDevice(w=32, h=16)
    with tpd.open("A") as d:
        before = len(dev.requests)
        for x, w in ((4, 8), (8, 12)):
            with pytest.raises(tpd.DisplayError, match="multiples of 8"):
                d.area(tpd.Canvas(w, 4), x, 0)
        with pytest.raises(tpd.DisplayError, match="inside"):
            d.area(tpd.Canvas(16, 4), 24, 0)
        assert len(dev.requests) == before, "nothing was sent"
        d.show(tpd.Canvas(32, 16), depth=1)
        c = tpd.Canvas(8, 4, 0)
        assert d.area(c, 8, 4)["mode"] in ("partial", "none")


def test_area_takes_a_png_and_a_picture_at_its_own_size(bench):
    bench.devices = {"A": FakeDevice(w=32, h=16)}
    rgb = png(8, 4, b"".join(b"\x00" + bytes([200, 30, 30]) * 8 for _ in range(4)))
    with tpd.open("A") as d:
        d.show(tpd.Canvas(32, 16))
        assert d.area(rgb, 8, 4, mode="full")["mode"] == "full"


def test_show_a_picture_a_path_bytes_and_the_defaults(bench, tmp_path):
    dev = bench.devices["A"] = FakeDevice(w=16, h=8)
    rows = b"".join(b"\x00" + bytes(v for _ in range(8) for v in (x * 16, 80, 160)) for x in range(8))
    picture_png = png(8, 8, rows)
    path = tmp_path / "p.png"
    path.write_bytes(picture_png)
    with tpd.open("A", dither="none", border="black") as d:
        assert d.defaults["border"] == "black"
        a = d.show(path)
        assert a["depth"] == 2 and a["w"] == 16, "the deepest depth the board has"
        glass_black = dev.pixels()
        assert glass_black[0] == 0, "the black border"
        d.show(picture_png, border="white", depth=1)
        assert dev.glass_depth == 1 and dev.pixels()[0] == 1, "per call beats the connection's default"
        with pytest.raises(tpd.DisplayError, match="unknown picture option"):
            d.show(path, colour=1)
    with pytest.raises(tpd.DisplayError, match="unknown picture default"):
        tpd.open("A", dithering="none")


def test_a_picture_that_is_not_a_png_needs_pillow_or_says_so(bench):
    bench.devices = {"A": FakeDevice()}
    with tpd.open("A") as d:
        with pytest.raises(tpd.DisplayError, match="Pillow|cannot read"):
            d.show(b"\xff\xd8\xff\xe0not really a jpeg")


def test_configure_is_stored_on_the_device_and_none_clears(bench):
    dev = bench.devices["A"] = FakeDevice()
    with tpd.open("A") as d:
        assert d.caps.full_every == 30
        caps = d.configure(full_every=50)
        assert (caps.full_every, caps.full_every_set, dev.full_every) == (50, True, 50) and d.caps is caps
        assert d.configure().full_every == 50, "no argument leaves it"
        caps = d.configure(full_every=None)
        assert (caps.full_every, caps.full_every_set) == (30, False)


def test_open_all_and_a_group_of_two_fake_devices(bench):
    one, two = FakeDevice(board="one"), FakeDevice(board="two", w=24, h=16)
    bench.devices = {"A": one, "B": two}
    with tpd.open_all() as group:
        assert [d.board for d in group] == ["one", "two"]
        results = group.show(tpd.Canvas(16, 8, 0))
        assert len(results) == 2 and one.refreshes == 1
        assert two.requests and group.devices[1].w == 24
        ports = [d.port for d in group]
    assert all(p.closed == 1 for p in ports)


def test_open_all_closes_the_ones_it_opened_when_none_is_found(bench):
    bench.devices = {"A": None}
    with pytest.raises(tpd.DisplayError, match="no device"):
        tpd.open_all()


def test_a_group_closes_every_device_even_when_one_close_fails(bench):
    bench.devices = {"A": FakeDevice(), "B": FakeDevice()}
    group = tpd.open_all()
    first, second = group.devices[0].port, group.devices[1].port
    first.close = lambda: (_ for _ in ()).throw(OSError("gone"))
    with pytest.raises(OSError):
        group.close()
    assert second.closed == 1


def test_module_show_opens_shows_closes(bench):
    bench.devices = {"A": FakeDevice()}
    reply = tpd.show(tpd.Canvas(16, 8, 0))
    assert reply["mode"] == "full" and bench.opened[-1][1].closed == 1


def test_canvas_png_is_what_the_old_example_made():
    c = tpd.Canvas(8, 2)
    c.rect(0, 0, 4, 1, 0)
    from tiny_paper_display import display_fw
    assert c.png(1) == display_fw.write_png(8, 2, 1, bytes([0x0F, 0xFF]))
    assert c.crop(4, 0, 4, 2).px == bytearray([15] * 8)
    with pytest.raises(tpd.DisplayError):
        c.crop(6, 0, 4, 2)


def _example(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, usb_display.ROOT / "python/examples" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_the_test_page_example_runs_on_the_api(bench, monkeypatch, capsys):
    dev = bench.devices["A"] = FakeDevice(w=800, h=480)
    monkeypatch.setattr("sys.argv", ["test_page.py", "--depth", "1"])
    _example("test_page").main()
    out = capsys.readouterr().out
    assert "| 1 | PARTIAL |" in out and "| 1 | SUMMARY |" in out and "refused" not in out
    assert dev.requests, "it talked to the device"


def test_the_quickstart_example_runs(bench, monkeypatch, tmp_path):
    dev = bench.devices["A"] = FakeDevice(w=64, h=48)
    pic = tmp_path / "p.png"
    pic.write_bytes(tpd.Canvas(64, 48, 8).png(2))
    monkeypatch.setattr("sys.argv", ["quickstart.py", str(pic)])
    _example("quickstart")
    assert dev.glass_depth and bench.opened[-1][1].closed == 1


# ---------------------------------------------------------------------------------------------- failures and logging
import logging  # noqa: E402

from tiny_paper_display import display_fw  # noqa: E402


@pytest.fixture(autouse=True)
def _quiet_logging():
    yield
    tpd.log(None)
    logging.getLogger("tiny_paper_display").setLevel(logging.NOTSET)


def cmds(dev):
    return [c for c, _ in dev.requests]


def test_the_error_classes_are_display_errors():
    for cls in (tpd.Refused, tpd.Busy, tpd.Disconnected, tpd.PanelFault):
        assert issubclass(cls, tpd.DisplayError)
    assert issubclass(tpd.Busy, tpd.Refused)


def test_refused_carries_the_device_reason_and_code(bench):
    bench.devices = {"A": FakeDevice(w=16, h=8)}
    with tpd.open("A") as d:
        with pytest.raises(tpd.Refused) as e:
            d.show(tpd.Canvas(16, 8), depth=2, mode="partial")
        assert (e.value.err, e.value.name) == (36, "MODE") and "MODE 36" in str(e.value)
        assert d.show(tpd.Canvas(16, 8))["mode"], "the object is usable after a refusal"


def test_busy_longer_than_the_wait(bench, monkeypatch):
    dev = bench.devices["A"] = FakeDevice(w=16, h=8)
    real = dev.handle

    def busy(cmd, p):
        if cmd == display_fw.SHOW_BEGIN:
            raise __import__("fake_display_fw").Refuse(3, "refreshing", retry_ms=1)
        return real(cmd, p)

    dev.handle = busy
    monkeypatch.setattr(display_fw.request, "__defaults__", (None, 3, 0))
    with tpd.open("A") as d:
        with pytest.raises(tpd.Busy) as e:
            d.show(tpd.Canvas(16, 8))
        assert e.value.err == 3
        dev.handle = real
        assert d.show(tpd.Canvas(16, 8))["mode"]


def cut_chunks(dev, times):
    """The device stops answering the first `times` show_chunk requests (a cable pulled mid transfer)."""
    real = dev.respond
    state = {"left": times}

    def respond(wire, **kw):
        if wire[5] == display_fw.SHOW_CHUNK and state["left"] > 0:
            state["left"] -= 1
            return b""
        return real(wire, **kw)

    dev.respond = respond
    return state


def test_a_cut_transfer_is_tried_again_once_on_a_new_link(bench):
    dev = bench.devices["A"] = FakeDevice(w=16, h=8)
    state = cut_chunks(dev, 1)
    with tpd.open("A") as d:
        first = d.port
        reply = d.show(tpd.Canvas(16, 8, 0))
        assert reply["mode"] == "full" and state["left"] == 0
        assert first.closed == 1 and d.port is not first, "a new link"
    assert cmds(dev).count(display_fw.SHOW_BEGIN) == 2


def test_a_transfer_cut_twice_is_disconnected_and_the_object_stays_usable(bench, caplog):
    dev = bench.devices["A"] = FakeDevice(w=16, h=8)
    cut_chunks(dev, 2)
    with caplog.at_level(logging.WARNING, logger="tiny_paper_display"):
        with tpd.open("A") as d:
            with pytest.raises(tpd.Disconnected, match="cut twice"):
                d.show(tpd.Canvas(16, 8, 0))
            assert cmds(dev).count(display_fw.SHOW_BEGIN) == 2, "one retry, not more"
            assert "transfer cut" in caplog.text, "the retry is a warning"
            before = len(dev.requests)
            assert d.show(tpd.Canvas(16, 8, 0))["mode"] == "full"
            assert display_fw.HAVE not in cmds(dev)[before:], "the whole picture, no 'unchanged' shortcut"


def test_a_gone_port_is_disconnected(bench, monkeypatch):
    bench.devices = {"A": FakeDevice(w=16, h=8)}
    with tpd.open("A") as d:
        d.port.write = lambda data: (_ for _ in ()).throw(OSError("gone"))
        monkeypatch.setattr(usb_display.codec, "open_serial", lambda n, ui=None: (_ for _ in ()).throw(OSError("unplugged")))
        with pytest.raises(tpd.Disconnected):
            d.show(tpd.Canvas(16, 8))


def test_a_wrong_gate_clock_raises_panel_fault(bench):
    dev = bench.devices["A"] = FakeDevice(w=16, h=8, ckv={"per": 3, "rows": 8, "ok": False})
    with tpd.open("A") as d:
        with pytest.raises(tpd.PanelFault) as e:
            d.show(tpd.Canvas(16, 8, 0))
        assert e.value.reply["ckv"]["ok"] is False
        dev.ckv = {"per": 3, "rows": 8, "ok": True}
        before = len(dev.requests)
        assert d.show(tpd.Canvas(16, 8, 0))["mode"]
        assert display_fw.HAVE not in cmds(dev)[before:] and display_fw.SHOW_CHUNK in cmds(dev)[before:], "sent whole again"


def test_a_group_goes_on_when_one_device_fails(bench):
    good, bad = FakeDevice(board="good"), FakeDevice(board="bad", ckv={"per": 1, "rows": 8, "ok": False})
    bench.devices = {"A": bad, "B": good}
    with tpd.open_all() as group:
        results = group.show(tpd.Canvas(16, 8, 0))
        assert isinstance(results[0], tpd.PanelFault) and results[1]["mode"] == "full"
        assert good.refreshes == 1
        assert len(group.clear()) == 2


def test_logging_is_silent_by_default(bench, capsys, caplog):
    import io
    bench.devices = {"A": FakeDevice(w=16, h=8)}
    assert any(isinstance(h, logging.NullHandler) for h in logging.getLogger("tiny_paper_display").handlers)
    with tpd.open("A") as d:
        d.show(tpd.Canvas(16, 8, 0))
    out = capsys.readouterr()
    assert out.err == "" and out.out == ""


def test_info_logging_has_the_times_of_a_show(bench, capsys):
    bench.devices = {"A": FakeDevice(w=16, h=8)}
    with tpd.open("A", log="info") as d:
        reply = d.show(tpd.Canvas(16, 8, 0))
    err = capsys.readouterr().err
    assert "host prepare" in err and "transfer" in err and "full (depth)" in err
    assert "prepare_ms" in reply and "transfer_ms" in reply
    assert "host: prepare" in display_fw.describe(reply)
    assert "request 0x" not in err, "debug lines are not info"


def test_info_logging_has_the_device_steps(bench, capsys):
    """The device's ms for each step of a show (recv, prep, inflate, ...) are in the info line and in describe()."""
    dev = bench.devices["A"] = FakeDevice(w=16, h=8)
    dev.steps = {"recv": 11, "prep": 2, "inflate": 31, "convert": 17, "diff": 3, "fingerprint": 5, "panel": 70, "total": 150}
    with tpd.open("A", log="info") as d:
        reply = d.show(tpd.Canvas(16, 8, 0))
    err = capsys.readouterr().err
    assert "steps recv 11, prep 2, inflate 31, convert 17, diff 3, fingerprint 5, panel 70, total 150" in err
    assert "steps recv 11" in display_fw.describe(reply)
    assert "steps" not in display_fw.describe({"mode": "full", "why": "asked", "depth": 1, "ms": 0}), "an old firmware has none"


def test_debug_logging_shows_requests_without_picture_bytes(bench, capsys):
    bench.devices = {"A": FakeDevice(w=16, h=8)}
    with tpd.open("A", log="debug") as d:
        d.show(tpd.Canvas(16, 8, 0))
    err = capsys.readouterr().err
    assert "request 0x09" in err and "answer 0x09" in err and "<" in err and "bytes>" in err and "\\x89PNG" not in err
    tpd.log(None)
    with pytest.raises(tpd.DisplayError):
        tpd.log("loud")
