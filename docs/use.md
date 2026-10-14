# Use it

tiny-paper display turns a TRMNL X, a TRMNL OG or a PicPak into a plain e-paper display. A computer sends it a PNG
over the USB cable and it shows the picture. No WiFi, no account, no app. Bluetooth is there too, off until you switch it on.

You need:

- a supported device with the display firmware on it, installed from the [Flash page](../site/index.html)
- a USB data cable (a cable that only charges does not work)
- Chrome or Edge on a computer for the Flash and Sketch pages (they use Web Serial)
- for the Python package: Python 3.9 or newer

**What has been seen on a real device so far:** on a TRMNL X the firmware starts, and black strokes drawn on the Sketch
page show as partial updates. Clearing with a partial update leaves faint traces, so Clear is a full refresh. Nothing
else has been tried on a device. Where a section below depends on that, it says so.

## Try it without code

| Where | What |
|---|---|
| [Sketch page](../site/sketch.html) | Plug the device in, press Connect, pick its port. Draw: each stroke goes to the panel as a partial update of the area that changed |
| [Picture page](../site/picture.html) | Connect, then drop, pick or paste a picture, or use a test picture. The preview shows the exact pixels that will be sent. Choose fit, rotation, depth and dithering, then Send. The picture stays in the browser |
| Without a device | [TRMNL X](../site/sketch.html?mock=x), [TRMNL OG](../site/sketch.html?mock=og) or [PicPak](../site/sketch.html?mock=picpak): a pretend device answers, nothing reaches a panel. The same on the [Picture page](../site/picture.html?mock=x) |

### Picture page

| Setting | Choices |
|---|---|
| Fit | whole picture with a border, fill and crop, stretch |
| Rotate | 0, 90, 180, 270, auto (turns it when that fills more of the panel) |
| Depth | each depth the device takes (1, 2, 4 bit gray), on the PicPak its four colours |
| Dithering | none (nearest level; at 1 bit a threshold), Floyd-Steinberg, ordered 4x4 |
| Refresh | auto, partial (only what changed; the device compares with the frame it keeps), full |

Scaling averages the pixels a new pixel covers when shrinking and is bilinear when enlarging. Gray is luminance
`0.299 R + 0.587 G + 0.114 B` on the stored values, not linear light. Dithering comes after scaling. The status line
says what was asked, what the device did and why, the panel time and the total time. Compare sends the picture four ways
(1 bit and the deepest depth, each nearest and Floyd-Steinberg, all full refreshes) with a pause you set, and Copy report
gives a text to paste with a line for what you saw.

## Python

### Install

The package is not on PyPI yet. Today it installs from this repository. This command was run in a fresh virtual
environment and works:

```sh
python3 -m venv .venv
.venv/bin/pip install "git+https://github.com/tiny-paper/tiny-paper-display#subdirectory=python"
```

From a clone, `.venv/bin/pip install ./python` does the same. On Windows the tools are in `.venv\Scripts\`. The one
dependency is pyserial. After the first release it will be `pip install tiny-paper-display`.

The install adds the command `tiny-paper-display`. The examples below use that name: activate the environment
(`source .venv/bin/activate`, on Windows `.venv\Scripts\activate`) or write the path to it.

### Find the port

| Computer | Port looks like | Look it up |
|---|---|---|
| Linux, Raspberry Pi | `/dev/ttyACM0` | `ls /dev/ttyACM*`. You need access to it: `sudo usermod -aG dialout $USER`, then log in again |
| macOS | `/dev/cu.usbmodem...` | `ls /dev/cu.usbmodem*` |
| Windows | `COM5` | Device Manager, "Ports (COM & LPT)": the number shown |

On any system, `python -m serial.tools.list_ports -v` lists the ports. The device is the one made by Espressif
(vendor id `303A`). The examples write `/dev/ttyACM0`; use your port.

### The simple API

Find the devices, open one, draw, and it closes by itself (also after an error):

```python test
import tiny_paper_display as tpd

for dev in tpd.devices():                    # USB ports that answer as the display firmware; bluetooth=True also scans
    print(dev.port, dev.board, dev.w, dev.h)

with tpd.open() as d:                        # the first device; tpd.open("COM5"), tpd.open(bluetooth="NAME")
    print(d.caps.depths, d.caps.cleanup)  # what the board can do
    d.configure(cleanup={"screen": 30})      # stored on the device; None brings the defaults back
    d.show("picture.png", fit="fit", dither="floyd")
    box = tpd.Canvas(8, 4, 0)                # drawn in code: gray levels 0 (black) to 15
    d.area(box, 8, 0)                        # a rectangle; x and its width multiples of 8
    d.clear()
```

| Call | What |
|---|---|
| `tpd.devices(bluetooth=False)` | list of `Found(port, address, board, w, h)` |
| `tpd.open(target=None, bluetooth=None, **defaults)` | a `Device`; no target: the first found |
| `tpd.open_all()` | a `Group` of every device found; `show`, `area`, `clear`, `configure` return one result per device |
| `tpd.show(picture)` | open the first device, show, close |
| `d.show(picture, fit, border, dither, depth, threshold, rotate, mode)` | any picture: a path, bytes or a `Canvas` |
| `d.area(picture, x, y, mode=None)` | a rectangle at its own size; refused before sending when `x` or the width is not a multiple of 8 or it leaves the panel |
| `d.configure(cleanup=...)` | ghosting clean-up, stored on the device: `{"areas": N, "screen": M}`; returns the new `d.caps` (`d.caps.cleanup`) |
| `d.clear()` | a white panel |

Picture options are per call, with the defaults of the connection (`d.defaults`, or keywords of `open`); they are never
sent to the device. They are the Picture page's settings and give the same pixels as the page.

| Option | Values (default) |
|---|---|
| `fit` | `fit` (whole picture, border round it), `fill` (crop), `stretch` |
| `border` | `white`, `black` |
| `rotate` | `auto`, `0`, `90`, `180`, `270` |
| `depth` | `1`, `2`, `4`; none: the deepest the board has (a colour panel: its four inks) |
| `dither` | `floyd`, `ordered`, `none` |
| `threshold` | 0 to 255, for 1 bit with `dither="none"` (128) |

PNG needs nothing else. JPEG, WebP and the like work when Pillow is installed (`pip install Pillow`), with a clear error
otherwise. Converting a large photo takes a few seconds in plain Python.

### Show a picture in a few lines (the parts underneath)

```python test
from tiny_paper_display import display_fw
from tiny_paper_display.usb_display import Display, open_port

with open_port("/dev/ttyACM0") as port:
    display = Display(port)
    hello = display.hello()                       # what the device is and can do
    png = open("picture.png", "rb").read()
    png = display_fw.panel_png(png, hello)        # fitted to the panel when it is not the right size
    reply = display_fw.show_png(display, png, hello)
    print(display_fw.describe(reply))
```

`show_png` returns when the panel has finished refreshing. Only one program can hold the port: close the Sketch page
first.

### Show a picture from the command line

```sh test
tiny-paper-display show /dev/ttyACM0 picture.png
```

It prints what the device is, then the answer, then one line saying which refresh ran. On Windows write `COM5`.

```
fake display firmware 0.0.0+fake: 16x8, gray depths [1, 2], partial at [1] bit, modes full partial partial_all inverse passes
{"depth": 2, "h": 8, "mode": "full", "ms": 0, "w": 16, "why": "depth", "x": 0, "y": 0}
Panel: full (depth) 0,0 16x8 depth 2 0 ms; total 0.002s
```

That is the output against the pretend 16 x 8 test device of this project's tests. Yours names your board and its panel.
The first line is what the device is, the second the device's answer, the third which refresh ran.

### Clear

```sh test
tiny-paper-display clear /dev/ttyACM0
```

In Python: `display_fw.clear(display)`. Clear is always one full refresh.

### What the answer tells you

`show_png` and `clear` return the device's answer as a dict. `display_fw.describe(reply)` turns it into a line such as
`partial (changed) 8,2 8x1 depth 1 0 ms`: the refresh that ran, why, the area it drove, the bit depth and the time
the panel took.

| `mode` | What ran |
|---|---|
| `full` | the whole panel, with the full waveform (the panel flashes) |
| `partial` | only the pixels that changed, without a flash |
| `partial_all`, `inverse`, `passes` | the refreshes you can ask for on a 1 bit picture, see below |
| `none` | nothing: the panel already shows this picture |

| `why` | In words |
|---|---|
| `asked` | you named the mode |
| `changed` | partial because the picture changed |
| `unchanged` | the picture is the one already shown |
| `first` | full because nothing was known yet (after power-on) |
| `depth` | full because the bit depth changed |
| `area` | full, but only the area in the answer: the device refreshes a rectangle fully on its own (TRMNL X with the tuning key `driver` set to `plain`) |
| `cleanup` | full because the count of partial updates since the last full one was reached (20 unless changed) |
| `board` | full because this board has no partial refresh |
| `refused`, `cut`, `failed`, `memory` | full to recover after a refused or cut transfer, a panel failure, or too little memory to keep the last frame |

The same picture sent twice is not uploaded the second time: the package asks the device first, and you get
`unchanged`. To redraw it anyway, ask for `--mode full`.

### Ask for a partial or a full refresh

Without a mode the device decides: partial when it can, full when it must. To name one:

```sh test
tiny-paper-display show /dev/ttyACM0 picture.png --mode full
```

```python test
from tiny_paper_display import display_fw
from tiny_paper_display.usb_display import Display, open_port

with open_port("/dev/ttyACM0") as port:
    display = Display(port)
    hello = display.hello()
    png = display_fw.panel_png(open("picture.png", "rb").read(), hello)
    reply = display_fw.show_png(display, png, hello, mode="full")
    print(display_fw.describe(reply))
```

`--mode` takes `full`, `partial`, `partial_all`, `inverse` or `passes` (with `--passes 1` to `4`). A board refuses a
mode it cannot do: the package checks against what the device reported and says so before sending. See the table of
boards below.

### Update only part of the screen

Send a small PNG and say where it goes. Its left edge `x` and its width must be multiples of 8, and all of it must lie
inside the panel. This makes a black 8 x 4 box:

```python test
from tiny_paper_display import display_fw

open("box.png", "wb").write(display_fw.write_png(8, 4, 1, bytes(4)))
```

```sh test
tiny-paper-display show /dev/ttyACM0 box.png --at 8,4
```

In Python: `display_fw.show_area(display, png, hello, 8, 4)`. The rectangle is sent as it is, not fitted. The device must
have a partial refresh at the bit depth of the box. Where it does not (a PicPak), it refuses with `MODE`.

### Ask the device what it can do

```python test
from tiny_paper_display.usb_display import Display, open_port

with open_port("/dev/ttyACM0") as port:
    hello = Display(port).hello()
    print(hello["board"], hello["fw"], hello["w"], hello["h"])
    print("gray depths", hello["depths"], "partial at", hello["partial_depths"], "modes", hello["modes"])
```

`depths` are the bit depths it shows. `partial_depths` are the ones it refreshes partially (empty: none). `modes` are
the refreshes it can do, and `depth_modes` (`{"1": [...], "2": [...]}`) which of them at which depth: on a TRMNL X only `full`
and `partial` at 2 and 4 bit. `full_every` is the full refresh after that many partial ones (default 30; `full_every_set`
false while the default applies). In Python, `display_fw.capabilities(display)` returns all of it (canvas, depths,
partial_depths, depth_modes, colors, max_png, full_every), `display_fw.full_every(display, 50)` stores a value and
`display_fw.full_every(display, clear=True)` brings the default back. A PicPak also has `colors`.

### Bluetooth

**Not tried on a device yet.** The radio has never been run on a real board. Everything here is how it is built to work,
and the battery cost is an estimate that has not been measured.

Bluetooth is off by default, and you switch it on only over the cable. Install the extra part, then set it up:

```sh
.venv/bin/pip install "tiny-paper-display[bluetooth] @ git+https://github.com/tiny-paper/tiny-paper-display#subdirectory=python"
```

```sh test
tiny-paper-display setup --port /dev/ttyACM0 --on --pair
```

For 2 minutes the panel shows `BLUETOOTH PAIRING` and 6 digits. Type them where your computer asks for them. Outside that
window nothing can pair. Then send, using the name shown on the pairing screen (`tp-display-` and 4 characters):

```sh
tiny-paper-display show --bluetooth tp-display-XXXX picture.png
```

`python -c "from tiny_paper_display.ble_port import find; print(find())"` lists the devices in range. On battery the device
is only listening now and then, so finding it can take up to about 22 seconds. `setup --off` switches Bluetooth off and
`setup --forget` removes every pairing.

## What kind of picture to give it

| Board | Panel | The device takes | Partial refresh (by design) |
|---|---|---|---|
| TRMNL X (`trmnl_x`) | 1872 x 1404 | gray PNG, 1, 2 or 4 bit | 1, 2 and 4 bit; the grays are not judged on a device |
| TRMNL OG (`trmnl_og`) | 800 x 480 | gray PNG, 1 or 2 bit | 1 bit |
| PicPak (`picpak`) | 400 x 300, four inks | gray 1 bit, or indexed with exactly black, white, red, yellow | none |

The device takes a PNG of exactly the panel's size and shows it as given: no scaling, no dithering, no rotation. It
refuses 8 and 16 bit gray, RGB, alpha, interlaced files, and an indexed file on a gray panel.

| What | The Python package | You |
|---|---|---|
| Size | fits the picture on white, centered, keeping its proportions | choose the picture; crop or rotate it first if you want that |
| Colors | gray panels: the nearest gray level (no dithering). PicPak: the nearest of the four inks | dither or adjust contrast first if the result looks flat |
| Input files | reads gray or indexed 1 to 8 bit, RGB, gray with alpha and RGBA 8 bit, not interlaced, up to 16 MiB and 4 megapixels; alpha is put on white | convert other kinds (16 bit, interlaced) to PNG first |
| Already right | a gray PNG of the panel's size and a depth the device takes is sent unchanged | |
| Sending | checks the size limit and the mode, waits while the panel is busy | |
| Area updates | sends the rectangle as given | cut the rectangle and give `x` and `y` (multiples of 8) |

A picture the size of the TRMNL X takes about ten seconds to convert in Python (measured once on a desktop computer).

## When something goes wrong

| You see | Meaning | Do |
|---|---|---|
| `/dev/ttyACM0 is busy: close ...` | another program holds the port | close the Sketch or Flash page, a terminal program, or another copy of this tool |
| `No permission to open /dev/ttyACM0` | Linux: your user may not use the port | the message gives the fix (the `dialout` group, or a udev rule); log in again |
| `no reply for command 0x01 ...` | nothing answered | wrong port, a charge-only cable, or not the display firmware: install it from the Flash page. On battery the device only looks for a computer now and then (by design, not measured): press its button and try again |
| `... needs the display firmware (hello has no project: display)` | the device answers but runs other firmware | install the display firmware from the Flash page |
| `display refused: ... (WRONG_SIZE 33)` | the PNG is not the panel's size | use `panel_png`, or `show` without `--at`, so it is fitted |
| `display refused: ... (DEPTH 34)` | a picture kind this panel does not show | use `panel_png`, or save as a non-interlaced PNG the table above allows |
| `display refused: ... (MODE 36)` or `(RECT 37)` | the board cannot do that refresh, or the area is not allowed | ask `hello` for `modes` and `partial_depths`; keep `x` and the width at multiples of 8, inside the panel |
| `display refused: ... (BUSY 3)` | the panel is refreshing | the package already waits up to a minute; try again when the panel has stopped |
| "This browser cannot reach a USB device (no Web Serial)" | the Sketch or Flash page needs Web Serial | use Chrome or Edge on a computer |

## JavaScript

There is no npm package yet; it is still to do. Nothing here is a documented JavaScript API.

What exists today is the Sketch page, which talks to the device from a browser over Web Serial. Its source is a working
example you can read:

| File | What |
|---|---|
| `site/src/display/client.ts` | `DisplayClient`: `hello()`, `show(png, hello, options)`, `clear()` |
| `site/src/display/mockDevice.ts` | the pretend device behind `?mock=` |
| `site/src/display/png.ts` | `packedPng`: a panel PNG from packed rows |
| `site/frame.js` | the framing of the messages on the serial stream |
| `site/sketch-page.js` | the page: opens the port with `navigator.serial.requestPort` and ties these together |

`npm run build` in `site/` makes `site/sketch.js` from `site/src`. Expect these names to change before a release.

## The protocol

Writing your own client in another language? The bytes, the commands and every error code are in
[the protocol reference](../site/protocol.html). You do not need it for anything above.
