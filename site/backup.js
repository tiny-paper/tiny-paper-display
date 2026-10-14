// SPDX-License-Identifier: MIT
// Back up the whole flash of a TRMNL X to a file, and put such a file back. Page logic apart from the page.
import { BOARDS, connect, detectBoard, flashBytes, normChip, refuseUnsupported } from './flash.js';
import { md5Hex } from './md5.js';
import { resetToApp, watchdogReset } from './reset.js';

const CHUNK = 256 * 1024; // esptool-js 0.7.0 readFlash copies its whole answer for every packet: small reads keep that cheap
/** Chip id in the extended header of an ESP image (bytes 12-13 of the bootloader at 0x0), esp-idf esp_image_format.h. */
const CHIP_IDS = { esp32s3: 9, esp32c3: 5 };

/** `trmnl-x-<mac without colons>-backup.bin` */
export const backupName = (mac) => `trmnl-x-${mac.replace(/[^0-9a-f]/gi, '').toLowerCase()}-backup.bin`;

/** The chip a flash image was read from, as normChip says ("esp32s3"), or null when it does not start like one. */
export function imageChip(data) {
  if (data.length < 16 || data[0] !== 0xe9) return null;
  const id = data[12] | (data[13] << 8);
  return Object.keys(CHIP_IDS).find((k) => CHIP_IDS[k] === id) ?? null;
}

/** Start the loader and check it is a TRMNL X. Returns what is needed next; throws a plain sentence otherwise. */
async function open(port, esptool, log) {
  const c = connect(port, esptool, log);
  try {
    const desc = await c.loader.main('default_reset');
    const bytes = flashBytes(await c.loader.readFlashId());
    refuseUnsupported(detectBoard(desc, bytes), desc, bytes);
    return { ...c, chip: normChip(desc), bytes, mac: await c.loader.chip.readMac(c.loader) };
  } catch (e) {
    await close(c.transport);
    throw e;
  }
}

const close = async (transport) => {
  try {
    await transport.disconnect();
  } catch {
    /* already closed */
  }
};

/** Read all of the flash. Returns { name, data, chip, mac }. Writes nothing. `progress`: 0..1. */
export async function backup({ port, esptool, log = () => {}, progress = () => {} }) {
  const { transport, loader, chip, bytes, mac } = await open(port, esptool, log);
  try {
    const data = new Uint8Array(bytes);
    for (let at = 0; at < bytes; at += CHUNK) {
      const part = await loader.readFlash(at, Math.min(CHUNK, bytes - at));
      if (part.length !== Math.min(CHUNK, bytes - at)) throw new Error('The device sent less than it should. No file was made.');
      data.set(part, at);
      progress((at + part.length) / bytes);
    }
    return { name: backupName(mac), data, chip, mac };
  } finally {
    await close(transport);
  }
}

/** Throws a plain sentence when `data` cannot be this device's backup. Pure: checks only. */
export function checkBackup(data, chip, bytes) {
  if (data.length !== bytes) throw new Error(`This file is ${data.length} bytes, the flash of this device is ${bytes}. Nothing was written.`);
  const from = imageChip(data);
  if (from !== chip) throw new Error(`This file is ${from ? `from another chip (${from})` : 'not a flash backup'}, this device is ${chip}. Nothing was written.`);
}

/** Write `data` (a backup file) at 0, check it with the loader's MD5, reset into the app. Refuses before any write. */
export async function restore({ port, esptool, data, log = () => {}, progress = () => {}, wait }) {
  const { transport, loader, chip, bytes } = await open(port, esptool, log);
  try {
    checkBackup(data, chip, bytes);
    await loader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep', eraseAll: false, compress: true,
      reportProgress: (_i, written, size) => progress(written / size),
    });
    log('Checking what was written...');
    const want = md5Hex(data);
    const got = await loader.flashMd5sum(0, data.length);
    if (got !== want) throw new Error('The device does not hold what was written (checksum). Write the file again before you restart it.');
    if (!(await watchdogReset(loader, chip, wait))) await resetToApp(transport.device, wait);
    return { board: 'trmnl_x', size: data.length, title: BOARDS.trmnl_x.title };
  } finally {
    await close(transport);
  }
}
