#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Turn the display's Bluetooth on or off and open its pairing window, over the USB cable.
"""tiny-paper-display setup: the Bluetooth setting of a display firmware (docs/spec/usb-display.md "Bluetooth").

Over the cable it shows the setting, says what on costs and how pairing works, asks, and applies. For unattended
use: --on, --off, --pair, --forget (no questions). It runs over the cable only: the device refuses it over Bluetooth.

  setup_bt.py --port /dev/ttyACM0
  setup_bt.py --port /dev/ttyACM0 --on --pair
"""
import argparse
import sys

from . import display_fw
from .usb_display import Display, DisplayError, codec, open_port

COST = """Bluetooth on: the device also takes pictures over Bluetooth Low Energy.
  Cost: about three times the battery use on battery (an unmeasured estimate), and it is found only within about 22 s
        (it advertises for 2 s in every 20 s). On the cable there is no extra cost.
  Pairing: a pairing window of 120 s opens when you press the device's button or answer yes below. The panel shows a
        6 digit passkey; type it on the computer when it asks. Only a paired computer can use Bluetooth.
  Radio behaviour on real hardware is unproven until a person has tried it."""


def describe(state):
    out = f"Bluetooth is {'on' if state['on'] else 'off'}, the device is called {state['name']}"
    if state["pairing"]:
        out += f"; the pairing window is open for {state['pairing']} s: the panel shows the passkey, type it on the computer"
    return out


def ask(prompt, answers, default, input_fn):
    while True:
        text = input_fn(f"{prompt} [{default}]: ").strip().lower() or default
        if text in answers:
            return text
        print(f"Please answer {' or '.join(answers)}.")


def run(display, args, input_fn=input):
    """Apply the flags, or ask. Returns 0; DisplayError when it cannot."""
    if display.ble:
        raise DisplayError("setup runs over the USB cable only: the device refuses the Bluetooth setting over Bluetooth")
    hello = display.hello()
    if not display_fw.is_display_firmware(hello):
        raise DisplayError("this device does not run the display firmware (hello has no project: display)")
    now = display_fw.bluetooth(hello)
    if now is None:
        raise DisplayError("this display firmware has no Bluetooth: update it")
    unattended = args.on or args.off or args.pair or args.forget
    print(f"Bluetooth: {now}{' (default)' if now == 'off' else ''}")
    on, pair = True if args.on else False if args.off else None, True if args.pair else None
    if args.pair and now == "off" and not args.on:
        raise DisplayError("--pair needs Bluetooth on: add --on")
    if not unattended:
        print(COST)
        on = ask("Bluetooth: off (default) / on", ("off", "on"), now, input_fn) == "on"
        if on and ask("Open the pairing window now (120 s)? yes / no", ("yes", "no"), "no", input_fn) == "yes":
            pair = True
    if pair and on is False:
        raise DisplayError("--pair and --off do not go together")
    state = display_fw.bt(display, on=on, pair=pair, forget=True if args.forget else None)
    print(describe(state))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="/dev/ttyACM0, COM5 (the USB cable)")
    p.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS", help="refused: setup runs over the cable only")
    p.add_argument("--on", action="store_true", help="Bluetooth on, without asking")
    p.add_argument("--off", action="store_true", help="Bluetooth off, without asking")
    p.add_argument("--pair", action="store_true", help="open the pairing window (120 s; needs on)")
    p.add_argument("--forget", action="store_true", help="delete every pairing, drop the connection")
    args = p.parse_args(argv)
    if args.on and args.off:
        p.error("--on or --off")
    try:
        if args.bluetooth:
            raise DisplayError("setup runs over the USB cable only: give --port, not --bluetooth")
        if not args.port:
            p.error("--port PORT (the USB cable)")
        with open_port(args.port) as port:
            return run(Display(port), args)
    except (DisplayError, OSError, ValueError, codec.PortError) as exc:
        print(f"setup: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
