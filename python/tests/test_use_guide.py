# SPDX-License-Identifier: MIT
"""Every code block of docs/use.md marked `python test` or `sh test` is run against the fake display firmware, in
the order of the page, in one empty directory that holds picture.png. A sample that no longer works fails here.
Blocks without `test` (install, Bluetooth send) need a computer, a device or the network and are not run."""
import re
import shlex
from pathlib import Path

import pytest

from conftest import FakePort
from fake_display_fw import FakeDevice
from tiny_paper_display import cli
from tiny_paper_display.usb_display import ROOT, codec

GUIDE = ROOT / "docs/use.md"
BLOCK = re.compile(r"^```(\w+) test\n(.*?)^```$", re.S | re.M)


class Port(FakePort):
    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


def blocks():
    return BLOCK.findall(GUIDE.read_text())


def test_the_guide_has_samples_to_run():
    kinds = [kind for kind, _ in blocks()]
    assert kinds.count("python") >= 4 and kinds.count("sh") >= 5, kinds


def test_guide_samples_run(tmp_path, monkeypatch, capsys):
    device = FakeDevice(bt="off")
    monkeypatch.setattr(codec, "open_serial", lambda name, ui=None: Port(device.respond))
    (tmp_path / "picture.png").write_bytes((ROOT / "python/examples/bars.png").read_bytes())
    monkeypatch.chdir(tmp_path)
    for kind, code in blocks():
        if kind == "python":
            exec(compile(code, "docs/use.md", "exec"), {"__name__": "__guide__"})
        else:
            for line in code.strip().splitlines():
                words = shlex.split(line)
                assert words[0] == "tiny-paper-display", line
                assert cli.main(words[1:]) == 0, line
    assert device.refreshes >= 5
    assert "bluetooth" not in capsys.readouterr().err.lower()
