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
from .usb_display import Display, DisplayError, codec, convert, open_port


def display_firmware(display, hello, png, started, mode=None, passes=None, at=None, force=False):
    """The display firmware (firmware/display): the picture is on the panel when the answer comes. Without a mode
    the device updates only what changed; at: (x, y), the PNG is sent as it is as a rectangle there."""
    print(f"{hello['board']} display firmware {hello['fw']}: {hello['w']}x{hello['h']}, "
          f"gray depths {hello['depths']}{', colors ' + ' '.join(hello['colors']) if hello.get('colors') else ''}"
          f", partial at {hello.get('partial_depths', [])} bit, modes {' '.join(hello.get('modes', ['full']))}")
    if png is None:
        reply = display_fw.clear(display)
    elif at:
        reply = display_fw.show_area(display, png, hello, at[0], at[1], mode, passes, force=force)
    else:
        reply = display_fw.show_png(display, display_fw.panel_png(png, hello), hello, mode=mode, passes=passes, force=force)
    print(json.dumps(reply, sort_keys=True))
    print(f"Panel: {display_fw.describe(reply)}; total {time.monotonic() - started:.3f}s")
    if "warn" in reply:
        print(f"Warning from the device: {reply['warn']}", file=sys.stderr)
    if display_fw.gate_clock_wrong(reply):
        print("The device counted a wrong gate clock: the picture is not where it should be", file=sys.stderr)
        return 1
    return 0


def show_on_display_firmware(name, png, started, bluetooth=None):
    with open_port(name, bluetooth) as port:
        display = Display(port)
        hello = display.hello()
        if not display_fw.is_display_firmware(hello):
            raise DisplayError("--clear needs the display firmware (hello has no project: display)")
        return display_firmware(display, hello, png, started)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?", help="/dev/ttyACM0, COM5, or socket://127.0.0.1:7701 (sim only); "
                                                "not with --bluetooth")
    parser.add_argument("picture", type=Path, nargs="?")
    parser.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS",
                        help="display firmware only: over Bluetooth to the device with this name (tp-display-XXXX) or "
                             "address instead of a port; needs pip install tiny-paper-display[bluetooth] and a pairing "
                             "made with tiny-paper-display setup")
    parser.add_argument("--clear", action="store_true", help="display firmware only: a white panel instead of a picture")
    parser.add_argument("--force", action="store_true", help="upload even when the picture is unchanged")
    parser.add_argument("--mode", choices=display_fw.MODES,
                        help="display firmware only: the refresh to ask for (default: the device updates only what "
                             "changed and does a full refresh when it has to)")
    parser.add_argument("--passes", type=int, help="with --mode passes: how many drives (1..4)")
    parser.add_argument("--at", metavar="X,Y", help="display firmware only: the PNG is a rectangle with its top left "
                                                    "pixel there (x and its width multiples of 8), sent as it is")
    args = parser.parse_args(argv)
    if args.bluetooth and args.picture is None and args.port is not None:
        args.port, args.picture = None, Path(args.port)  # the one positional is the picture
    if bool(args.bluetooth) == bool(args.port):
        parser.error("a port, or --bluetooth NAME_OR_ADDRESS")
    if not args.clear and args.picture is None:
        parser.error("a picture, or --clear")
    try:
        at = tuple(int(v) for v in args.at.split(",")) if args.at else None
    except ValueError:
        at = ()
    if at is not None and len(at) != 2:
        parser.error("--at X,Y")
    started = time.monotonic()
    try:
        if args.clear:
            return show_on_display_firmware(args.port, None, started, args.bluetooth)
        if args.picture.stat().st_size > 16 * 1024 * 1024:
            raise DisplayError("PNG exceeds 16 MiB")
        with args.picture.open("rb") as image:
            png = image.read(16 * 1024 * 1024 + 1)
        if len(png) > 16 * 1024 * 1024:
            raise DisplayError("PNG exceeds 16 MiB")
        with open_port(args.port, args.bluetooth) as port:
            display = Display(port)
            hello = display.hello()
            if display_fw.is_display_firmware(hello):
                return display_firmware(display, hello, png, started, args.mode, args.passes, at, args.force)
            if args.mode or at:
                raise DisplayError("--mode and --at need the display firmware (hello has no project: display)")
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
