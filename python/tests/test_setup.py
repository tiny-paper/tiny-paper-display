# SPDX-License-Identifier: MIT
"""tiny-paper-display setup against the fake firmware: the setting asked or given by flags, and never over Bluetooth."""
import argparse

import pytest

from tiny_paper_display import setup_bt
from conftest import FakePort
from fake_ble import Air, install
from fake_display_fw import FakeDevice
from tiny_paper_display.usb_display import Display, DisplayError, open_port


def flags(**kw):
    return argparse.Namespace(**{"on": False, "off": False, "pair": False, "forget": False, **kw})


def cable(bt="off"):
    dev = FakeDevice(bt=bt, name="tp-display-FAKE")
    return dev, Display(FakePort(dev.respond))


def answers(*lines):
    it = iter(lines)
    return lambda _prompt: next(it)


def test_default_is_off_and_the_cost_is_told(capsys):
    dev, display = cable()
    assert setup_bt.run(display, flags(), answers("")) == 0
    out = capsys.readouterr().out
    assert dev.bt == "off" and "Bluetooth: off (default)" in out
    assert "three times the battery use" in out and "22 s" in out and "120 s" in out and "passkey" in out
    assert "unproven" in out and [c for c, _ in dev.requests] == [1, 0x0D]


def test_on_without_pairing_and_with_pairing():
    dev, display = cable()
    setup_bt.run(display, flags(), answers("on", "no"))
    assert dev.bt == "on" and dev.pairing == 0
    setup_bt.run(display, flags(), answers("on", ""))  # the window opens only when asked for
    assert dev.pairing == 0
    setup_bt.run(display, flags(), answers("on", "yes"))
    assert dev.bt == "on" and dev.pairing == 120
    setup_bt.run(display, flags(), answers("maybe", "off"))  # a wrong answer is asked again
    assert dev.bt == "off" and dev.pairing == 0


def test_flags_ask_nothing(capsys):
    dev, display = cable()
    nothing = answers()  # any question would raise StopIteration
    setup_bt.run(display, flags(on=True, pair=True), nothing)
    assert dev.bt == "on" and dev.pairing == 120 and "pairing window is open for 120 s" in capsys.readouterr().out
    dev.bonded = True
    setup_bt.run(display, flags(forget=True), nothing)
    assert dev.bonded is False and dev.bt == "on" and dev.pairing == 0
    setup_bt.run(display, flags(off=True), nothing)
    assert dev.bt == "off"


def test_pair_while_off_is_refused_before_anything_is_sent():
    dev, display = cable()
    with pytest.raises(DisplayError, match="--pair needs Bluetooth on"):
        setup_bt.run(display, flags(pair=True), answers())
    dev.bt = "on"
    with pytest.raises(DisplayError, match="do not go together"):
        setup_bt.run(display, flags(off=True, pair=True), answers())
    assert {c for c, _ in dev.requests} == {1}  # hello only


def test_firmware_without_bluetooth_says_so():
    dev, display = cable(bt=None)
    with pytest.raises(DisplayError, match="no Bluetooth"):
        setup_bt.run(display, flags(on=True), answers())
    assert [c for c, _ in dev.requests] == [1]


def test_refuses_to_run_over_bluetooth(monkeypatch, capsys):
    dev = FakeDevice(bt="on", name="tp-display-FAKE")
    air = install(monkeypatch, Air(dev, name="tp-display-FAKE"))
    with open_port(bluetooth="tp-display-FAKE") as port:
        with pytest.raises(DisplayError, match="USB cable only"):
            setup_bt.run(Display(port), flags(on=True), answers())
    assert dev.requests == [] and air.writes == []
    assert setup_bt.main(["--bluetooth", "tp-display-FAKE", "--off"]) == 1
    assert "USB cable only" in capsys.readouterr().err and dev.bt == "on"


def test_command_line_over_the_cable(monkeypatch):
    dev = FakeDevice(bt="off", name="tp-display-FAKE")

    class Cable(FakePort):
        def __enter__(self):
            return self

        def __exit__(self, *_):
            return None

    monkeypatch.setattr(setup_bt, "open_port", lambda port, bluetooth=None: Cable(dev.respond))
    assert setup_bt.main(["--port", "/dev/x", "--on", "--pair"]) == 0
    assert dev.bt == "on" and dev.pairing == 120
    with pytest.raises(SystemExit):
        setup_bt.main(["--on", "--off", "--port", "x"])
    with pytest.raises(SystemExit):
        setup_bt.main(["--on"])  # no port
