# USB display protocol, version 1

The byte-level reference of the display firmware: a board as a plain display driven over its USB cable (and over
Bluetooth). It is for people writing their own client. To use the display, read [the guide](../site/api.html).

Commands: `hello`, `have`, `show` (a whole picture or a rectangle, with a refresh mode), `clear`, `set`, `bt`.
Vectors: `vectors/usb-display.json` (`gen_usb_display.py`), replayed by the firmware's host test and by
`python/tests/test_display_fw.py`. The corpus also has codec-only fault replies (`refusals`) for IO and NOMEM.
Code: `firmware/components/usbdisp`.

## Frames

USB CDC byte stream, any baud rate.

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
| 0x01 | hello | any map (the app's `{ proto: 1, app: tstr }` is fine) | `{ proto: 1, project: "display", fw: tstr, board: tstr, w: uint, h: uint, depths: [uint], colors?: [tstr], max_payload: 6144, max_png: uint, chunk: 4096, partial_depths: [uint], modes: [tstr], depth_modes: map, full_every: uint, full_every_set: bool, clean_every: uint, clean_every_set: bool, fingerprint_hash: "fnv1a64", fingerprint?: tstr, bt?: tstr, partial_max?: map, tuning?: map, full_area?: [uint] }` (`partial_max`, `full_area`: "Area and refresh mode"; `tuning`: "Tuning") |
| 0x08 | show_begin | `{ size: uint, x?: uint, y?: uint, mode?: tstr, passes?: uint }`: the PNG's bytes, where it goes and how ("Area and refresh mode") | `{ chunk: 4096, next: 0 }`; replaces an open transfer (which counts as cut) |
| 0x09 | show_chunk | `{ off: uint, data: bstr }` | `{ next: uint }`. `off` must be `next`; the first chunk holds at least the PNG's first 29 bytes and is checked at once (size, depth) |
| 0x0A | show_end | `{}` | `{ depth: uint, ms: uint, mode: tstr, why: tstr, x: uint, y: uint, w: uint, h: uint, passes?: uint, ckv?: map, warn?: tstr, same?: true, steps: map }` after the refresh ("The answer") |
| 0x0B | clear | `{}` | as show_end: the panel white, `mode: "full"`, `why: "asked"` |
| 0x0C | set | `{ full_every?: uint or null, clean_every?: uint or null, tuning?: map or "defaults" }` `full_every`, `clean_every` 0 (never) .. 250, null: remove the stored value, the default (0 and 15) applies again; "Tuning"; without a key nothing changes | `{ full_every: uint, full_every_set: bool, clean_every: uint, clean_every_set: bool, tuning?: map, refused?: map }` `full_every`, `clean_every` as in use (the `_set` flags as in hello), each stored on the device under its own key, kept over power-off; the others under "Tuning" |
| 0x0D | bt | `{ on?: bool, pair?: bool, forget?: bool }`, over the USB cable only ("Bluetooth") | `{ on: bool, pairing: uint, name: tstr }`: the setting, the seconds the pairing window stays open (0: closed), the advertised name |
| 0x0E | have | `{ fingerprint: tstr }` | `{ same: true }` when known and equal, else `{ send: true }`; no PNG bytes, refresh or counter change |

`project: "display"` tells this firmware from the app, whose hello has no `project` and which answers 0x08..0x0B with
`UNKNOWN_CMD`. `board`: `trmnl_og`, `picpak`, `trmnl_x`. `depths`: the gray bit depths taken. `colors`
(`["black","white","red","yellow"]`, PicPak): the panel's own colors. `warn`: the driver reported a refresh in less
time than the panel needs (SPI panels: under 300 ms): look at the panel. `partial_depths`: the depths the board
refreshes partially now (empty: none; the tuning key `driver` takes the gray depths away while it is `plain`). `modes`: the modes it does at some depth. `depth_modes`: `{ "1": [tstr], "2": [tstr], "4": [tstr] }`, a key for each of `depths` (text keys), the modes a `show` of a whole picture at that depth is accepted with, from the same check that refuses the others with MODE 36 (a TRMNL X at 2 and 4 bit lists `full` and `partial` only, `partial_all`, `inverse` and `passes` need a 1 bit picture; a firmware from before this field lacks it: try the mode and read the refusal). `full_every`: the setting in use (default 0, never); `full_every_set`: true when it is a stored value, false while the default applies (`set` with `full_every: null` makes it false again). `clean_every`, `clean_every_set`: the same for the clean of the changed area (default 15; "Clean of the changed area"). `bt`: `"off"` or
`"on"`, the Bluetooth setting; absent on a build without Bluetooth, which answers `bt` with `UNKNOWN_CMD`.

Example (vector session `gray`, a 16 x 8 test panel, partial at 1 bit):

| | Bytes |
|---|---|
| hello | `a55a 1400 01 01 a26570726f746f016361707067766563746f7273 2e7a` |
| answer | `a55a 1c01 01 81 b06570726f746f016770726f6a65637467646973706c617962667769302e302e302b76656365626f617264687665635f67726179617710616808666465707468738201026b6d61785f7061796c6f6164191800676d61785f706e67191028656368756e6b1910006e7061727469616c5f6465707468738101656d6f646573856466756c6c677061727469616c6b7061727469616c5f616c6c67696e7665727365667061737365736b64657074685f6d6f646573a26131856466756c6c677061727469616c6b7061727469616c5f616c6c67696e7665727365667061737365736132816466756c6c6a66756c6c5f6576657279146e66756c6c5f65766572795f736574f47066696e6765727072696e745f6861736867666e7631613634 9050` |
| show_begin, 86 bytes | `a55a 0800 31 08 a16473697a651856 e617` -> `a55a 1000 31 88 a2656368756e6b191000646e65787400 2322` |
| show_end, first picture | `a55a 0100 05 0a a0 8c0a` -> `a55a 7200 05 8a a9 65646570746801 626d7300 646d6f64656466756c6c 63776879656669727374 617800 617900 617710 616808 657374657073 a8 6472656376 00 6470726570 00 67696e666c617465 00 67636f6e76657274 00 6464696666 00 6b66696e6765727072696e74 00 6570616e656c 00 65746f74616c 00 d9ba` (full, first) |
| show_end, a corner changed | `a55a 0100 33 0a a0 897d` -> `a55a 7700 33 8a a9 65646570746801 626d7300 646d6f6465677061727469616c 63776879676368616e676564 617808 617902 617708 616802 657374657073 a8 6472656376 00 6470726570 00 67696e666c617465 00 67636f6e76657274 00 6464696666 00 6b66696e6765727072696e74 00 6570616e656c 00 65746f74616c 00 dac5` (partial, changed, 8,2 8x2) |
| show_begin, 8 x 4 at 0,4, inverse | `a55a 1b00 3d 08 a4 6473697a651847 617800 617904 646d6f646567696e7665727365 08b2` |
| clear | `a55a 0100 0b 0b a0 bc22` -> `a55a 2c00 0b 8b a8 65646570746801 626d7300 646d6f64656466756c6c 637768796561736b6564 617800 617900 617710 616808 8a24` |
| set | `a55a 0d00 76 0c a16a66756c6c5f657665727902 25e7` -> `a55a 1d00 76 8c a26a66756c6c5f6576657279 02 6e66756c6c5f65766572795f736574 f5 60ac` |

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

`hello.full_area` (the TRMNL X while the tuning key `driver` is `plain`; absent: none) lists the depths at which the
device refreshes one rectangle fully, alone: the clearing flash and the full waveform inside the rectangle, the rest of
the panel left as it is, so the grays come out right whatever was there. The frame is kept at those depths too, and a
rectangle may have such a depth. Then, when the frame kept is known:

| Sent | Runs | Answer |
|---|---|---|
| a picture or rectangle without a mode, at a depth in `full_area` and not in `partial_depths` | one full refresh of the changed rectangle (at least 2 rows) | `mode: "full"`, `why: "area"`, the area the rectangle |
| a rectangle with `full`, at a depth in `full_area` | one full refresh of that rectangle | the same |
| a whole picture with `full`; any picture when the frame kept is not known; the refresh after `full_every` | the whole panel, as without `full_area` | |

A rectangle's full refresh counts towards `full_every` as a partial update does. It takes as long as a full refresh
with the same clearing (every scan covers all rows): the clearing is `fast` unless the key `clear` says `none` or `slow`.

A gray partial update (2 or 4 bit; `partial_depths` lists them) keeps copies of old and new pixels of its rectangle in RAM, so
its changed rectangle may have at most 262144 pixels on the TRMNL X (512 x 512, about a tenth of the 1872 x 1404 panel; 256
KiB of copies at 4 bit). The device finds the rectangle before it touches the panel. With no mode named, a larger change is a
full refresh (`why: "large"`); with `partial` named it is refused `MODE` ("a partial update at 4 bit takes at most 262144
changed pixels in its rectangle, this one has N: use full"). `hello.partial_max` gives it per gray depth (`{ "2": 262144, "4": 262144 }`; no key: no limit), so a host splits a large change into
several rectangles that each fit. The limit is the area of the rectangle, because the copies are a window: the memory a gray
partial of N pixels needs is N bytes (old and new at 4 bit, N / 2 each) plus the frame kept and the new frame (1.3 MB each at
4 bit), FastEPD's two buffers (1.3 MB each), the fresh bitmap (0.66 MB) and a scratch buffer (0.33 MB): about 6.2 MB of the
X's 8 MB PSRAM before the windows, so roughly 1.8 MB less what the system uses, in one contiguous block. 262144 is 14 % of
that: strokes up to 176 x 840 (148,000 pixels) are seen working on a panel; more has not been tried. A 1 bit partial has no such limit. A photo (a whole dithered
picture) is therefore a full refresh, whatever the frame kept.

The frame kept is forgotten, so the next update is a full refresh whatever was asked (a rectangle then has white
around it): at power-on, after a refused step of a show, a transfer cut (quiet for 10 s, or replaced by a new
show_begin), a panel failure, and after a picture at a depth outside `partial_depths`. `clear` leaves a white 1 bit
frame. A whole picture without a mode gives up the frame kept when memory is short (`why: "memory"`).

### The answer

| Key | Value |
|---|---|
| `mode` | what ran: `full`, `partial`, `partial_all`, `inverse`, `passes`, or `none` (the picture is what the panel shows: nothing driven) |
| `why` | `asked` (the mode named), `changed`, `unchanged`, or for a full refresh nobody named: `first`, `refused`, `cut`, `failed`, `memory` (why the frame kept was not known), `depth` (another depth than the picture shown, or one outside `partial_depths`), `board` (no partial refresh), `cleanup` (`full_every` reached), `large` (too much changed for a gray partial update: the whole panel was refreshed), `area` (a full refresh of the answer's rectangle alone, `hello.full_area`) |
| `x`, `y`, `w`, `h` | the area driven (all 0 with `none`) |
| `same` | true with `mode: "none"` |
| `passes` | with the mode `passes` |
| `clean` | when a clean ran after this update: `{ x, y, w, h, mode, ms }`, the area cleaned, the mode used and its time ("Clean of the changed area") |
| `ckv` | TRMNL X: `{ edges: uint, frames: uint, per: uint, rows: uint, ok: bool }`, the gate clock edges the firmware counted over the last refresh of the update, the drive frames, edges a frame, the panel's rows; `ok` false when a frame did not clock once per row (the picture is then drawn at the wrong height). Log: `# display W show ... WRONG` |

`steps` (a `show_end` answer, not `clear`) is the device's own time for each step, in whole milliseconds on the board's
monotonic clock. The steps are disjoint and run one after the other, so their sum is at most `total`:

| Key | Time |
|---|---|
| `recv` | first `show_chunk` arriving to the last one (the bytes' transfer without the first chunk's own flight; 0 for a one-chunk PNG) |
| `prep` | from `show_end` to the start of the decoding: the header check, the frame's allocation, the copy of the frame kept (a rectangle) |
| `inflate` | the PNG decoder minus `convert`: zlib inflate and the row unfilter |
| `convert` | each decoded row into the panel's packed format (all rows) |
| `diff` | the search for the changed rectangle (a partial update only, else 0) |
| `fingerprint` | the hash of the new frame |
| `panel` | the panel calls (the same as `ms`) |
| `clean` | the clean of the changed area after the update (0: none ran); not in `panel` |
| `total` | first `show_chunk` to the answer being built; what is not in a step (the wait between the last chunk and `show_end`, mode choice, freeing) is the rest |

The device log line `show ...` ends with `steps=recv/prep/inflate/convert/diff/fingerprint/panel/total`.

`full_every` (default 0: never, `set`): the count of partial updates since the last full refresh; a named mode is run as
named and counts, except an unchanged `partial` which uses no count.

### Clean of the changed area

A 1 bit partial update turns black back to white only weakly, so light ghosting builds up in a page that redraws every few
seconds. Instead of a flash of the whole screen the device cleans only the area that changed. `clean_every` (`set`, 0..250,
0 = off, default 15): the device keeps the union rectangle of all partial updates since the last clean or full refresh.
After `clean_every` partial updates it drives that rectangle once with the strongest area mode the depth offers
(order: `inverse`, `passes` twice, `partial_all`; from `depth_modes`), redrawing the frame kept there, then starts a new
union. The answer of that `show_end` carries `clean: { x, y, w, h, mode, ms }`, and `steps.clean` its time.

| Rule | |
|---|---|
| Counting | each update that drove a partial refresh counts and adds its rectangle; a full refresh of the whole panel resets the count and the union; a rectangle's full refresh (`why: "area"`) counts like a partial one |
| Both due | `full_every` reached: the next update is the full refresh and no clean runs; the full resets both |
| Mode | at 1 bit `inverse` (every pixel goes through its opposite and back, so black turns white fully; `passes` leaves a pixel driven twice towards the value it already has). A depth with none of the modes (the TRMNL X at 2 and 4 bit lists `full` and `partial` only) is not cleaned: it is left to `full_every` |
| No memory, panel refusal | no memory for the negative: no clean now, tried again at the next update; a panel failure: refused `IO` and the frame kept is forgotten, as for any update |
| Default 15 | a pc_stats-style page at 1 bit shows the first light ghost after about 10 to 20 partial updates; 15 cleans before it is seen, and the clean (one area, about a partial update twice) costs far less than the full flash. This is an estimate, not measured on the glass |

## Tuning

For comparing how the panel is driven without reflashing. Only the TRMNL X has knobs: `hello.tuning` is a map of every
key to its value now (a number, or a word where the key has words); other boards have no `tuning` in hello and refuse every
key. `set { tuning: { key: value, ... } }` applies each valid key from the next refresh; it is **not stored** (the values
last until the device restarts). A value is a whole number or one of the words (`true` and `false` for the on/off keys).
Each key is checked on its own: the ones that are fine apply, every other one is answered under `refused` as `{ key: why;
allowed range }` (an unknown key lists the known ones; at most 8 are listed). `set { tuning: "defaults" }` restores every key
to its default. The answer of every `set` carries `tuning` (the values now) on a board that has it. The knobs are those of
the driver's `!tp` console commands, with the bounds those commands check.

| Key | Allowed | Default | What it does |
|---|---|---|---|
| `gray_table` | `fastepd`, `stock` | `stock` | the 16 gray table of a full refresh and of a gray partial update: FastEPD's 8 passes, or the stock firmware's 9 (both printed below) |
| `gray_partial` | `off`, `on` | `on` | gray partial updates; `off` refuses a partial update at 2 and 4 bit (hello's `partial_depths` is read at start and does not change) |
| `gray_dark` | 0 to 14 | 0 | frames that push the changed pixels dark before the table runs |
| `gray_light` | 0 to 14 | 0 | frames that push the changed pixels light before the table runs (after `gray_dark`); on the X they do nothing for pixels that start white and cost about 0.7 s a frame group |
| `passes_partial` | 1 to 14 | 4 | FastEPD passes of a 1 bit partial update: more is darker, less ghosting of the old picture, slower |
| `passes_full` | 1 to 14 | 3 | FastEPD passes of a full refresh |
| `wave_kick` | 0 to 14 | 2 | frames of the 1 bit partial waveform that run before the passes |
| `wave_extra` | 0 to 14 | 0 | black-only frames after the passes of a 1 bit partial |
| `wave_redrive` | 0 to 3 | 2 | how often a later partial pushes a black pixel black again (0 off) |
| `row_settle` | 0 to 20 | 2 | microseconds each row waits before its clock |
| `row_hold` | 0 to 20 | 2 | microseconds each row holds after |
| `row_drive` | 0 to 200 | 32 | microseconds of each partial row's drive (0: free running) |
| `clear` | `auto`, `none`, `fast`, `slow` | `auto` | the clearing passes before a full refresh (auto: slow every 8th, else fast) |
| `gray_row` | text: `level:passes` entries | none | replaces the drive sequence of single gray levels in the table in use, for a full refresh and a gray partial update alike. `level` 0 (black) to 15 (white); `passes` one character a pass, `.` no drive, `D` dark, `L` light, exactly as many as the table has (`stock` 9, `fastepd` 8). Several entries separated by spaces or commas, or the key sent again: the rows add up. An empty text, `"defaults"` and a change of `gray_table` put every row back; a `gray_table` in the same request is applied first. Any bad entry refuses the whole text, in words (`level 5: 8 passes given, the stock table has 9`). `hello.tuning.gray_row` is the rows that differ from the table, as the same text (empty: none) |
| `step_neutral` | `each`, `end` | `each` | the scan with every source output neutral that ends a step's drive: after each step of a partial update (a 4 bit one has about 10 steps, a 1 bit one 2), or only after the last; `end` saves one scan a step. Watch for: grays darker or uneven on a large area (the pause between steps, `gap=` in the log, lengthens the frame before it), ghosting |
| `row_skip` | `off`, `on` | `off` | rows outside the changed rows get one short gate clock and no data instead of a timed row, the source outputs latched neutral; one row above and two below the changed rows stay timed. Needs `row_drive` above 0; a change within 2 rows of the last row is not skipped. Watch for: a stroke shifted, doubled or stretched, a faint band above or below it, weaker blacks or lighter grays (a scan is much shorter, so each frame may push less) |
| `row_skip_us` | 2 to 50 | 10 | microseconds the gate clock is high for a skipped row (1 low before it). Watch for, when lowered: rows shifted up (clocks the gate driver missed) |
| `scan_min_ms` | 0 to 150 | 0 | with `row_skip`: a scan lasts at least this long (it waits after its last row); about 120 gives every frame the time it has without skipping. Tells a weak drive (cured by it) from a misplaced row (not cured) |
| `frame_work` | `whole`, `rows` | `whole` | the passes over the two 328 KB frames around the scans of a 1 bit partial update (copies, the search for new blacks, the codes of each step, the redrive counts, setting the frames white) cover whole frames, or only the changed rows (with `wave_redrive` above 0 joined with the rows of the blacks still to be driven again). The library is handed the same frames either way (host-tested byte for byte), so the panel shows the same; `rows` takes the pause between steps (`gap=`) from about 110 ms to a few. With `rows` the fingerprints of the `panel given` line cover the worked rows only. Watch for: anything that differs from `whole` |
| `rails_hold_ms` | 0 to 10000 | 0 | milliseconds the panel's supply rails stay up after a partial update, so the next one inside that time skips the power up (`up=` in the `panel time` line shows what that is worth). 0: down at the end of every update. They come down when the time passes, at once when an update fails or the key is set to 0, before a full refresh, a restart and any sleep. Watch for: the picture fading or graying while the rails are held (nothing is driven then, but the supply is on), and battery use |
| `driver` | `ours`, `plain` | `ours` | who drives the glass. `ours`: this firmware's own row control, bus setup, waveform steps and scans around the panel library; every other key applies. `plain`: the panel library's public calls only, as the stock firmware uses them: a full refresh is one `fullUpdate` (grays through the table of `gray_table` and `gray_row`), a 1 bit partial update is one `partialUpdate` over the changed rows, with the library's own row control, bus clock, timing and power up and down. No gray partial update: `partial_depths` in hello is `[1]` while it lasts and `partial` named at 2 or 4 bit is refused; a changed gray area is one `fullUpdate` with its rectangle (`full_area` in hello is `[1, 2, 4]`, `why: "area"`, "Area and refresh mode"). Of the other keys only `gray_table`, `gray_row`, `passes_partial`, `passes_full`, `clear` and `rails_hold_ms` (the library's own keep-on) do anything then; the stock firmware runs 3 and 3 passes. Watch for: thin lines on every second row below a stroke; a rectangle's full refresh landing mirrored left to right, or changing every second row left and right of the rectangle (all three read in the library's source, not seen); blacks lighter than under `ours` (no frames before the passes, no redrive), traces of erased strokes, the time (`ms`) |

`step_neutral` to `scan_min_ms` are the scans of a partial update. With their defaults the scans are the library's own: every step is its
drive frames and then one neutral scan, every scan writes all 1404 rows with the timed row step. The gate driver gets one
clock per row in every scan whatever the keys say; the device counts the clock edges in hardware and the rows as it
drives them, and logs both after each partial update: `panel scan neutral=<each|end> skip=<off|on/<us>us> frames=<scans>
careful=<rows written> skipped=<rows clocked through> left=<neutral scans left out> gap=<longest pause between steps>
<ok|BAD> per=<clock edges a scan, 1408>`; `careful + skipped` is `frames` times 1404, else `BAD` and a `W` line.
With `row_skip` an update starts with one more scan that writes three neutral rows and skips the rest, so the source
drivers hold neutral before any row is skipped.

The time of every update on the TRMNL X, split, is the last of its log lines: `panel time total=<ms> begin=<ms>
write=<ms> up=<ms> prep=<ms> masks=<ms> lib=<ms> scans=<ms> down=<ms> after=<ms> save=<ms> log=<ms> other=<ms>
calls=<n>`. The update's own line (`show ...`) has `ms`, the time on the panel, and `pre`, the time before it.
The lines `panel full`, `panel partial` and `panel gpart` of an update say `driver=ours` or `driver=plain`. Under
`plain` the one library call is not split: `lib` holds its power up, scans and power down, `calls` is 1, and a line
`panel rows driver=plain ckv=<gate clock edges> scans=<scans the call makes> rows=1404` replaces the row timing (a `W`
line follows when the edges are not one per row and scan; the answer carries no `ckv` then).

| Part | What |
|---|---|
| `pre` | before the panel: the frame, the picture decoded into it, its fingerprint, the changed rectangle |
| `begin`, `write` | the driver's start (frames set white) and the rows copied in |
| `up`, `down` | panel rails up, until the supply reports good, and down; a full refresh powers up inside `lib` |
| `prep` | once an update, before the first scan: frame copies, the search for new blacks, fingerprints |
| `masks` | per step: the codes of the step for every pixel |
| `lib` | inside the library before the first scan of each call (`calls`): its codes per row, its task |
| `scans` | the scans themselves; the only part the scan keys shorten |
| `after`, `save` | redrive counts and the frames kept; the redrive file (0: kept in memory) |
| `log`, `other` | writing the update's other log lines; what no part covers |

The two gray tables, one row a level, one character a pass in the order driven (`.` no drive, `D` dark, `L` light). A
full refresh runs a level's row on every pixel of that level after the clearing passes have left the panel white; a gray
partial update runs it on the changed pixels from whatever they show. The rows are tuned by hand. A refresh drawn with
replaced rows says so in the device log: `gray=stock rows*=5,6`.

| Level | `stock` | `fastepd` |
|---|---|---|
| 0 | `......DDD` | `LLDDDDDD` |
| 1 | `..DDDLLDD` | `LLLLDDLD` |
| 2 | `....DLLDD` | `LLLDDDDL` |
| 3 | `DDLLDDDDL` | `LLLDDDDL` |
| 4 | `...DLDDDL` | `LLLLDDDL` |
| 5 | `DLLLLDDDL` | `LLLLDLLD` |
| 6 | `..DDLLDDL` | `LLDDDLDL` |
| 7 | `.DDLDDLDL` | `LLLDDLDL` |
| 8 | `.DDDLDLDL` | `DDDDDDLL` |
| 9 | `.DDDDLLDL` | `LDDDDDLL` |
| 10 | `DDDLDDDLL` | `LLDDDDLL` |
| 11 | `..DLDDDLL` | `LLLDDDLL` |
| 12 | `...DLDDLL` | `LDDLDDLL` |
| 13 | `....DLDLL` | `LLLLDDLL` |
| 14 | `.DDDLLLLL` | `LLLLLDLL` |
| 15 | `........L` | `LLLLLLLL` |

There is no key to skip a 1 bit black step for gray targets: a 4 bit rectangle goes straight to the gray partial, which
drives only the pixels that changed, so there is no such step.

## Refusals

`{ err: uint, msg: tstr (<= 120 bytes), detail?: map }`. A refusal never changes the panel.

| err | Name | When |
|---|---|---|
| 1 | UNKNOWN_CMD | any other cmd 0x01..0x3F |
| 2 | BAD_REQUEST | payload not one CBOR map, a key missing or of the wrong type, a first chunk under 29 bytes, `passes` without the mode `passes` or outside 1..4, `full_every` or `clean_every` over 250 |
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
| `full_every` | 0..250, default 0 |
| `clean_every` | 0..250, default 15 |
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
(kept across restarts; `!tp gpart on` back); there is no `set` key for it. The X keeps its redrive bitmap in RAM only here (`hal_panel_fresh_ram`):
no flash file is written per partial update.

## Bluetooth

The same firmware also takes the same frames over Bluetooth Low Energy. Off by default.
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
