# SPDX-License-Identifier: MIT
"""Show pictures on a tiny-paper display over its USB cable (docs/spec/usb-display.md).

The simple API: `devices()`, `open()`, `open_all()`, `show()`, `Canvas`; see api.py. The parts below it are the modules
display_fw, usb_display, ble_port and picture."""
from .api import Device, Found, Group, devices, open, open_all, show  # noqa: A004
from .canvas import Canvas
from .usb_display import DisplayError

__all__ = ["Canvas", "Device", "DisplayError", "Found", "Group", "devices", "open", "open_all", "show"]
