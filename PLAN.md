# Plan

What was decided and what is still to do. Kept here, in the repository, instead of an issue tracker.

## Decided

| Date | Decision |
|---|---|
| 2026-10-07 | A firmware of its own for the boards we can test: TRMNL X, TRMNL OG, PicPak. Driven only over the USB cable: no WiFi, no account. One job: show the picture it is given |
| 2026-10-07 | One name for project, packages and commands: tiny-paper display, `tiny-paper-display` |
| 2026-10-07 | Partial refresh is the default where the panel can do it. The same picture is not sent or drawn twice while the device stays connected |
| 2026-10-08 | Bluetooth Low Energy as a second way to send pictures: same firmware, off by default, switched on in a setup step after flashing, bonded devices only |
| 2026-10-08 | The standard of what a device accepts is what TRMNL renders for that model: such a picture is uploadable as it is |
| 2026-10-08 | The device says what it supports and a host can ask: depths, colours, picture kinds, refresh modes |
| 2026-10-08 | Refresh is auto by default; the user may insist on partial or full. Every call reports what was asked, what was done and why. A board that cannot do something says so in words |
| 2026-10-08 | Converting a picture (resize, fit, dither) is an optional add-on and the user's choice per call: auto, never (sent untouched, or refused before sending), always, or the user's own function |
| 2026-10-08 | Libraries in Python and JavaScript. The example app is a sketch app that draws with the palette the connected device has; in the browser the painting itself is done by a JavaScript drawing library |
| 2026-10-08 | The site is static, with no account. One page, two tabs: Flash (with backup and restore) and Test (sketching). After a successful install it switches to Test |
| 2026-10-08 | This repository is the project's home, independent of the tiny-paper main project. Code both need is copied by hand |
| 2026-10-08 | Releases to PyPI and npm are made by hand decision only; npm versions are staged and approved on npm |

## To do

| What | Notes |
|---|---|
| The site as one page with two tabs | Flash: install, back up this device, restore a backup, a plain warning that installing erases the firmware on the device (stronger when it is not ours). Test: sketching first. Switch to Test after an install, keeping the cable connection |
| The painting by a JavaScript drawing library | The canvas is snapped to the device's levels before anything is compared or sent |
| Picture conversion as an add-on | fit, background, rotation, dither, gamma; the result says in words what was done |
| JavaScript package | Same commands and behaviour as Python, for the browser (Web Serial) and Node; the two libraries give the same pixels for the same input |
| Logo and startup screen | Shown once after flashing, never over a picture the host sent |
| First releases | PyPI and npm, version 0.1.0 |

## Set aside

| What | Why |
|---|---|
| An example that renders TRMNL plugins locally and shows them | Later |
| Flashing from the Python package | The flash page does it |

## Not proven yet

Nothing here has been seen on a real panel. To check on each board (TRMNL X, TRMNL OG, PicPak): the firmware starts,
a picture shows, a partial update shows without a flash, sketching, backup, flash, restore, and going back to the
device's own firmware.
