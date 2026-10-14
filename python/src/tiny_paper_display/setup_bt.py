#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Turn the display's Bluetooth on or off and make its key, over the USB cable.
"""tiny-paper-display setup: the Bluetooth setting and key of a display firmware (docs/spec/usb-display.md "Bluetooth").

Over the cable it shows the setting, says what on costs, asks, and applies. With a key made here a computer uses
Bluetooth without any system pairing: the key is saved to ~/.config/tiny-paper-display/keys/<name>.json (mode 0600);
copy that file to give another computer (or a container: key_file=) the device. For unattended use: --on, --off,
--key, --forget (no questions). It runs over the cable only: the device refuses it over Bluetooth.

  setup_bt.py --port /dev/ttyACM0
  setup_bt.py --port /dev/ttyACM0 --key                  # Bluetooth on, a new key saved
  setup_bt.py --port /dev/ttyACM0 --key --key-file k.json
"""
import argparse
import sys

from . import display_fw
from .usb_display import Display, DisplayError, codec, open_port

COST = """Bluetooth on: the device also takes pictures over Bluetooth Low Energy.
  Cost: about three times the battery use on battery (an unmeasured estimate), and it is found only within about 22 s
        (it advertises for 2 s in every 20 s). On the cable there is no extra cost.
  Key: the device makes a key now and this computer saves it in a file; only a computer with that file can use
        Bluetooth (no system pairing). A new key locks out the computers with the old one.
  Radio behaviour on real hardware is unproven until a person has tried it."""


def describe(state):
    key = f"a key (id {state['kid'].hex()})" if state.get("kid") else "no key"
    return f"Bluetooth is {'on' if state['on'] else 'off'}, the device is called {state['name']}, it has {key}"


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
    if args.key and args.off:
        raise DisplayError("--key and --off do not go together (a key switches Bluetooth on)")
    unattended = args.on or args.off or args.key or args.forget
    print(f"Bluetooth: {now}{' (default)' if now == 'off' else ''}")
    on, make_key = True if args.on else False if args.off else None, args.key
    if not unattended:
        print(COST)
        on = ask("Bluetooth: off (default) / on", ("off", "on"), now, input_fn) == "on"
        if on:
            has = display_fw.bt(display).get("kid")
            make_key = ask("Make a new key and save it here? yes / no" + (" (the old key stops working)" if has else ""),
                           ("yes", "no"), "no" if has else "yes", input_fn) == "yes"
    state = display_fw.bt(display, on=on, forget=True if args.forget else None)
    if make_key:
        state, path = display_fw.bt_key(display, args.key_file)
        print(f"The key is saved in {path} (keep it private; copy it to use the device from another computer)")
    print(describe(state))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", help="/dev/ttyACM0, COM5 (the USB cable)")
    p.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS", help="refused: setup runs over the cable only")
    p.add_argument("--on", action="store_true", help="Bluetooth on, without asking")
    p.add_argument("--off", action="store_true", help="Bluetooth off, without asking")
    p.add_argument("--key", action="store_true", help="Bluetooth on and a new key, saved here (the old one stops working)")
    p.add_argument("--key-file", metavar="PATH", help="save the key there instead of the key folder")
    p.add_argument("--forget", action="store_true", help="erase the device's key, drop the connection")
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
