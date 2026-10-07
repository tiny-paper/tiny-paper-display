# PicPak third-party files

Device-specific files for the PicPak that are not ours and keep their own license. Do not relicense or edit them;
replace a file only with a newer copy from its source.

| File | What | Source | License |
|---|---|---|---|
| `bwry_luts.h` | the fast-refresh waveforms `5s` and `10s` (register LUTs of the SSD2683, panel P420E55), 535 bytes each | [varanu5/picpak-tesserae-client](https://github.com/varanu5/picpak-tesserae-client) @834811c, `firmware/main/epd_lut_5s.h` and `epd_lut_10s.h` (first 535 bytes) | AGPL-3.0-or-later, `LICENSE` |

Credit: varanu5, for publishing the PicPak waveforms in an open client. The bytes equal the tables in PicPak's stock
firmware V1.1.21 (docs/hardware/picpak-refresh.md).

Use: `firmware/components/hal_esp/esp/panel_bwry.c` includes `bwry_luts.h` when it exists (`refresh_speed` fast,
fastest and auto, docs/spec/firmware.md "PicPak refresh speed"). Without it the PicPak runs on the panel's own OTP
waveform. The PicPak firmware built with this file is conveyed under the AGPL terms for that part; every other board
and the rest of `firmware/` stay GPL-3.0-or-later.
