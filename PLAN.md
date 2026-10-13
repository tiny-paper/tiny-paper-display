# Plan

What this project is, what is done and what is still to do. Kept here instead of an issue tracker.

## Scope

| # | What |
|---|---|
| 1 | Firmware: flash, back up, restore, from a static page |
| 2 | Show a picture on the device over the USB cable and over Bluetooth. A Picture page exists (`site/picture.html`); a real picture on a real panel is still to be seen |
| 3 | Packages on npm and PyPI as the API |
| 4 | A sketch example in the browser |
| 5 | Later: a sketch example in Python |
| 6 | Partial refresh, at every bit depth where it has been seen to work on a device |

Anything else is out until asked for.

## Branches and pages

| Branch | Page | Flash installs |
|---|---|---|
| `main` | the site | numbered releases |
| `testing` | the site under `testing/` | test builds, marked as such |

Work lands on `testing` first; `main` moves by promoting a state that was tried on a device.

## State

| Scope | Done | To do |
|---|---|---|
| 1 Flash, backup, restore | a flash page | backup and restore; one page with the tabs Flash and Test, switching to Test after an install; a plain warning that installing erases the firmware on the device |
| 2 USB and Bluetooth | the firmware and its protocol; Bluetooth off by default with a setup step | the first run on a real device, on each board, cable and Bluetooth |
| 3 Packages | a Python package that installs from the git URL; a guide page ("Use it") | the JavaScript package; first releases, version 0.1.0 |
| 4 Sketch in the browser | a sketch page, black and white at 1 bit by default (looked best on the X), optional 16 grays (seen on a device, slower); Clear is a full refresh and leaves the panel clean (seen) | erasing in grays judged; the painting by a drawing library; into the Test tab |
| 5 Sketch in Python | a first version exists | later |
| 6 Partial refresh | in the firmware | seen on a device per board and depth before it is offered there (table below) |

### Partial refresh, what has been seen on a device

| Board | 1 bit | Grays |
|---|---|---|
| TRMNL X | yes: on 2026-10-08 with this firmware over the cable, black strokes crisp and in place (see below) | yes, they appear (see below); slow |
| TRMNL OG | yes, with the firmware this project grew out of | not yet |
| PicPak | not yet | not yet |

On 2026-10-08, on a TRMNL X: the firmware started, and a browser sketch over the cable showed black strokes crisp and in
place as partial updates. Clearing by a partial update left faint traces of the removed strokes (a partial update turns
black back to white only weakly). Since then the Sketch page sends Clear as a full refresh, and erasing as the strongest
region clean the device offers, else a full refresh after a few erasing sends.

Later on 2026-10-08, with a test build on the same TRMNL X: the device reported partial updates at 1, 2 and 4 bit (the
cause of the missing grays was a setting stored by the firmware the device ran before, which the display firmware no
longer reads). Clear as a full refresh leaves the panel clean. A gray stroke is first drawn black and then lightens to
its shade (the gray partial drive goes through black). The grays are even, consistent between strokes and in the right
order (mid looks mid, light looks light). Grays on the TRMNL X are even and in the right order and come out lighter than
the same shade on a computer screen; accepted as the panel's character, not being worked on.

Device result, TRMNL X: 16 grey levels in order on the X by partial and by one full refresh (stock table, build 5c0ec75).

### Known limits (TRMNL X, seen on a device, 2026-10-08)

Compared on the device with the Debug section: the 1 bit partial update looked best (crisp, and the fastest). It is the
Sketch page's default on every board; grays are an option that is slower (each stroke is drawn black first, then fades to
its shade).

| What | Seen |
|---|---|
| 1 bit partial update | about 1.45 s on the panel (1.67 s in all) whatever the area |
| 1 bit full refresh | 1.93 s |
| Gray partial update (a bar, no pre-push) | 2.57 s |
| Gray full refresh | 2.59 s |
| Gray pre-push (light frames) | no effect on pixels that start white, about 0.7 s: the default is now none |

Measured on 2026-10-08 from the device's own log: a 4 bit partial update of a line 3 rows high ran 22 scans of all 1404
rows (12 drive frames in 10 steps, each step followed by one neutral scan) in 2987 ms, about 136 ms a scan, whatever the
height; a window 876 rows high took 4381 ms with the same 22 scans (the gray codes of the window's rows are computed
again for every step); a gray full refresh ran 43 scans in 2542 ms, 59 ms a scan (rows free running, no timed gate
pulse).

Tuning keys for this, all off by default (spec "Tuning"): `step_neutral: end` leaves out the neutral scan between
steps, `row_skip: on` clocks the rows outside the changed ones through without data (`row_skip_us`, `scan_min_ms`).
Host tests model the gate driver and check one clock per row in every scan and the same data on every gate; the
device logs the rows it wrote and skipped next to its clock count. Not seen on a panel yet: whether a skipped scan
places rows right, whether the shorter scans draw as dark, and which combination looks right is still to be recorded.

Measured later on 2026-10-08 on a TRMNL X (1 bit, `row_skip: on`): a 3 row line took 704 ms, 667 ms with
`step_neutral: end` (expected was about 550). Of that a step spent about 100 ms and the pause between steps (`gap=`)
112 to 128 ms on no scan at all, and about 200 ms more lay outside the steps: passes over the two 328 KB frames.
Fewer passes (`passes_partial: 2`, `wave_kick: 1`) gave 565 ms and gray blacks: not the way.

| Since then in the firmware (host-tested, none of it seen on a device yet) | Expected | To look for |
|---|---|---|
| a `panel time` log line per update with the split in ms, `pre=` in the `show` line (spec "Tuning") | the numbers that replace the estimates here | which part is largest |
| `frame_work: rows`: the passes over the frames cover the changed rows only | a 3 row line with default passes, `row_skip: on`: about 250 ms (9 scans of about 20 ms, rails up and down, log lines); `gap=` a few ms | that the panel looks as with `whole` |
| `rails_hold_ms`: rails kept up after a partial update, down by timer, on any failure and before sleep | `up=` of the next update 0 | the picture while the rails are held; whether to keep the key at all (owner: no more panel driving of our own) |
| gray partial: codes from a table per step instead of per pixel; with `frame_work: rows` no whole-frame white per step | the time of a gray partial no longer grows with the window height | grays as before |
| an answer longer than one USB write arrives whole | no cut hello or set answer | the garbled log lines seen once: cause not proven, raw bytes needed if seen again |

`esp/epd_fastepd.cpp` has grown past 1100 lines: to split (rails, time split, gray partial) when the layer's future is decided.

Seen on 2026-10-08: the grays are out of order around levels 5 and 6. With the stock table and a full refresh level 6
is too light against its neighbours; with the FastEPD table and gray partial updates from white level 5 is darker than 4
and 6, and 2 and 3 look the same (their rows in that table are the same). The tuning key `gray_row` replaces single
rows of the table in use for comparing on the device (spec "Tuning", with both tables printed); which rows look
right is still to be recorded, and then become the table.

Idea, not started: drive light grays directly from white instead of through black.

A gray partial update takes at most 262144 pixels in its changed rectangle (hello `partial_max`; memory sums in the spec: about 1.8 MB
free of 8 MB PSRAM after the frames, not measured). The Sketch page names no mode at 2 and 4 bit, so a long stroke is a partial
update or, when too much changed, a full refresh the device chooses (`why: large`); strokes are also split into rectangles that
each fit `partial_max`. The limit itself is a guess kept until the free heap is measured.

The drive options of the X (gray table, pre-push frames, passes, waveform, row timing, clear) are reachable from the Sketch
page's Debug section for comparison, without reflashing; 1 bit looked best, which of the gray settings looks best is still to be recorded.

## Driving the glass: library calls only

Owner decision (2026-10-08): this project stops carrying its own panel driving. The glass is driven only through the
panel library's public calls (FastEPD: `fullUpdate`, `partialUpdate`, `setCustomMatrix`, `setPasses`, `einkPower`), as
the stock firmware does, so keeping up means raising the library version.

Rule: before building any panel mechanism, read the library's public header (`src/FastEPD.h`) and write down here why
its own call does not do. No such note, no mechanism.

First step, in the firmware: the tuning key `driver` (spec "Tuning"), `ours` (default, as before) or `plain`.

| Update under `plain` | What runs |
|---|---|
| full refresh, 1 bit or gray | one `fullUpdate`, gray through `setCustomMatrix` (`gray_table`, `gray_row`), `clear` as its argument |
| 1 bit partial | one `partialUpdate` over the changed rows, `passes_partial` passes; `rails_hold_ms` above 0 is its keep-on |
| gray partial | not offered: hello lists partial updates at 1 bit only |
| a changed area at 2 or 4 bit, a rectangle sent with `full` at any depth | one `fullUpdate` with that rectangle (`why: area`, hello `full_area`): clearing and passes inside it, the rest of the panel sent neutral |

Gray areas go through the library's rectangle full update. Our own gray partial is correct only on white (compared
with epdiy: it runs the table from white without clearing first) and leaves with `ours`.

| `plain` leaves out | |
|---|---|
| our row control (row timing, fixed gate pulse, counters) | the library's own is installed |
| the bus set up again at 26.67 MHz with hardware chip select | the library's 20 MHz, chip select by software |
| kick, main and extra steps, redrive and its bitmap | one call |
| the scan sequencer (`row_skip`, `step_neutral`, `scan_min_ms`), `frame_work` | the library scans every row |
| rails up before the call | the library powers up and down; with a hold time its keep-on, down by our timer policy |

Kept, not panel driving: the pin guard, the expander's alive check, the gauge's reset pin handed back, the zeroed
locals build flag, rails forced down after a failed call, the call in its task on core 1, the gate clock count as a log
line. Switching the key sets the bus up again once, through the library's own IO init.

Not seen on a panel: anything under `plain`. Expected for a small 1 bit partial: about 0.5 to 0.7 s (5 scans of all
rows and the power up and down) against 1.45 s. Read in the library's source (the pinned 8dc8c74 and its head), not
seen: with a row range `partialUpdate` clears only one of its two row buffers, so every second row below the changed
rows would be sent the last changed row again (thin lines under a stroke). The glass decides; if it shows, the choices
are every row named in the call (as the stock firmware) or a fix in the library.

The rectangle full update, read in the same source, not seen. Expected time about 1.7 s whatever the size (27 scans
of all rows with the fast clearing, 9 passes of the stock table; 43 scans, about 2.6 s, with `clear: slow`). x and
width need no alignment in the library (it masks to the pixel); ours are multiples of 8, height at least 2. To look
for on glass:

| Where | What | Why it could happen |
|---|---|---|
| the rectangle's place | the flash and the picture at the mirror position left to right | the library masks the row data by position and the X sends rows last pixel first; we hand it the mirrored x. If that reading is wrong, the place is |
| left and right of the rectangle, in its rows | every second row a little lighter or darker than its neighbours | in the 4 bit passes the mask is put on the first of the library's two row buffers only, so every other row is sent the picture's codes across the whole width, without clearing; the frame holds the true picture there, so black and white should not move, mid grays may |
| above and below | nothing | those rows are sent neutral |
| the rectangle's edges | a clean edge, no flash outside | the clearing scans use the same mask on every row |

Not built, with the reason: `fastUpdate` (drives every pixel of the panel, one flash: not a partial), the library's
2 bit partial (our gray frames are 4 bit; needs its 2 bit frames end to end), `startVideo` (a thread that scans without
pause from a count per pixel, rails on throughout, 2.6 MB of heap on the X: fits a sketch in idea, to try only after
`plain` was seen).

The pin: see "Upstream".

The removal of `ours` and of its keys waits for a look at the glass: `plain` and `ours` side by side from the Debug
section, 1 bit strokes, erasing, a gray picture.

## Upstream

Rule: upstream issues and pull requests are read before any panel work and at least weekly.

| Library | Our pin | Upstream head | The stock firmware's X build pins | Checked |
|---|---|---|---|---|
| tiny-paper/FastEPD (fork of bitbank2/FastEPD) | eb10f7e (branch all-fixes-edge-head: upstream 3d2a63a plus our fixes, setVCOM) | 3d2a63a (2026-10-03) | 9a84c35 (2026-09-22) | 2026-10-13 |

The pin is a full commit of the fork (the same as the main project's, copied 2026-10-13); the lock files name it.
Rule: the pin and MIRROR_X move in one commit (esp/epd_fastepd.cpp): this fork commit has no mirrored rectangle fix, so the
glue mirrors a rectangle's x. `firmware/tools/check_fastepd_source.py` checks pin, lock, compiled library and glue in
the build (`firmware/tools/idf/build.sh`); X_EDGE variants must stay off.

| Open upstream, touches the X | State | Us |
|---|---|---|
| issue 42: a strip of about 8 columns at the right edge never written (one extra clock per transfer of the LCD peripheral while the hardware chip select is active shifts the row by one bus word) | open, no fix, seen by its reporter on two devices with 9a84c35 | both drivers write all 1872 columns into the frames and send rows through the same `esp_lcd` call. `ours` uses the hardware chip select, so by the issue's reasoning it is exposed. `plain` at our pin uses the chip select by software, which is not the case the issue describes: unknown. Neither seen on glass: show a black bar at the right edge under both |
| row range in `bbepPartialUpdate` (see "Driving the glass") | found by reading, in our pin and the head; not reported upstream by anyone | `plain` passes the range. Not to be reported before a photo from a device and the owner's word |
| issue 35, pull request 34 (ESP-IDF build and SPI path) | open | not our panel type; 34 touches the ESP-IDF build, to read when the pin moves |

## Clean-up

The firmware folder still carries code from the project it grew out of that the display firmware does not use
(picture decoders other than PNG, touch, update over the air, headers of plugins, albums and stores). To remove:
everything that no board's build links.

## Out for now

| What | Why |
|---|---|
| The film example and the speed test tool | not in the scope above |
| An example that renders TRMNL plugins locally | later, if asked |
| Flashing from the Python package | the flash page does it |

## Not proven yet

Seen on a real panel so far, on the TRMNL X (2026-10-08): the firmware starts, a sketch over the cable works with black
strokes as partial updates, the grays appear and are even, Clear as a full refresh leaves the panel clean. Everything else
has not been seen on a real panel. To check on each board (TRMNL X, TRMNL OG, PicPak): a picture shows over the cable and
over Bluetooth, a partial update shows without a flash at each depth (on the X: that erasing in grays leaves no traces),
sketching on the other boards, backup, flash, restore, and going back to the device's own firmware.
