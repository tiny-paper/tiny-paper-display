# Plan

What is open now. Kept here instead of an issue tracker. First release: the TRMNL X only; the OG and PicPak code stays in
the tree, not built or released.

## Branches and pages

| Branch | Page | Flash installs |
|---|---|---|
| `main` | the site | numbered releases (X image) |
| `testing` | the site under `testing/` | test builds, marked as such |

Work lands on `testing` first; `main` moves by promoting a state that was tried on a device.

## Open

| Area | To do |
|---|---|
| Flash page | backup and restore tried on a device; one page with the tabs Flash and Test; a plain warning that installing erases the firmware |
| Bluetooth | see the plain link working on a device, then the key (below); not yet tried on a device |
| Packages | the JavaScript package; first releases, version 0.1.0 (Python installs from the git URL) |
| Sketch | erasing in grays judged; the painting by a drawing library; into the Test tab; later a Python sketch |
| Picture page | a real picture on a real panel is still to be seen |
| Partial refresh | gray order around levels 5 and 6 (`gray_row` rows to record, then become the table); the tool refuses an 8 bit PNG for `--at` while it converts one for a full picture |
| Driving the glass | look at `plain` and `ours` side by side (Debug section), then remove `ours` and its keys |
| Clean-up | firmware code the display firmware does not use (decoders other than PNG, touch, update over the air, albums); remove what no board's build links |
| Not proven | on the X: Bluetooth, backup, flash, restore, going back to TRMNL's firmware. OG and PicPak: everything |

## Decisions

### 2026-10-14: Bluetooth trust by our own key, set up over the cable

| Item | Decision |
|---|---|
| Why | system pairing needs a passkey typed into the computer: awkward on a headless PC and in Docker |
| Set up | over the USB cable only: device and host agree a random key; the host keeps it in a small file |
| Use | every Bluetooth frame is authenticated and encrypted with that key (AES-CCM), shared test vectors; no system pairing |
| Kept | stored with the other settings: survives a program update, not a fresh install; `forget` over the cable replaces it |
| Built | spec "Bluetooth key", `bt_key`, handshake and frames, vectors |

### 2026-10-08: driving the glass through library calls only

| Item | Decision |
|---|---|
| Rule | the panel is driven only through FastEPD's public calls; before building any panel mechanism read `src/FastEPD.h` and write down here why its own call does not do |
| Tuning key `driver` | `ours` (default) or `plain` (library calls); removal of `ours` waits for a look at the glass |
| Gray areas | the library's rectangle full update (about 1.7 s whatever the size); our gray partial is correct only on white |
| Not built | `fastUpdate`, the library's 2 bit partial, `startVideo` (try only after `plain` was seen) |

To look for on glass under `plain` and the rectangle full update: the rectangle's place (mirrored x), every second row lighter
or darker beside it, nothing above or below, a clean edge.

## Seen on a device (TRMNL X)

| What | Result |
|---|---|
| Cable, 2026-10-14, build 010272a | full 1 bit picture 1.9 s panel, 5.7 s whole; 4 bit wedge in 16 steps in order; partial rectangles at 4 and 1 bit in place; clear 1.1 s; a partial at an x not a multiple of 8 is refused |
| Release build t20261014.1 (710f84f), cable | flashed from the GitHub release, reports `0.1.0+20261014.1620.710f84f`; test page: every mode at 1 bit, full and partial at 2 and 4 bit, no refusal; a small change in a whole page refreshes only its box (440x96, 1.8 s) |
| Long runs, 2026-10-14 | PC stats every 2 s: 437 updates in 14 min, no failure; Bitcoin ticker over WebSocket: partial updates 1.9 s for 30 min |
| Partial 1 bit | crisp, about 1.45 s whatever the area; full 1.93 s |
| Partial and full grays | partial 2.57 s, full 2.59 s; a gray stroke is drawn black first, then lightens; grays even and in order, lighter than on a screen (accepted) |
| Clear | a full refresh leaves the panel clean |
| Not seen | OG, PicPak; the tuning keys (`row_skip`, `step_neutral`, `frame_work`, `rails_hold_ms`) |

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

## Out for now

| What | Why |
|---|---|
| The film example and the speed test tool | not in the scope |
| An example that renders TRMNL plugins locally | later, if asked |
| Flashing from the Python package | the flash page does it |
