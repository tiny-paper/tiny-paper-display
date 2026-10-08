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
| 3 Packages | a Python package | the JavaScript package; first releases, version 0.1.0 |
| 4 Sketch in the browser | a sketch page, black and white | the device's grays; the painting by a drawing library; into the Test tab |
| 5 Sketch in Python | a first version exists | later |
| 6 Partial refresh | in the firmware | seen on a device per board and depth before it is offered there (table below) |

### Partial refresh, what has been seen on a device

| Board | 1 bit | Grays |
|---|---|---|
| TRMNL X | yes, with the firmware this project grew out of | not yet: on in test builds so it can be looked at |
| TRMNL OG | yes, with the firmware this project grew out of | not yet |
| PicPak | not yet | not yet |

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

Nothing here has been seen on a real panel. To check on each board (TRMNL X, TRMNL OG, PicPak): the firmware starts,
a picture shows over the cable and over Bluetooth, a partial update shows without a flash at each depth, sketching,
backup, flash, restore, and going back to the device's own firmware.
