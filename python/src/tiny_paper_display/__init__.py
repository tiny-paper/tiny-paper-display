# SPDX-License-Identifier: MIT
"""Show pictures on a tiny-paper display over its USB cable (docs/spec/usb-display.md).

The simple API: `devices()`, `open()`, `open_all()`, `show()`, `Canvas`; see api.py. The parts below it are the modules
display_fw, usb_display, ble_port and picture."""
import logging

from .api import Device, Found, Group, devices, open, open_all, set_log as log, show  # noqa: A004
from .display_fw import Busy, PanelFault, Refused
from .usb_display import Disconnected
from .canvas import Canvas
from .usb_display import DisplayError

logging.getLogger(__name__).addHandler(logging.NullHandler())  # silent unless the program or log() asks

__all__ = ["Busy", "Canvas", "Device", "Disconnected", "DisplayError", "Found", "Group", "PanelFault", "Refused", "devices", "log",
           "open", "open_all", "show"]
