# USB display protocol, version 1

The whole API of the display firmware (`firmware/display`): a board as a plain display
driven over its USB cable. Three things: `hello`, `show`, `clear`. Vectors: `vectors/usb-display.json`
(`gen_usb_display.py`), replayed by `firmware/components/usbdisp/test/test_usbdisp.c` and
`python/tests/test_display_fw.py`. Code: `firmware/components/usbdisp`.

## Frames

As the link protocol (link-protocol.md "Frame"): USB CDC byte stream, any baud rate.

| Off | Size | Field |
|---|---|---|
| 0 | 2 | sync `A5 5A` |
| 2 | 2 | `len` u16 LE: payload length, 0..6144 |
| 4 | 1 | `seq` 1..255, chosen by the host |
| 5 | 1 | `cmd`; the answer has `cmd \| 0x80` and the same `seq` |
| 6 | len | payload: one CBOR map with text keys in the order given below (`len = 0` is read as `{}`) |
| 6+len | 2 | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over bytes 2 .. 6+len-1, u16 LE |

Device to host, bytes outside frames are log text (`# ...\n`, never `A5`): skip to the next `A5 5A`. A frame with a
bad CRC gets no answer. One request at a time. Half a frame with no further byte for 2 s is forgotten. No retransmit
cache: after a lost answer send `hello` and start the picture again.

## Commands

| cmd | Name | Request | Answer |
|---|---|---|---|
| 0x01 | hello | any map (the app's `{ proto: 1, app: tstr }` is fine) | `{ proto: 1, project: "display", fw: tstr, board: tstr, w: uint, h: uint, depths: [uint], colors?: [tstr], max_payload: 6144, max_png: uint, chunk: 4096 }` |
| 0x08 | show_begin | `{ size: uint }` the PNG's bytes | `{ chunk: 4096, next: 0 }`; replaces an open transfer |
| 0x09 | show_chunk | `{ off: uint, data: bstr }` | `{ next: uint }`. `off` must be `next`; the first chunk holds at least the PNG's first 29 bytes and is checked at once (size, depth) |
| 0x0A | show_end | `{}` | `{ depth: uint, ms: uint, warn?: tstr }` after the full refresh. `ms`: time of the panel work |
| 0x0B | clear | `{}` | as show_end: the panel white, full refresh |

`project: "display"` tells this firmware from the app, whose hello has no `project` and which answers 0x08..0x0B with
`UNKNOWN_CMD`. `board`: `trmnl_og`, `picpak`, `trmnl_x`. `depths`: the gray bit depths taken. `colors`
(`["black","white","red","yellow"]`, PicPak): the panel's own colors. `warn`: the driver reported a refresh in less
time than the panel needs (SPI panels: under 300 ms): look at the panel.

Example (vector session `gray`, a 16 x 8 test panel):

| | Bytes |
|---|---|
| hello | `a55a 1400 01 01 a26570726f746f016361707067766563746f7273 2e7a` |
| answer | `a55a 6700 01 81 aa 6570726f746f01 6770726f6a65637467646973706c6179 62667769302e302e302b766563 65626f617264687665635f67726179 617710 616808 6664657074687382 0102 6b6d61785f7061796c6f6164191800 676d61785f706e67191028 656368756e6b191000 0773` |
| show_begin, 85 bytes | `a55a 0800 02 08 a16473697a651855 d13b` -> `a55a 1000 02 88 a2656368756e6b191000646e65787400 7125` |
| show_end | `a55a 0100 05 0a a0 8c0a` -> `a55a 0c00 05 8a a265646570746801626d7300 0eb8` |
| clear | `a55a 0100 0b 0b a0 bc22` -> `a55a 0c00 0b 8b a265646570746801626d7300 611d` |

## The picture

Exactly the panel's width and height, shown as given: no scaling, no dithering, no rotation on the device.

| Panel | PNG taken | Shown as |
|---|---|---|
| gray (`trmnl_og`, `trmnl_x`) | grayscale (color type 0), bit depth 1, 2 or 4 when in `depths` | its levels, 0 black |
| `colors` (`picpak`) | grayscale 1 bit; or indexed (color type 3, up to 8 bit) whose used entries are `000000`, `ffffff`, `ff0000`, `ffff00` | black, white, red, yellow |

Not taken: 8 or 16 bit, RGB, alpha, interlaced, an indexed PNG on a gray panel. Chunk CRCs and the Adler sum are not
checked (the link's CRC covered the bytes); ancillary chunks are skipped.

## Refusals

`{ err: uint, msg: tstr (<= 120 bytes), detail?: map }`. A refusal never changes the panel.

| err | Name | When |
|---|---|---|
| 1 | UNKNOWN_CMD | any other cmd 0x01..0x3F |
| 2 | BAD_REQUEST | payload not one CBOR map, a key missing or of the wrong type, a first chunk under 29 bytes |
| 3 | BUSY | the panel is refreshing: `detail { retry_ms: 2000 }`; `hello` is still answered. `trmnl_x`: requests wait for the refresh instead |
| 10 | IO | the panel driver failed (`msg` says at which step) |
| 11 | SEQUENCE | no transfer open, `off` not the next offset, or show_end before all bytes: `detail { next: uint }` |
| 13 | TOO_LARGE | `size` over `max_png`, or more bytes than `size` (the transfer is dropped) |
| 14 | NOMEM | no memory for the PNG, the frame or the decoder |
| 32 | NOT_PNG | no PNG signature, or under 57 bytes |
| 33 | WRONG_SIZE | not the panel's width and height (`msg` has both) |
| 34 | DEPTH | a color type, bit depth or color the panel does not show; interlaced |
| 35 | BAD_PNG | damaged or cut short |

## Bounds

| Item | Value |
|---|---|
| `max_png` | rows of the deepest PNG taken, a filter byte each, plus 4096: 100576 (`trmnl_og`), 124396 (`picpak`), 1319644 (`trmnl_x`) |
| Memory | the PNG, then the frame (`w * depth / 8 * h`) and the decoder (<= 32 KiB window, two rows); the PNG is freed before the refresh |
| Quiet transfer | dropped 10 s after its last request, memory freed |
| Cable pulled | the device restarts from deep sleep: no transfer state survives; the panel keeps the last picture shown |

Next (not in version 1): area updates, partial refresh.
