# Plan

What this project is, what is done and what is still to do. Kept here instead of an issue tracker.

## Scope

| # | What |
|---|---|
| 1 | Firmware: flash, back up, restore, from a static page |
| 2 | Show a picture on the device over the USB cable and over Bluetooth |
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
| 4 Sketch in the browser | a sketch page with 16 grays on the TRMNL X (seen on a device); Clear is a full refresh and leaves the panel clean (seen) | erasing in grays judged; the painting by a drawing library; into the Test tab |
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

### Known limits (TRMNL X, seen on a device, 2026-10-08)

| What | Seen |
|---|---|
| A black partial update (1 bit) | 1465 ms on the panel, 1673 ms in all, the same for any size of area |
| A mid gray partial update | 3219 ms on the panel, 3573 ms in all |

Ideas, not started: skip quickly over the rows outside the changed area (the time does not depend on the area today);
drive light grays directly from white instead of through black.

The drive options of the X (gray table, pre-push frames, passes, waveform, row timing, clear) are reachable from the Sketch
page's Debug section for comparison, without reflashing; which setting looked best on glass is still to be recorded.

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
