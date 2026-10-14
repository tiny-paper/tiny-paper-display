# SPDX-License-Identifier: MIT
"""tiny-paper-display setup against the fake firmware: the setting and the key asked or given by flags, the key saved
privately, and never over Bluetooth."""
import argparse
import json
import os
import stat

import pytest

from tiny_paper_display import bt_key, setup_bt
from conftest import FakePort
from fake_ble import Air, install
from fake_display_fw import FakeDevice
from tiny_paper_display.usb_display import Display, DisplayError, open_port

NAME = "tp-display-FAKE"


def flags(**kw):
    return argparse.Namespace(**{"on": False, "off": False, "key": False, "key_file": None, "forget": False, **kw})


def cable(bt="off"):
    dev = FakeDevice(bt=bt, name=NAME)
    return dev, Display(FakePort(dev.respond))


def answers(*lines):
    it = iter(lines)
    return lambda _prompt: next(it)


def test_default_is_off_and_the_cost_is_told(capsys):
    dev, display = cable()
    assert setup_bt.run(display, flags(), answers("")) == 0
    out = capsys.readouterr().out
    assert dev.bt == "off" and "Bluetooth: off (default)" in out and dev.key is None
    assert "three times the battery use" in out and "22 s" in out and "no system pairing" in out
    assert "unproven" in out and [c for c, _ in dev.requests] == [1, 0x0D]


def test_on_makes_a_key_saved_privately(key_folder, capsys):
    dev, display = cable()
    setup_bt.run(display, flags(), answers("on", ""))  # a device without a key: yes by default
    assert dev.bt == "on" and dev.key
    path = key_folder / f"{NAME}.json"
    assert stat.S_IMODE(os.stat(path).st_mode) == 0o600
    assert bt_key.load(NAME)["key"] == dev.key
    out = capsys.readouterr().out
    assert str(path) in out and dev.key.hex() not in out  # the key is never printed
    old = dev.key
    setup_bt.run(display, flags(), answers("on", ""))  # a key is there: no by default, kept
    assert dev.key == old
    setup_bt.run(display, flags(), answers("maybe", "on", "yes"))  # a wrong answer is asked again; a new key
    assert dev.key != old and bt_key.load(NAME)["key"] == dev.key
    setup_bt.run(display, flags(), answers("off"))
    assert dev.bt == "off"


def test_flags_ask_nothing(tmp_path):
    dev, display = cable()
    nothing = answers()  # any question would raise StopIteration
    setup_bt.run(display, flags(key=True, key_file=str(tmp_path / "k.json")), nothing)
    assert dev.bt == "on" and json.loads((tmp_path / "k.json").read_text())["key"] == dev.key.hex()
    setup_bt.run(display, flags(forget=True), nothing)
    assert dev.key is None and dev.bt == "on"
    setup_bt.run(display, flags(off=True), nothing)
    assert dev.bt == "off"


def test_key_and_off_is_refused_before_anything_is_sent():
    dev, display = cable()
    with pytest.raises(DisplayError, match="do not go together"):
        setup_bt.run(display, flags(off=True, key=True), answers())
    assert {c for c, _ in dev.requests} == {1}  # hello only


def test_firmware_without_bluetooth_says_so():
    dev, display = cable(bt=None)
    with pytest.raises(DisplayError, match="no Bluetooth"):
        setup_bt.run(display, flags(on=True), answers())
    assert [c for c, _ in dev.requests] == [1]


def test_refuses_to_run_over_bluetooth(monkeypatch, capsys):
    dev = FakeDevice(bt="on", name=NAME)
    dev.key = bytes(32)
    bt_key.save(NAME, bt_key.kid(dev.key), dev.key)
    air = install(monkeypatch, Air(dev, name=NAME))
    with open_port(bluetooth=NAME) as port:
        writes = len(air.writes)
        with pytest.raises(DisplayError, match="USB cable only"):
            setup_bt.run(Display(port), flags(on=True), answers())
    assert dev.requests == [] and len(air.writes) == writes
    assert setup_bt.main(["--bluetooth", NAME, "--off"]) == 1
    assert "USB cable only" in capsys.readouterr().err and dev.bt == "on"


def test_command_line_over_the_cable(monkeypatch):
    dev = FakeDevice(bt="off", name=NAME)

    class Cable(FakePort):
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return None

    monkeypatch.setattr(setup_bt, "open_port", lambda port, bluetooth=None: Cable(dev.respond))
    assert setup_bt.main(["--port", "/dev/x", "--key"]) == 0
    assert dev.bt == "on" and bt_key.load(NAME)["key"] == dev.key
    with pytest.raises(SystemExit):
        setup_bt.main(["--on", "--off", "--port", "x"])
    with pytest.raises(SystemExit):
        setup_bt.main(["--on"])  # no port
