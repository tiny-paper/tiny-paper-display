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
