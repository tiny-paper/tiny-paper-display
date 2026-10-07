# tinypaper-display

Show pictures on a device running the tiny-paper display firmware, over its USB cable. MIT. Python 3.9+; one
dependency, pyserial. Not published: install from this directory (`pip install ./python` from the repository root).

| Command | What |
|---|---|
| `tinypaper-show PORT picture.png` | one picture; returns when the panel has refreshed |
| `tinypaper-show PORT --clear` | a white panel |
| `tinypaper-movie PORT film.mp4 --every 5 --duration 60` | frames of a video (needs `ffmpeg`) |

| Module | What |
|---|---|
| `tinypaper_display.display_fw` | `panel_png` (fit any PNG to the panel), `show_png`, `clear` |
| `tinypaper_display.usb_display` | `Display` (the link: `hello`, requests), `convert` |
| `tinypaper_display.codec` | framing, CBOR, the serial port |

Tests, no hardware: `pip install -e "./python[test]"`, then `pytest python/tests`. They replay
`docs/spec/vectors/usb-display.json`, the same bytes the firmware's own test replays.
