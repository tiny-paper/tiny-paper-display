# tiny-paper-display

Show pictures on a device running the tiny-paper display firmware, over its USB cable. MIT. Python 3.9+; one
dependency, pyserial. `pip install tiny-paper-display` works after the first release. Until then, use
`pip install ./python` from the repository root, or
`pip install "git+https://github.com/tiny-paper/tiny-paper-display#subdirectory=python"`. The guide with every step:
`docs/use.md` (the site's "Use it" page).

| Command | What |
|---|---|
| `tiny-paper-display show PORT picture.png` | one picture; returns when the panel has refreshed |
| `tiny-paper-display clear PORT` | a white panel |
| `tiny-paper-display movie PORT film.mp4 --every 5 --duration 60` | frames of a video (needs `ffmpeg`) |
| `tiny-paper-display setup --port PORT` | the Bluetooth setting (off by default), asked; `--on`, `--off`, `--pair`, `--forget` without questions. Cable only |
| `tiny-paper-display show --bluetooth NAME_OR_ADDRESS picture.png` | the same over Bluetooth (also `clear`, `movie`) |

Bluetooth: `pip install "tiny-paper-display[bluetooth]"` (adds `bleak`). Turn it on with the cable (`setup`, about three
times the battery use, an unmeasured estimate), open the pairing window (120 s), type the passkey the panel shows on the
computer, then use the device name (`tp-display-XXXX`) or address. A refusal `AUTH` reads "not paired: run setup with the
cable and press the device's button". On battery the device is found within about 22 s. Radio behaviour on real hardware
is unproven until a person has tried it.

```python
import tiny_paper_display as tpd
with tpd.open() as d:                                        # the first device found; closed at the end
    d.configure(cleanup={"screen": 30})                      # a setting on the device, see below
    d.show("photo.png", fit="fit", dither="floyd")           # path, bytes or tpd.Canvas
    d.area(tpd.Canvas(48, 24, 0), 8, 8)                      # a rectangle, x a multiple of 8
```

| Call | What |
|---|---|
| `tpd.devices(bluetooth=False)` | the devices running the display firmware: port or address, board, w, h |
| `tpd.open(target=None, bluetooth=None, **defaults)` | a `Device` (`caps`, `defaults`, `show`, `area`, `clear`, `configure`) |
| `tpd.open_all()` | a `Group` of every device; calls return a list of results |
| `tpd.show(picture)` | open, show, close |
| `tpd.Canvas(w, h)` | draw in 16 grays: `rect`, `frame`, `text`, `checker`, `crop` |

Ghosting builds up in a live page when partial updates repeat. `configure(cleanup=...)` (stored on the device,
`d.caps.cleanup` reads it): `areas` cleans the changed parts every N updates (default 15, 0 never), `screen` refreshes the
whole screen every M updates (default 0, never). A key left out keeps its value.

| `cleanup` | Means |
|---|---|
| `None` | the defaults: `{"areas": 15, "screen": 0}` |
| `{"screen": 30}` | a whole-screen refresh every 30 updates, as before |
| `{"areas": 5}` | clean the changed parts every 5 updates |
| `{"areas": 0, "screen": 0}` | no cleaning (the device still does a full refresh when it must) |

Picture options (`fit`, `border`, `dither`, `depth`, `threshold`, `rotate`) are per call, never sent to the device, and
give the pixels of the site's Picture page (`docs/spec/vectors/picture-prepare.json`). JPEG and other formats need Pillow: `pip install tiny-paper-display[images]`.
`examples/quickstart.py` is the short version, `examples/test_page.py` the reference, `examples/pc_stats.py` live PC stats
(CPU, memory, disks, network, processes; psutil optional, else /proc; `--once`, `--png FILE`; pictures in `docs/img/`).

| Error (all subclasses of `DisplayError`) | When |
|---|---|
| `Refused` | the device said no: `.err` code, `.name` (e.g. `MODE` 36), `.msg`, `.detail` |
| `Busy` | a kind of `Refused`: busy for longer than the wait |
| `Disconnected` | port gone, or a transfer cut twice (a cut one is retried once on a new link) |
| `PanelFault` | the answer's gate clock count is wrong (`.reply`); not returned silently |

After any failure the `Device` stays usable and the next `show` sends the whole picture. `group.show(...)` returns, per
device, the answer or the exception; the other devices go on.

| Logging (standard `logging`, logger `tiny_paper_display`, silent by default) | What |
|---|---|
| `tpd.open(log="info")` or `tpd.log("info")` | one stderr handler; `tpd.log(None)` removes it |
| `info` | per show: host prepare ms, transfer ms, the device's mode, why and ms |
| `debug` | each request and answer (command, keys, bytes by length only) |
| `warning` | retries and refusals |

`show` results carry `prepare_ms` and `transfer_ms` (the host's time) next to the device's `ms`. Measured on this machine
for 1872x1404 (`pytest -s python/tests/test_speed.py`): fit + 4 bit without dither 0.5 s, Floyd-Steinberg 2.6 s, Canvas to
PNG under 0.05 s, PNG decode 0.03 s (filter none) to 3 s (Paeth rows; install Pillow for any PNG or JPEG).

| Module | What |
|---|---|
| `tiny_paper_display.picture` | the Picture page's preparation: `prepare`, `pack_rows` |
| `tiny_paper_display.display_fw` | `panel_png` (fit any PNG to the panel), `show_png`, `clear` |
| `tiny_paper_display.usb_display` | `Display` (the link: `hello`, requests), `convert` |
| `tiny_paper_display.ble_port` | `BlePort` (a port over Bluetooth), `find()` |
| `tiny_paper_display.codec` | framing, CBOR, the serial port |

Tests, no hardware: `pip install -e "./python[test]"`, then `pytest python/tests`. They replay
`docs/spec/vectors/usb-display.json`, the same bytes the firmware's own test replays.
