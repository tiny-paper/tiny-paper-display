"""The TRMNL X display firmware answers hello with gray partial depths: its gray partial switch is on by default.
The driver is ESP-only and has no host test, so this pins the default in the source (the Sketch page and the docs
rely on it)."""
import re
from pathlib import Path

PANEL = Path(__file__).resolve().parents[2] / "firmware/components/hal_esp/esp/panel_par.c"


def test_x_gray_partial_is_on_by_default():
    m = re.search(r"static gpart_cfg_t s_gp = \{(\d+),", PANEL.read_text())
    assert m and m.group(1) == "1"
