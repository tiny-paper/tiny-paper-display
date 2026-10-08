"""The TRMNL X display firmware answers hello with gray partial depths: its gray partial switch is on by default.
The driver is ESP-only and has no host test, so this pins the default in the source (the Sketch page and the docs
rely on it)."""
import re
from pathlib import Path

PANEL = Path(__file__).resolve().parents[2] / "firmware/components/hal_esp/esp/panel_par.c"


def test_x_gray_partial_is_on_by_default():
    m = re.search(r"static gpart_cfg_t s_gp = \{(\d+), (\d+), (\d+)\}", PANEL.read_text())
    assert m and m.group(1) == "1"


def test_x_gray_pre_push_defaults_to_none():
    """Measured on a device: light frames do nothing for pixels that start white and cost about 0.7 s an update; the
    knob table and the spec say the same default."""
    src = PANEL.read_text()
    m = re.search(r"static gpart_cfg_t s_gp = \{(\d+), (\d+), (\d+)\}", src)
    assert m and (m.group(2), m.group(3)) == ("0", "0")
    assert '{{"gray_light", 0, 14, 0, NULL}, &s_gp.light}' in src
    spec = (PANEL.parents[4] / "docs/spec/usb-display.md").read_text()
    assert re.search(r"\| `gray_light` \| 0 to 14 \| 0 \|", spec)
