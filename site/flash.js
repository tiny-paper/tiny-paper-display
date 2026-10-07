// SPDX-License-Identifier: MIT
// The flash page's logic, apart from the page: which board, which files at which offsets, the write, the reset.
// The offsets come from the release's own flash.txt (its `# separate:` line), never from this file.
import { md5Hex } from './md5.js';
import { resetToApp, watchdogReset } from './reset.js';

/** The one library the page loads, pinned. esptool-js is Espressif's, under the Apache License 2.0. */
export const ESPTOOL_JS = {
  version: '0.7.0',
  licence: 'Apache-2.0',
  url: 'https://cdn.jsdelivr.net/npm/esptool-js@0.7.0/bundle.js',
};

export const BOARDS = {
  trmnl_og: { title: 'TRMNL OG', chip: 'esp32c3', panel: '800 x 480, 4 grays' },
  picpak: { title: 'PicPak', chip: 'esp32c3', panel: '400 x 300, black white red yellow' },
  trmnl_x: { title: 'TRMNL X', chip: 'esp32s3', panel: '1872 x 1404, 16 grays' },
};

const MB = 0x100000;
// Flash size from the JEDEC capacity byte (esptool DETECTED_FLASH_SIZES)
const CAP = { 0x14: 1, 0x15: 2, 0x16: 4, 0x17: 8, 0x18: 16, 0x19: 32, 0x34: 1, 0x35: 2, 0x36: 4, 0x37: 8, 0x38: 16, 0x39: 32 };
// PicPak: the loader writes reliably only below 4 MB on its 32 MB chip; the display firmware ends far below
const PICPAK_WRITE_LIMIT = 4 * MB;

/** esptool-js chip description ("ESP32-C3 (QFN32) (revision v0.4)") to "esp32c3". */
export const normChip = (name) => (name.match(/esp32-?\w*/i)?.[0] ?? name).toLowerCase().replace(/-/g, '');

export function flashBytes(flashId) {
  const mb = CAP[(flashId >> 16) & 0xff];
  return mb ? mb * MB : 0;
}

/** C3 + 4 MB = TRMNL OG, C3 + 16/32 MB = PicPak, S3 + 16 MB = TRMNL X; null for anything else. */
export function detectBoard(chipName, bytes) {
  const c = normChip(chipName);
  if (c === 'esp32s3' && bytes === 16 * MB) return 'trmnl_x';
  if (c === 'esp32c3' && bytes === 4 * MB) return 'trmnl_og';
  if (c === 'esp32c3' && (bytes === 16 * MB || bytes === 32 * MB)) return 'picpak';
  return null;
}

/** The note for a browser that cannot flash, or null when it can. */
export function supportNote(nav) {
  if (nav && 'serial' in nav) return null;
  return 'This browser cannot reach a USB device (no Web Serial). Use Chrome or Edge on a computer, or flash with esptool: the commands are in the release\'s flash.txt.';
}

/** flash.txt of a build (firmware/tools/idf/build.sh): board, version, chip and the separate files with offsets. */
export function parseLayout(text) {
  const head = text.match(/^# \S+, board (\w+), version (\S+), ESP-IDF \S+, (esp32\w*), (\d+)MB/m);
  const sep = text.match(/^# separate:.*\bwrite_flash\b(.*)$/m);
  if (!head || !sep) throw new Error('flash.txt of this release is not understood: nothing was written');
  const sums = {};
  for (const m of text.matchAll(/^(0x[0-9a-f]+) (\S+) ([0-9a-f]{16})\s*$/gm)) sums[m[2]] = m[3];
  const files = [];
  const tokens = sep[1].trim().split(/\s+/);
  for (let i = 0; i + 1 < tokens.length; i++) {
    if (!/^0x[0-9a-f]+$/i.test(tokens[i]) || !/\.bin$/.test(tokens[i + 1])) continue;
    const name = tokens[i + 1];
    if (/merged/.test(name)) throw new Error('flash.txt lists a one-file image as a separate file: it would erase the settings');
    if (!sums[name]) throw new Error(`flash.txt has no checksum for ${name}`);
    files.push({ address: parseInt(tokens[i], 16), name, sha16: sums[name] });
    i++;
  }
  if (files.length < 2 || files[0].address !== 0) throw new Error('flash.txt of this release lists no bootloader at 0x0');
  return { board: head[1], version: head[2], chip: head[3], flashMb: Number(head[4]), files };
}

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');

/** The files of release `tag` for `board` from this site (`fw/<tag>/<board>__<file>`), each checked against flash.txt. */
export async function loadRelease(fetchFn, base, tag, board) {
  const get = async (name) => {
    const r = await fetchFn(`${base}fw/${encodeURIComponent(tag)}/${board}__${name}`);
    if (!r.ok) throw new Error(`${name} of release ${tag} for ${board}: not found (${r.status}). Nothing was written.`);
    return r;
  };
  const layout = parseLayout(await (await get('flash.txt')).text());
  if (layout.board !== board) throw new Error(`release ${tag}: the files for ${board} say ${layout.board}. Nothing was written.`);
  const files = [];
  for (const f of layout.files) {
    const data = new Uint8Array(await (await get(f.name)).arrayBuffer());
    const sum = hex(await crypto.subtle.digest('SHA-256', data)).slice(0, 16);
    if (sum !== f.sha16) throw new Error(`${f.name} of release ${tag} is damaged (checksum). Nothing was written.`);
    files.push({ address: f.address, name: f.name, data });
  }
  return { layout, files };
}

/**
 * Put release `tag` on the device behind `port`. `board`: the one the person chose, or null to take the one found.
 * `esptool`: { ESPLoader, Transport } of esptool-js. Returns what was written. Throws before the first byte is written
 * when the device is not the board, a file is missing or damaged, or something would not fit.
 */
export async function install({ port, esptool, fetchFn, base, tag, board, log = () => {}, progress = () => {}, wait }) {
  const transport = new esptool.Transport(port, false);
  const terminal = { clean() {}, writeLine: log, write: log };
  const loader = new esptool.ESPLoader({ transport, baudrate: 460800, romBaudrate: 460800, terminal });
  try {
    const desc = await loader.main('default_reset');
    const chip = normChip(desc);
    const bytes = flashBytes(await loader.readFlashId());
    const found = detectBoard(desc, bytes);
    if (!found) throw new Error(`This device is none of the three boards (${desc}, ${bytes / MB} MB flash). Nothing was written.`);
    if (board && board !== found) {
      throw new Error(`You chose ${BOARDS[board].title}, but this device looks like a ${BOARDS[found].title}. Nothing was written.`);
    }
    const { layout, files } = await loadRelease(fetchFn, base, tag, found);
    if (layout.chip !== chip) throw new Error(`release ${tag} for ${found} is built for ${layout.chip}, the device is ${chip}. Nothing was written.`);
    const limit = found === 'picpak' ? PICPAK_WRITE_LIMIT : bytes;
    for (const f of files) {
      if (f.address % 4096 || f.address + f.data.length > limit) throw new Error(`${f.name} does not fit at 0x${f.address.toString(16)}. Nothing was written.`);
    }
    const total = files.reduce((n, f) => n + f.data.length, 0);
    log(`${BOARDS[found].title}: writing ${layout.version}`);
    await loader.writeFlash({
      fileArray: files.map(({ data, address }) => ({ data, address })),
      flashMode: 'keep',
      flashFreq: 'keep',
      flashSize: 'keep',
      eraseAll: false,
      compress: true,
      reportProgress: (i, written, size) => {
        const before = files.slice(0, i).reduce((n, f) => n + f.data.length, 0);
        progress((before + (written / size) * files[i].data.length) / total);
      },
      calculateMD5Hash: md5Hex,
    });
    // not loader.after('hard_reset'): esptool-js 0.7.0 does not leave the loader on the USB Serial/JTAG (reset.js)
    if (!(await watchdogReset(loader, chip, wait))) await resetToApp(transport.device, wait);
    return { board: found, version: layout.version, files: files.map((f) => ({ name: f.name, address: f.address, size: f.data.length })) };
  } finally {
    try {
      await transport.disconnect();
    } catch {
      /* already closed */
    }
  }
}
