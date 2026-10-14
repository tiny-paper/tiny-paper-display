// SPDX-License-Identifier: MIT
// The flash page against a fake serial port and a fake loader: the right files at the right offsets per board, nothing
// written when something is wrong, the note for a browser without Web Serial. Run: node --test site/test/*.test.mjs
// TINY_DIST: directories of real builds (firmware/build/esp/<name>/dist), then their flash.txt is used as well.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import { BOARDS, ESPTOOL_JS, detectBoard, flashBytes, install, parseLayout, supportNote } from '../flash.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const MB = 0x100000;
const DEVICE = {
  trmnl_og: { desc: 'ESP32-C3 (QFN32) (revision v0.4)', id: 0x16 << 16 },
  picpak: { desc: 'ESP32-C3 (QFN32) (revision v0.4)', id: 0x19 << 16 },
  trmnl_x: { desc: 'ESP32-S3 (QFN56) (revision v0.2)', id: 0x18 << 16 },
};

/** Offsets as the board's partition table has them: the truth the page must end up writing to. */
function partitions(board) {
  const rows = readFileSync(join(ROOT, 'firmware/boards', board, 'partitions.csv'), 'utf8').split('\n')
    .filter((l) => l.trim() && !l.startsWith('#')).map((l) => l.split(',').map((c) => c.trim()));
  const off = (pred) => parseInt(rows.find(pred)[3], 16);
  return { otadata: off((r) => r[2] === 'ota'), app: off((r) => r[2] === 'ota_0' || r[2] === 'factory') };
}

const sha16 = (data) => createHash('sha256').update(data).digest('hex').slice(0, 16);

/** A release as the build workflow publishes it: flash.txt in build.sh's format and the four files. */
function release(board) {
  const p = partitions(board);
  const chip = BOARDS[board].chip;
  const files = [[0, 'bootloader.bin'], [0x8000, 'partition-table.bin'], [p.otadata, 'ota_data_initial.bin'], [p.app, 'tiny_display.bin']]
    .map(([address, name], i) => ({ address, name, data: new Uint8Array(4096 * (i + 1)).fill(i + 1) }));
  const pairs = files.map((f) => `0x${f.address.toString(16)} ${f.name}`);
  const text = [
    `# display-${board}, board ${board}, version 0.1.0+test, ESP-IDF v5.5.1, ${chip}, ${board === 'trmnl_og' ? 4 : 16}MB`,
    '# flash options: --flash_mode dio --flash_freq 80m --flash_size 4MB',
    '# offset   file   sha256',
    ...files.map((f) => `0x${f.address.toString(16)} ${f.name} ${sha16(f.data)}`),
    '0x0 display-merged.bin 0123456789abcdef   (all of the above in one file)',
    '',
    `# one file:  python -m esptool --chip ${chip} -p COMx -b 460800 write_flash 0x0 display-merged.bin`,
    `# separate:  python -m esptool --chip ${chip} -p COMx -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB ${pairs.join(' ')}`,
    '',
  ].join('\n');
  return { text, files };
}

function site(board, rel, tag = 'v1') {
  const served = { [`fw/${tag}/${board}__flash.txt`]: new TextEncoder().encode(rel.text) };
  for (const f of rel.files) served[`fw/${tag}/${board}__${f.name}`] = f.data;
  return async (url) => {
    const data = served[url];
    return { ok: !!data, status: data ? 200 : 404, text: async () => new TextDecoder().decode(data), arrayBuffer: async () => data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) };
  };
}

/** A serial port and esptool-js as far as the page uses them; everything done to the device is recorded. */
function fakeDevice(board) {
  const seen = { writes: [], regs: [], signals: [], closed: 0 };
  const port = { setSignals: async (s) => { seen.signals.push(s); } };
  class Transport {
    constructor(p) { this.device = p; }
    async disconnect() { seen.closed++; }
  }
  class ESPLoader {
    async main() { return DEVICE[board].desc; }
    async readFlashId() { return DEVICE[board].id; }
    async writeFlash(o) { seen.options = o; for (const f of o.fileArray) seen.writes.push({ address: f.address, size: f.data.length, first: f.data[0] }); }
    async writeReg(addr, value) { seen.regs.push([addr, value]); }
  }
  return { port, esptool: { ESPLoader, Transport }, seen };
}

const run = (board, rel, dev, extra = {}) => install({ port: dev.port, esptool: dev.esptool, fetchFn: site(board, rel), base: '', tag: 'v1', board: null, wait: async () => {}, ...extra });

for (const board of ['trmnl_x']) {
  test(`${board}: the separate files at the offsets of its partition table, and nothing else`, async () => {
    const rel = release(board);
    const dev = fakeDevice(board);
    const done = await run(board, rel, dev);
    const p = partitions(board);
    assert.equal(done.board, board);
    assert.deepEqual(dev.seen.writes.map((w) => w.address), [0, 0x8000, p.otadata, p.app]);
    assert.deepEqual(dev.seen.writes.map((w) => w.first), [1, 2, 3, 4]);
    assert.deepEqual(dev.seen.writes.map((w) => w.size), [4096, 8192, 12288, 16384]);
    // settings are kept: no erase of the whole flash, the flash header untouched, no one-file image
    assert.equal(dev.seen.options.eraseAll, false);
    assert.equal(dev.seen.options.flashMode, 'keep');
    assert.equal(typeof dev.seen.options.calculateMD5Hash, 'function');
    assert.ok(dev.seen.regs.length > 0, 'the device is restarted into the firmware');
    assert.equal(dev.seen.closed, 1);
  });
}

test('the offsets differ per board (the page takes them from the release, not from itself)', () => {
  const offsets = Object.keys(BOARDS).map((b) => parseLayout(release(b).text).files.map((f) => f.address).join());
  assert.equal(new Set(offsets).size, 3);
});

test('real build layouts (TINY_DIST) parse to their board and partition table', { skip: !process.env.TINY_DIST }, () => {
  const dirs = process.env.TINY_DIST.split(':').filter(Boolean);
  assert.ok(dirs.length > 0);
  for (const dir of dirs) {
    assert.ok(existsSync(join(dir, 'flash.txt')), `${dir}/flash.txt`);
    const layout = parseLayout(readFileSync(join(dir, 'flash.txt'), 'utf8'));
    const p = partitions(layout.board);
    assert.equal(layout.chip, BOARDS[layout.board].chip);
    assert.deepEqual(layout.files.map((f) => [f.address, f.name]),
      [[0, 'bootloader.bin'], [0x8000, 'partition-table.bin'], [p.otadata, 'ota_data_initial.bin'], [p.app, 'tiny_display.bin']]);
    for (const f of layout.files) assert.equal(sha16(readFileSync(join(dir, f.name))), f.sha16);
  }
});

test('a device that is not the chosen board: nothing written', async () => {
  const dev = fakeDevice('trmnl_x');
  await assert.rejects(run('trmnl_og', release('trmnl_og'), dev, { board: 'trmnl_og' }), /looks like a TRMNL X.*Nothing was written/);
  assert.deepEqual(dev.seen.writes, []);
  assert.equal(dev.seen.closed, 1);
});

for (const board of ['trmnl_og', 'picpak']) {
  test(`a ${board} is refused with a plain sentence, nothing written`, async () => {
    const dev = fakeDevice(board);
    await assert.rejects(run(board, release(board), dev), /This device is a .*works on the TRMNL X only\. Nothing was written/);
    assert.deepEqual(dev.seen.writes, []);
    assert.deepEqual(dev.seen.regs, []);
    assert.equal(dev.seen.closed, 1);
  });
}

test('a device that is none of the three: nothing written', async () => {
  const dev = fakeDevice('trmnl_og');
  dev.esptool.ESPLoader.prototype.readFlashId = async () => 0x17 << 16; // 8 MB
  await assert.rejects(run('trmnl_og', release('trmnl_og'), dev), /not a TRMNL X/);
  assert.deepEqual(dev.seen.writes, []);
});

test('a damaged file: nothing written', async () => {
  const rel = release('trmnl_x');
  rel.files[3].data = rel.files[3].data.slice().fill(9);
  const dev = fakeDevice('trmnl_x');
  await assert.rejects(run('trmnl_x', rel, dev), /tiny_display.bin of release v1 is damaged/);
  assert.deepEqual(dev.seen.writes, []);
});

test('a missing file or release: nothing written', async () => {
  const rel = release('trmnl_x');
  rel.files.pop();
  const dev = fakeDevice('trmnl_x');
  await assert.rejects(run('trmnl_x', rel, dev), /tiny_display.bin of release v1 for trmnl_x: not found/);
  assert.deepEqual(dev.seen.writes, []);
});

test('files of another board under this board\'s name: nothing written', async () => {
  const rel = release('picpak');
  const dev = fakeDevice('trmnl_x');
  await assert.rejects(run('trmnl_x', rel, dev), /say picpak/);
  assert.deepEqual(dev.seen.writes, []);
});

test('a one-file image is never written (it would erase the settings)', () => {
  const text = release('trmnl_og').text.replace(/(# separate:.*write_flash) .*/, '$1 0x0 display-merged.bin 0x8000 partition-table.bin');
  assert.throws(() => parseLayout(text), /erase the settings/);
  assert.throws(() => parseLayout('# nothing'), /not understood/);
});

test('board detection', () => {
  assert.equal(detectBoard('ESP32-C3', 4 * MB), 'trmnl_og');
  assert.equal(detectBoard('ESP32-C3', 16 * MB), 'picpak');
  assert.equal(detectBoard('ESP32-C3', flashBytes(0x19 << 16)), 'picpak');
  assert.equal(detectBoard('ESP32-S3', 16 * MB), 'trmnl_x');
  assert.equal(detectBoard('ESP32-S3', 8 * MB), null);
  assert.equal(detectBoard('ESP32-C6', 4 * MB), null);
  assert.equal(flashBytes(0), 0);
});

test('a browser without Web Serial gets the note, and the page shows it', () => {
  assert.match(supportNote({}), /no Web Serial.*Chrome or Edge/);
  assert.match(supportNote(undefined), /no Web Serial/);
  assert.equal(supportNote({ serial: {} }), null);
  const page = readFileSync(join(ROOT, 'site/index.html'), 'utf8');
  assert.match(page, /<p id="nosupport"[^>]*hidden>/);
  assert.match(page, /const note = supportNote\(navigator\);\s*if \(note\) \{ \$\('nosupport'\)\.textContent = note; \$\('nosupport'\)\.hidden = false; \}/);
  assert.match(page, /else if \(!note\) \$\('install'\)\.disabled = false/);
});

test('the page says which library it loads, its version and licence', () => {
  const page = readFileSync(join(ROOT, 'site/index.html'), 'utf8');
  assert.equal(ESPTOOL_JS.url, `https://cdn.jsdelivr.net/npm/esptool-js@${ESPTOOL_JS.version}/bundle.js`);
  assert.ok(page.includes(`esptool-js</a> ${ESPTOOL_JS.version}`));
  assert.ok(page.includes('Apache License 2.0'));
  assert.equal(page.match(/https?:\/\/[^"'\s<)]+/g).filter((u) => !u.startsWith('https://github.com/espressif/esptool-js') && !u.startsWith('https://help.trmnl.com/en/articles/11936721-put-your-trmnl-in-flashing-mode')).length, 0);
});

test('the user pages name only the TRMNL X, and the flash page links TRMNL\'s flashing-mode help and the backup', () => {
  for (const f of ['index.html', 'api.html', 'sketch.html', 'picture.html']) {
    const page = readFileSync(join(ROOT, 'site', f), 'utf8');
    assert.doesNotMatch(page, /PicPak|TRMNL OG|trmnl_og|mock=og|mock=picpak/i, f);
  }
  const page = readFileSync(join(ROOT, 'site/index.html'), 'utf8');
  assert.match(page, /id="backup"/);
  assert.match(page, /id="restore"/);
  assert.match(page, /href="https:\/\/help\.trmnl\.com\/en\/articles\/11936721-put-your-trmnl-in-flashing-mode"/);
  assert.doesNotMatch(page, /hold its button/);
});
