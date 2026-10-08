# tiny-paper display

Firmware that turns an e-paper device you already have into a plain display: plug it into a computer with its USB
cable and send it a picture. No WiFi, no account, no apps. Bluetooth is there too, switched off until you switch it on.

| Board | Panel |
|---|---|
| TRMNL X (`trmnl_x`) | 1872 x 1404 |
| TRMNL OG (`trmnl_og`) | 800 x 480 |
| PicPak (`picpak`) | 400 x 300, black white red yellow |

| In the browser (Chrome or Edge on a computer) | |
|---|---|
| [Site](https://tiny-paper.github.io/tiny-paper-display/) | put the firmware on the device (Flash), draw on the panel (Sketch) |
| [Guide: use it](https://tiny-paper.github.io/tiny-paper-display/api.html) | pictures from Python, from the command line, what the answers mean, what to do when something goes wrong |

## Show a picture in three commands

Install the firmware from the Flash page first. The Python package is not on PyPI yet; this installs it from here:

```sh
python3 -m venv .venv
.venv/bin/pip install "git+https://github.com/tiny-paper/tiny-paper-display#subdirectory=python"
.venv/bin/tiny-paper-display show /dev/ttyACM0 picture.png
```

The port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem...` on macOS, `COM5` on Windows; the guide shows how to find it.
Any PNG is fitted to the panel first. The command returns when the panel has refreshed. `tiny-paper-display clear PORT`
makes the panel white; `tiny-paper-display movie PORT film.mp4 --every 5 --duration 60` shows frames of a video, one
every few seconds (needs `ffmpeg`).

The bytes of the USB protocol, for your own client in another language: [protocol reference](docs/spec/usb-display.md)
(on the site: `protocol.html`).

## Install the firmware

Open the [Flash page](https://tiny-paper.github.io/tiny-paper-display/) in Chrome or Edge on a computer, plug the device in,
press Install. The page finds the board, checks it and writes the newest release. Without Web Serial: every release
has a `flash.txt` per board with the `esptool` command (use the `# separate:` line, it keeps the device's settings).
Installing replaces the firmware the device has.

## Unplugged

The panel keeps the last picture with no power. On battery the device sleeps and looks for a computer every 20 s;
after plugging in, it answers within about 20 s, or at once after a press of its button. (By design; not measured.)

## Bluetooth

Off by default; with it off the device behaves exactly as without it. Nobody has tried the radio on a real device
yet: everything here is unproven until a person has, and the battery numbers are estimates.

| | |
|---|---|
| Switch on or off | only over the cable: the flash page's "Bluetooth setup" step, or `tiny-paper-display setup --port PORT` |
| What on costs | on battery about three times the use (the battery lasts about a third as long); on the cable nothing |
| Pairing | press the device's button (or use setup): for 2 minutes the panel shows `BLUETOOTH PAIRING` and 6 digits; type them on the computer or phone. Outside that window nothing can pair |
| Who can send pictures | paired devices only; anyone else gets nothing and changes nothing |
| Forget all paired devices | setup, over the cable |
| Send | install the extra (`bleak`, see the guide), then `tiny-paper-display show --bluetooth tp-display-XXXX picture.png` (the name is on the pairing screen); on battery the device is found within about 22 s |

## Going back

The device can always be flashed again over USB. Install the firmware it had with that firmware's own installer. To
be sure you can, read a backup of the whole flash before the first install: `esptool read_flash 0 ALL backup.bin`
(PicPak: with `--no-stub`, the first 16 MB); writing it back at 0 restores the device exactly.

## Not supported

| | |
|---|---|
| Other boards | none yet; the Xteink X4 Pro is not built |
| Scaling, dithering, rotation on the device | the picture is shown as given; the Python tool fits it first |
| WiFi, updates over the air | not in the image (the build checks the link map) |
| Sending pictures from a browser over Bluetooth | not yet; the Python package does it |
| Colour beyond the PicPak's four inks, 8-bit gray, RGB, alpha, interlaced PNG | refused with a clear error |

## Not proven yet

Seen on a real panel so far: on the TRMNL X, the firmware starts, and a sketch over the cable works with black strokes
as partial updates (2026-10-08); clearing by a partial update left traces, so Clear is now a full refresh. Everything
else has not been seen on a real panel: pictures over the cable and over Bluetooth, partial updates on the other boards
and at the gray depths, backup, flash, restore. Battery life unplugged is not measured. See `PLAN.md`.

## Build and test

| | |
|---|---|
| Tests, no hardware | `make test` (firmware host tests, Python tests, flash page tests; needs cmake, ninja, gcc, python3, node 20+) |
| Pages | `docs/use.md` becomes `site/api.html` and `docs/spec/usb-display.md` becomes `site/protocol.html`: run `npm run pages` in `site/` after editing either; a test fails when a page is stale. The Python samples of the guide are run by `python/tests/test_use_guide.py` |
| Firmware | `make -C firmware esp-setup` once (Docker; ESP-IDF v5.5.1, about 4.5 GB), then `make -C firmware esp BOARD=trmnl_og` (or `picpak`, `trmnl_x`): `firmware/build/esp/display[-<board>]/dist` with `flash.txt` |
| Guards in every build | the image fits the board's slot (`tools/check_slot.py`), no WiFi or network code is linked and Bluetooth is reached only through its one gate (`tools/check_no_radio.py`) |
| CI | `.github/workflows/build.yml`: tests and the three images on every push; a tag `v*` publishes them as a release and the flash page picks them up |

## Licenses

Firmware: GPL-3.0-or-later. Python package, flash page, docs: MIT. Files from others: `NOTICE`.

This repository is the home of the project: it is edited here. What is planned and what was seen on a device is in `PLAN.md`.
