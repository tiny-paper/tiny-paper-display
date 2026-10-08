# USB display protocol, version 1

The whole API of the display firmware (`firmware/display`): a board as a plain display
driven over its USB cable. Commands: `hello`, `have`, `show` (a whole picture or a rectangle, with a refresh mode), `clear`,
`set`. Vectors: `vectors/usb-display.json` (`gen_usb_display.py`), replayed by
`firmware/components/usbdisp/test/test_usbdisp.c`, and `python/tests/test_display_fw.py`. The corpus also has codec-only fault replies (`refusals`) for IO and NOMEM. Code:
`firmware/components/usbdisp`.

The browser shows a USB display page for `project: "display"`: fit or fill, conversion with the album photo pipeline,
an exact pixel preview, Show and Clear. Returning to the app uses Flash. The debug relay's bounded `display`
command carries a source PNG to that same browser conversion (docs/relay.md "USB display"). No wire change.

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
| 0x01 | hello | any map (the app's `{ proto: 1, app: tstr }` is fine) | `{ proto: 1, project: "display", fw: tstr, board: tstr, w: uint, h: uint, depths: [uint], colors?: [tstr], max_payload: 6144, max_png: uint, chunk: 4096, partial_depths: [uint], modes: [tstr], full_every: uint, fingerprint_hash: "fnv1a64", fingerprint?: tstr, bt?: tstr }` |
| 0x08 | show_begin | `{ size: uint, x?: uint, y?: uint, mode?: tstr, passes?: uint }`: the PNG's bytes, where it goes and how ("Area and refresh mode") | `{ chunk: 4096, next: 0 }`; replaces an open transfer (which counts as cut) |
| 0x09 | show_chunk | `{ off: uint, data: bstr }` | `{ next: uint }`. `off` must be `next`; the first chunk holds at least the PNG's first 29 bytes and is checked at once (size, depth) |
| 0x0A | show_end | `{}` | `{ depth: uint, ms: uint, mode: tstr, why: tstr, x: uint, y: uint, w: uint, h: uint, passes?: uint, ckv?: map, warn?: tstr, same?: true }` after the refresh ("The answer") |
| 0x0B | clear | `{}` | as show_end: the panel white, `mode: "full"`, `why: "asked"` |
| 0x0C | set | `{ full_every?: uint }` 0 (never) .. 250; without the key nothing changes | `{ full_every: uint }` the value in use; stored on the device, kept over power-off |
| 0x0D | bt | `{ on?: bool, pair?: bool, forget?: bool }`, over the USB cable only ("Bluetooth") | `{ on: bool, pairing: uint, name: tstr }`: the setting, the seconds the pairing window stays open (0: closed), the advertised name |
| 0x0E | have | `{ fingerprint: tstr }` | `{ same: true }` when known and equal, else `{ send: true }`; no PNG bytes, refresh or counter change |

`project: "display"` tells this firmware from the app, whose hello has no `project` and which answers 0x08..0x0B with
`UNKNOWN_CMD`. `board`: `trmnl_og`, `picpak`, `trmnl_x`. `depths`: the gray bit depths taken. `colors`
(`["black","white","red","yellow"]`, PicPak): the panel's own colors. `warn`: the driver reported a refresh in less
time than the panel needs (SPI panels: under 300 ms): look at the panel. `partial_depths`: the depths the board
refreshes partially (empty: none). `modes`: the modes it does at some depth. `full_every`: the setting. `bt`: `"off"` or
`"on"`, the Bluetooth setting; absent on a build without Bluetooth, which answers `bt` with `UNKNOWN_CMD`.

Example (vector session `gray`, a 16 x 8 test panel, partial at 1 bit):

| | Bytes |
|---|---|
| hello | `a55a 1400 01 01 a26570726f746f016361707067766563746f7273 2e7a` |
| answer | `a55a b300 01 81 ad 6570726f746f01 6770726f6a65637467646973706c6179 62667769302e302e302b766563 65626f617264687665635f67726179 617710 616808 6664657074687382 0102 6b6d61785f7061796c6f6164191800 676d61785f706e67191028 656368756e6b191000 6e7061727469616c5f6465707468738101 656d6f64657385 6466756c6c 677061727469616c 6b7061727469616c5f616c6c 67696e7665727365 66706173736573 6a66756c6c5f657665727914 31d2` |
| show_begin, 86 bytes | `a55a 0800 31 08 a16473697a651856 e617` -> `a55a 1000 31 88 a2656368756e6b191000646e65787400 2322` |
| show_end, first picture | `a55a 0100 05 0a a0 8c0a` -> `a55a 2c00 05 8a a8 65646570746801 626d7300 646d6f64656466756c6c 63776879656669727374 617800 617900 617710 616808 ee39` (full, first) |
| show_end, a corner changed | `a55a 0100 33 0a a0 897d` -> `a55a 3100 33 8a a8 65646570746801 626d7300 646d6f6465677061727469616c 63776879676368616e676564 617808 617902 617708 616802 e451` (partial, changed, 8,2 8x2) |
| show_begin, 8 x 4 at 0,4, inverse | `a55a 1b00 3d 08 a4 6473697a651847 617800 617904 646d6f646567696e7665727365 08b2` |
| clear | `a55a 0100 0b 0b a0 bc22` -> `a55a 2c00 0b 8b a8 65646570746801 626d7300 646d6f64656466756c6c 637768796561736b6564 617800 617900 617710 616808 8a24` |
| set | `a55a 0d00 70 0c a16a66756c6c5f657665727902 4e31` -> `a55a 0d00 70 8c a16a66756c6c5f657665727902 3066` |

## Fingerprint and duplicate pictures

`hello.fingerprint_hash` advertises `fnv1a64`: FNV-1a, 64 bits, offset `cbf29ce484222325`, prime
`100000001b3`, unsigned arithmetic modulo 2^64. The fingerprint is exactly 16 lowercase hex digits.
Hash these bytes in order: panel width u16 BE, height u16 BE, accepted panel depth u8, color flag u8
(0 gray, 1 black/red/yellow/white), then packed panel rows, top first, MSB first. Zero unused low bits
at each row's end. Gray samples are the accepted levels; color codes are 0 black, 1 red, 2 yellow,
3 white, at depth 2 (including gray 1-bit input on PicPak). PNG chunks, filters and compression do
not enter the hash. This is a noncryptographic duplicate check with a 64-bit collision risk.

The fingerprint is remembered at every accepted depth, including boards without partial refresh,
until the panel frame becomes unknown: power-on, reset, refused/cut show transfer or panel failure.
It is absent from `hello` then. Keeping only the hash does not enable partial refresh.
`have` answers `send` during an open transfer. A malformed fingerprint gets BAD_REQUEST.

After decoding a `show`, identical pixels with default or `partial` mode answer immediately with
`mode: "none"`, `why: "unchanged"`, `same: true`; no panel call or clean-up count is used, even when
clean-up is due. Explicit `full` always redraws, including identical pixels, and answers `why: "asked"`
so a person can clean the panel. `partial_all`, `inverse` and `passes` also run as asked.

Hosts prepare the panel picture before hashing it. Compare against hello and the last successful
send on this connection, then validate a match with `have` before skipping the upload. A new link
asks hello again and discards host memory. A repeated area can use its last whole-panel fingerprint
(read back through hello), never the area's hash in `have`. Scripts print skipped uploads; `--force`
sends PNG bytes anyway. To force a panel refresh too, ask `--mode full`.
Shared `fingerprints` vectors contain two fixed pictures differing in one pixel at depths 1, 2, 4
and color depth 2; both firmware and Python tests read them.

## The picture

Exactly the panel's width and height (with `x`, `y`: a rectangle, below), shown as given: no scaling, no dithering,
no rotation on the device.

| Panel | PNG taken | Shown as |
|---|---|---|
| gray (`trmnl_og`, `trmnl_x`) | grayscale (color type 0), bit depth 1, 2 or 4 when in `depths` | its levels, 0 black |
| `colors` (`picpak`) | grayscale 1 bit; or indexed (color type 3, up to 8 bit) whose used entries are `000000`, `ffffff`, `ff0000`, `ffff00` | black, white, red, yellow |

Not taken: 8 or 16 bit, RGB, alpha, interlaced, an indexed PNG on a gray panel. Chunk CRCs and the Adler sum are not
checked (the link's CRC covered the bytes); ancillary chunks are skipped.

## Area and refresh mode

The device keeps the frame it last showed, at the depths in `partial_depths`. A `show` then updates only what changed,
without a flash; the full refresh comes by itself. Nothing is replaced silently: a mode the board or the depth cannot
do is refused (err 36), and the answer says which refresh ran and why.

| `mode` | Drives | Needs |
|---|---|---|
| none (the default) | `partial` when the frame kept is known and has the picture's depth, else `full`; `full` also after `full_every` partial updates | |
| `full` | the whole panel, with the panel's full waveform | |
| `partial` | the changed pixels only: the device compares with the frame kept and drives the smallest rectangle that holds them (x and width in steps of 8) | the depth in `partial_depths` |
| `partial_all` | every pixel of the rectangle, changed or not | 1 bit, `partial_all` in `modes` |
| `inverse` | the rectangle to the negative of the new picture, then to the picture (two partial refreshes: every pixel changes at least once) | 1 bit, `inverse` in `modes` |
| `passes` | as `partial_all`, `passes` (1..4) times | 1 bit, `passes` in `modes` |

`x`, `y` (both or neither; `x` a multiple of 8): the PNG is a rectangle with its top left pixel there. Its width is a
multiple of 8, all of it lies inside the panel, its depth is in `partial_depths` and is the depth of the picture shown.
Without them the rectangle is the whole panel. `full` with a rectangle refreshes the whole panel with the rectangle put
into the frame kept.

The frame kept is forgotten, so the next update is a full refresh whatever was asked (a rectangle then has white
around it): at power-on, after a refused step of a show, a transfer cut (quiet for 10 s, or replaced by a new
show_begin), a panel failure, and after a picture at a depth outside `partial_depths`. `clear` leaves a white 1 bit
frame. A whole picture without a mode gives up the frame kept when memory is short (`why: "memory"`).

### The answer

| Key | Value |
|---|---|
| `mode` | what ran: `full`, `partial`, `partial_all`, `inverse`, `passes`, or `none` (the picture is what the panel shows: nothing driven) |
| `why` | `asked` (the mode named), `changed`, `unchanged`, or for a full refresh nobody named: `first`, `refused`, `cut`, `failed`, `memory` (why the frame kept was not known), `depth` (another depth than the picture shown, or one outside `partial_depths`), `board` (no partial refresh), `cleanup` (`full_every` reached) |
| `x`, `y`, `w`, `h` | the area driven (all 0 with `none`) |
| `same` | true with `mode: "none"` |
| `passes` | with the mode `passes` |
| `ckv` | TRMNL X: `{ edges: uint, frames: uint, per: uint, rows: uint, ok: bool }`, the gate clock edges the firmware counted over the last refresh of the update, the drive frames, edges a frame, the panel's rows; `ok` false when a frame did not clock once per row (issue #203: the picture is then drawn at the wrong height). Log: `# display W show ... WRONG` |

`full_every` (default 20, `set`): the count of partial updates since the last full refresh; a named mode is run as
named and counts, except an unchanged `partial` which uses no count.

## Refusals

`{ err: uint, msg: tstr (<= 120 bytes), detail?: map }`. A refusal never changes the panel.

| err | Name | When |
|---|---|---|
| 1 | UNKNOWN_CMD | any other cmd 0x01..0x3F |
| 2 | BAD_REQUEST | payload not one CBOR map, a key missing or of the wrong type, a first chunk under 29 bytes, `passes` without the mode `passes` or outside 1..4, `full_every` over 250 |
| 3 | BUSY | the panel is refreshing: `detail { retry_ms: 2000 }`; `hello` is still answered. `trmnl_x`: requests wait for the refresh instead |
| 10 | IO | the panel driver failed (`msg` says at which step; the panel may show the old or the new picture), or `set` could not store |
| 11 | SEQUENCE | no transfer open, `off` not the next offset, or show_end before all bytes: `detail { next: uint }` |
| 12 | AUTH | a request over Bluetooth on a link that is not encrypted with a bonded, passkey-checked key: every command, `hello` too; no `detail` |
| 13 | TOO_LARGE | `size` over `max_png`, or more bytes than `size` (the transfer is dropped) |
| 14 | NOMEM | no memory for the PNG, the frame, the decoder or the negative of `inverse` |
| 32 | NOT_PNG | no PNG signature, or under 57 bytes |
| 33 | WRONG_SIZE | without `x`, `y`: not the panel's width and height (`msg` has both) |
| 34 | DEPTH | a color type, bit depth or color the panel does not show; interlaced; a rectangle at another depth than the picture shown |
| 35 | BAD_PNG | damaged or cut short |
| 36 | MODE | an unknown `mode`; a mode or a rectangle the board cannot do, or not at this depth (`msg` says which) |
| 37 | RECT | `x` without `y`, `x` not a multiple of 8, outside the panel; the PNG's width not a multiple of 8, or part of it outside |
| 38 | CABLE | `bt` over Bluetooth: the setting, the pairing window and the bonds change only over the USB cable |

## Bounds

| Item | Value |
|---|---|
| `max_png` | rows of the deepest PNG taken, a filter byte each, plus 4096: 100576 (`trmnl_og`), 124396 (`picpak`), 1319644 (`trmnl_x`) |
| Memory | the PNG, then the frame (`w * depth / 8 * h`) and the decoder (<= 32 KiB window, two rows); the PNG is freed before the refresh. At a depth in `partial_depths` one more frame is kept between updates; `inverse` adds the rectangle's rows |
| Rectangle | 8 x 1 up to the panel; the PNG's size bound is `max_png` as for a whole picture |
| `passes` | 1..4 |
| `full_every` | 0..250, default 20 |
| Quiet transfer | dropped 10 s after its last request, memory freed |
| Cable pulled | the device restarts from deep sleep: no transfer state and no frame kept survive (the next update is a full refresh); the panel keeps the last picture shown |

## Boards

| Board | `partial_depths` | `modes` |
|---|---|---|
| `trmnl_x` | 1, 2 and 4 (the gray partial of the panel is on by default; experimental, never seen on a real panel) | full, partial, partial_all, inverse, passes |
| `trmnl_og` | 1 | full, partial, inverse |
| `picpak` | none | full |

The X driver takes partials at 2 and 4 bit (`epdp_gray_partial`). In the display firmware it is on by default, because the
Sketch page draws grays with it; the result is not yet seen on a real panel. The firmware reports what the board's
`hal_panel_gray_partial` says, so `partial_depths` is 1, 2, 4. The console command `!tp gpart off` switches it off again
(kept across restarts; `!tp gpart on` back); there is no `set` key for it. The X keeps its redrive bitmap in RAM only here (`hal_panel_fresh_ram`, issue #214):
no flash file is written per partial update.

## Bluetooth

The same firmware also takes the same frames over Bluetooth Low Energy. Off by default (owner decision 2026-10-08).
Radio behaviour is unproven until a person has tried it on a device; every number of "Cost" is an estimate.

### Setting

| Item | Value |
|---|---|
| Stored | one byte under the key `dbt` (0 off, 1 on), kept over power-off; missing or any other value: off |
| Changed | `bt { on }` over the USB cable only; over Bluetooth `bt` is refused `CABLE`. Read: `hello.bt`, `bt {}` |
| Off | the Bluetooth stack is never started: no controller init, no advertising, no radio calibration. Sleep and wake are exactly those of a build without Bluetooth. A button press does nothing more than wake the device |
| Switched off | the connection is dropped and the stack stopped after the answer; the bonds stay (`forget` deletes them) |
| Boards | `trmnl_x`, `trmnl_og`, `picpak` |

### Transport

| Item | Value |
|---|---|
| Service | `48770001-4516-40a8-8cc6-57e06fe2ff0d` in the advertising data; the name (`tp-display-` and 4 hex digits of the device's address) in the scan response |
| RX `48770002-...` (same suffix) | write, host to device: the next 1..MTU-3 bytes of the host's stream of frames. Takes only an encrypted, passkey-checked link (else ATT error 0x05 or 0x0F, which makes the host's system pair) |
| TX `48770003-...` | notify, device to host: the next <= MTU-3 bytes of the device's frames (no log text) |
| Pair `48770004-...` | read, value `01`, needs the same link: a host reads it to make its system pair |
| Frames, commands | as over USB, the same `seq`, CRC, bounds and `chunk`. A frame is split over writes and notifications at any byte |
| ATT MTU | the device offers 247 and works down to 23 (20 bytes a write) |
| Data length | the device asks for 251 bytes (2120 us) after the connection; the host may refuse, nothing depends on it |
| PHY | 1M. The device does not ask for 2M or coded |
| Connection interval | the host's choice; the device asks for no change |
| Flow, host to device | one request at a time, the next after its answer. The device buffers 8191 bytes, more than one frame (6152), so a request never overruns it; writes without response are taken (faster), the last write of a frame should be one with response. A write with response the device has no room for fails with ATT error 0x11: send it again |
| Flow, device to host | notifications as the stack takes them; an answer that cannot leave within 5 s is dropped |
| Lost bytes | either way the CRC or the missing answer shows it: no answer within the host's timeout, then the same request again. A repeated `show_chunk` whose first copy arrived answers `SEQUENCE` with `next`: go on there. No retransmit cache |
| Both links | USB and Bluetooth are served together; an answer goes where its request came from. One transfer: a `show_begin` on either replaces an open one |
| Quiet connection | dropped after 60 s without a request while no USB host is attached |

### Safety

| Rule | How |
|---|---|
| Bonded hosts only | every request over Bluetooth, `hello` too, is refused `AUTH` unless the link is encrypted with a bonded key of 16 bytes made by passkey entry. An unbonded host reads nothing and draws nothing: RX refuses its writes, and a frame that got in any other way gets only `AUTH` |
| Pairing | LE Secure Connections only, passkey entry (the device shows 6 random digits, new for every window), bonding. No Just Works, no legacy pairing |
| Pairing window | pairing is taken only while the window is open: 120 s, opened by a press of the device's button (while the setting is on) or by `bt { pair: true }` over the cable. Outside it a pairing attempt ends the connection and stores nothing |
| The panel | while the window is open: `BLUETOOTH PAIRING`, the name and the passkey. When it closes (a host paired, the time ran out, `bt { pair: false }`, the setting off): `PAIRED` or `PAIRING ENDED`. Both are full refreshes; the picture before is gone and the next `show` is a full refresh (`why: "first"`) |
| Bonds | up to 3, the oldest replaced. `bt { forget: true }` (cable only) deletes all, drops the connection and closes the window |
| The cable | USB is trusted as before: who holds the cable holds the device |

`bt` keys are applied in the order `forget`, `on`, `pair`; `pair: true` while the setting is off is refused `MODE`.

### Cost

Unmeasured estimates (ESP32-C3 at about 25 mA awake with the radio advertising, 20 mA without; the `trmnl_x` draws
more). Measure before telling anyone a battery life.

| State | Off (today) | On |
|---|---|---|
| USB host attached | awake | awake, advertising every 100 ms: free, the cable feeds it |
| On battery, nobody connected | deep sleep, a look for a USB host every 20 s (about 0.9 s awake) | the same sleep; at every look it advertises for 2 s (every 100 ms, about 20 packets on each of 3 channels), so about 2.3 s awake |
| Average current, estimate | about 0.9 mA | about 2.9 mA: about three times today's, a third of the battery life |
| Finding it | not there | a host must scan or try to connect for up to 22 s |
| Connected on battery | not there | awake (about 25 mA) until the host leaves or 60 s pass without a request |
| Pairing window | not there | awake for up to 120 s, two full refreshes |
