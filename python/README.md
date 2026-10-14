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
| `tiny-paper-display setup --port PORT` | the Bluetooth setting (off by default) and key, asked; `--on`, `--off`, `--key` (`--key-file PATH`), `--forget` without questions. Cable only |
| `tiny-paper-display show --bluetooth NAME_OR_ADDRESS picture.png` | the same over Bluetooth (also `clear`, `movie`) |

Bluetooth: `pip install "tiny-paper-display[bluetooth]"` (adds `bleak` and `cryptography`). Make a key with the cable
(`setup --key`: Bluetooth on, about three times the battery use, an unmeasured estimate); the key is saved in
`~/.config/tiny-paper-display/keys/<name>.json` (mode 0600), no system pairing. Then use the device name
(`tp-display-XXXX`) or address; `tpd.open(bluetooth=NAME, key_file=PATH)` takes a key file from elsewhere (a container).
Every frame is sealed (AES-CCM); a device without that key answers `AUTH` (`bt_key.NoKey`). On battery the device is
found within about 22 s. Radio behaviour on real hardware
is unproven until a person has tried it.

```python
import tiny_paper_display as tpd
with tpd.open() as d:                                        # the first device found; closed at the end
    d.configure(full_every=30)                               # a setting on the device (None: default)
    d.show("photo.png", fit="fit", dither="floyd")           # path, bytes or tpd.Canvas
```

| Call | What |
|---|---|
| `tpd.devices(bluetooth=False)` | the devices running the display firmware: port or address, board, w, h |
| `tpd.open(target=None, bluetooth=None, key_file=None, **defaults)` | a `Device` (`caps`, `firmware`, `defaults`, `show`, `clear`, `configure`) |
| `tpd.open_all()` | a `Group` of every device; calls return a list of results |
| `tpd.show(picture)` | open, show, close |
| `tpd.Canvas(w, h)` | draw in 16 grays: `rect`, `frame`, `text`, `checker`, `crop` |

Picture options (`fit`, `border`, `dither`, `depth`, `threshold`, `rotate`) are per call, never sent to the device, and
give the pixels of the site's Picture page (`docs/spec/vectors/picture-prepare.json`). JPEG and other formats need Pillow: `pip install tiny-paper-display[images]`.
| Example | What it shows |
|---|---|
| `examples/quickstart.py` | the short version: find, show a picture, then a page drawn in code |
| `examples/test_page.py` | the reference: every gray depth and refresh mode, with timings |
| `examples/pc_stats.py` | live PC stats (psutil optional, else /proc; `--once`, `--png FILE`; pictures in `docs/img/`) |
| `examples/btc_ticker.py` | live Bitcoin price over 1-minute candles of the last hour, pushed by Kraken's WebSocket, partial updates with an inverse flash every 30 (`pip install websockets`) |
| `examples/live_wire/` | live Wikipedia edits and earthquakes (Docker) |

Pictures and videos from the command line: `tiny-paper-display show|clear|movie|setup`.

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
| `tiny_paper_display.bt_key` | the key file (`save`, `load`), `SealedPort` (the handshake and sealed frames), `NoKey`, `WrongKey` |
| `tiny_paper_display.codec` | framing, CBOR, the serial port |

Tests, no hardware: `pip install -e "./python[test]"`, then `pytest python/tests`. They replay
`docs/spec/vectors/usb-display.json`, the same bytes the firmware's own test replays.
