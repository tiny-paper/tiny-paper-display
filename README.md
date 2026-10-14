# tiny-paper display

Firmware that turns a TRMNL X into a plain e-paper display: plug it into a computer with its USB cable and send it a
picture. No WiFi, no account, no apps. Bluetooth is there too, off until you switch it on.

| | |
|---|---|
| Board | TRMNL X, 1872 x 1404, 16 grays |
| [Flash page](https://tiny-paper.github.io/tiny-paper-display/) | back up the device, install the firmware, restore (Chrome or Edge on a computer) |
| [Sketch page](https://tiny-paper.github.io/tiny-paper-display/sketch.html) | draw on the panel from the browser |
| [Guide](https://tiny-paper.github.io/tiny-paper-display/api.html) | Python, the command line, what the answers mean, what to do when something goes wrong |
| [Protocol](docs/spec/usb-display.md) | the bytes on the cable, for a client in another language |

## Install

1. Put the X in flashing mode: [TRMNL's instructions](https://help.trmnl.com/en/articles/11936721-put-your-trmnl-in-flashing-mode).
2. Open the [Flash page](https://tiny-paper.github.io/tiny-paper-display/), press Back up and keep the file, then Install.

Without Web Serial: each release has a `flash.txt` with the `esptool` command (use the `# separate:` line, it keeps the
device's settings). To go back, restore the backup from the Flash page, or install TRMNL's firmware with their installer.

## Show a picture

The package is not on PyPI yet; this installs it from here:

```sh
python3 -m venv .venv
.venv/bin/pip install "git+https://github.com/tiny-paper/tiny-paper-display#subdirectory=python"
.venv/bin/tiny-paper-display show /dev/ttyACM0 picture.png
```

The port is `/dev/ttyACM0` on Linux, `/dev/cu.usbmodem...` on macOS, `COM5` on Windows. Any picture is fitted to the
panel. `tiny-paper-display clear PORT` makes it white, `tiny-paper-display movie PORT film.mp4 --every 5` shows video
frames (needs `ffmpeg`).

```python
import tiny_paper_display as tpd

with tpd.open() as d:                    # the first device found
    d.show("photo.png", dither="floyd")
    page = tpd.Canvas(d.w, d.h)
    page.text(8, 8, "HELLO", 4)
    d.show(page, depth=1)                # the device refreshes only what changed
```

| Example (`python/examples/`) | What it shows |
|---|---|
| `quickstart.py` | the short version |
| `test_page.py` | every gray depth and refresh mode, with timings |
| `pc_stats.py` | live PC stats: CPU, memory, disks, network |
| `btc_ticker.py` | live Bitcoin price over 1-minute candles (Kraken WebSocket) |
| `live_wire/` | live Wikipedia edits and earthquakes (Docker) |

## Refreshes

| | |
|---|---|
| Partial | only what changed, no flash: a 1-bit page in about 1.9 s over USB |
| Full | the whole panel with a flash, at 1, 2 or 4 bit (a 4-bit page in about 3.3 s) |
| Clean-up | a full refresh after every 30 partial ones, against ghosting: `d.configure(full_every=N)`, stored on the device |

## Bluetooth

Off by default; with it off the device behaves exactly as without it.

| | |
|---|---|
| Switch on or off | over the cable only: the Flash page's Bluetooth step, or `tiny-paper-display setup --port PORT` |
| Key | `setup --key` makes a key on the device and saves it in a small file; no system pairing. Only computers with the file can send, every frame is encrypted and checked |
| Lock everyone out | `setup --key` again (a new key) or `--forget`, over the cable |
| Send | `pip install "tiny-paper-display[bluetooth]"`, then `tiny-paper-display show --bluetooth tp-display-XXXX picture.png` |
| Speed (Windows, BT 4.0 stick) | found in 7 s, connected in 2 s, a 1-bit page in 2.6 s |
| Cost | on battery about three times the use (an estimate); on the cable nothing |

## Unplugged

The panel keeps the last picture without power. On battery the device sleeps and looks for a computer every 20 s, or
at once after a press of its button.

## Seen on the device, and not yet

| Seen on the X (2026-10-14) | Not yet |
|---|---|
| pictures and pages over USB, all gray depths and refresh modes | battery life unplugged (not measured) |
| 437 PC stats updates in 14 minutes without a failure | pulling the cable during a transfer |
| Bluetooth from Windows with the key | the TRMNL OG and the PicPak: in the code, not released |

## Build and test

| | |
|---|---|
| Tests, no hardware | `make test` (firmware host tests, Python, flash page; needs cmake, ninja, gcc, python3, node 20+) |
| Firmware | `make -C firmware esp-setup` once (Docker, ESP-IDF v5.5.1), then `make -C firmware esp BOARD=trmnl_x` |
| Pages | `docs/use.md` becomes `site/api.html`, `docs/spec/usb-display.md` becomes `site/protocol.html`: `npm run pages` in `site/` (a test fails on a stale page) |
| Guards in every build | the image fits its slot, no WiFi or network code is linked, Bluetooth only through its one gate |
| Release | a tag `v*` builds and publishes the image; the Flash page picks it up |

## Licenses

Firmware: GPL-3.0-or-later. Python package, pages, docs: MIT. Files from others: `NOTICE`. Plans and device notes:
`PLAN.md`.
