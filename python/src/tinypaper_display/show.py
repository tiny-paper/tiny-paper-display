#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Show a host PNG through the current USB album upload path.
"""Upload a PNG to the USB album. A Photos album consumer must already be active."""
import argparse
import json
import sys
import time
from pathlib import Path

from . import display_fw
from .usb_display import Display, DisplayError, codec, convert


def display_firmware(display, hello, png, started):
    """The display firmware (firmware/display): the picture is on the panel when the answer comes."""
    print(f"{hello['board']} display firmware {hello['fw']}: {hello['w']}x{hello['h']}, "
          f"gray depths {hello['depths']}{', colors ' + ' '.join(hello['colors']) if hello.get('colors') else ''}")
    reply = display_fw.clear(display) if png is None else display_fw.show_png(display, display_fw.panel_png(png, hello), hello)
    print(json.dumps(reply, sort_keys=True))
    print(f"Panel refreshed in {reply.get('ms')} ms; total {time.monotonic() - started:.3f}s")
    if "warn" in reply:
        print(f"Warning from the device: {reply['warn']}", file=sys.stderr)
    return 0


def show_on_display_firmware(name, png, started):
    with codec.open_serial(name) as port:
        port.timeout, port.write_timeout = 0.05, 2
        display = Display(port)
        hello = display.hello()
        if not display_fw.is_display_firmware(hello):
            raise DisplayError("--clear needs the display firmware (hello has no project: display)")
        return display_firmware(display, hello, png, started)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="/dev/ttyACM0, COM5, or socket://127.0.0.1:7701 (sim only)")
    parser.add_argument("picture", type=Path, nargs="?")
    parser.add_argument("--clear", action="store_true", help="display firmware only: a white panel instead of a picture")
    args = parser.parse_args()
    if not args.clear and args.picture is None:
        parser.error("a picture, or --clear")
    started = time.monotonic()
    try:
        if args.clear:
            return show_on_display_firmware(args.port, None, started)
        if args.picture.stat().st_size > 16 * 1024 * 1024:
            raise DisplayError("PNG exceeds 16 MiB")
        with args.picture.open("rb") as image:
            png = image.read(16 * 1024 * 1024 + 1)
        if len(png) > 16 * 1024 * 1024:
            raise DisplayError("PNG exceeds 16 MiB")
        with codec.open_serial(args.port) as port:
            port.timeout, port.write_timeout = 0.05, 2
            display = Display(port)
            hello = display.hello()
            if display_fw.is_display_firmware(hello):
                return display_firmware(display, hello, png, started)
            hello, info = display.screen(hello)
            print(f"{hello['board']}: {info['w']}x{info['h']}, {info['depth']}-bit {info['palette']}")
            raster = convert(png, info)
            transfer = time.monotonic()
            reply = display.upload(raster, info)
            print(json.dumps(reply, sort_keys=True))
            print(f"Stored {len(raster)} bytes in {time.monotonic() - transfer:.3f}s; "
                  f"total {time.monotonic() - started:.3f}s")
            print("An active Photos album consumer picks next; the response does not confirm a panel refresh.")
        return 0
    except (DisplayError, OSError, ValueError, codec.PortError) as exc:
        print(f"show: {exc} ({time.monotonic() - started:.3f}s)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
