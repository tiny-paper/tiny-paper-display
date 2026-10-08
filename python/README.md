# tiny-paper-display

Show pictures on a device running the tiny-paper display firmware, over its USB cable. MIT. Python 3.9+; one
dependency, pyserial. `pip install tiny-paper-display` works after the first release. Until then, use
`pip install ./python` from the repository root.

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

| Module | What |
|---|---|
| `tiny_paper_display.display_fw` | `panel_png` (fit any PNG to the panel), `show_png`, `clear` |
| `tiny_paper_display.usb_display` | `Display` (the link: `hello`, requests), `convert` |
| `tiny_paper_display.ble_port` | `BlePort` (a port over Bluetooth), `find()` |
| `tiny_paper_display.codec` | framing, CBOR, the serial port |

Tests, no hardware: `pip install -e "./python[test]"`, then `pytest python/tests`. They replay
`docs/spec/vectors/usb-display.json`, the same bytes the firmware's own test replays.
