// SPDX-License-Identifier: MIT
// Backup and restore against a fake loader holding a flash image. Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { test } from 'node:test';

import { backup, backupName, checkBackup, restore } from '../backup.js';

const MB = 0x100000;
const CHIPS = {
  trmnl_x: { desc: 'ESP32-S3 (QFN56) (revision v0.2)', id: 0x18 << 16, bytes: 16 * MB, chipId: 9 },
  trmnl_og: { desc: 'ESP32-C3 (QFN32) (revision v0.4)', id: 0x16 << 16, bytes: 4 * MB, chipId: 5 },
};
const md5 = (d) => createHash('md5').update(d).digest('hex');

/** A flash image of `bytes` that starts like a bootloader of chip id `chipId`; the rest is a pattern. */
function image(bytes, chipId) {
  const d = new Uint8Array(bytes);
  for (let i = 0; i < bytes; i++) d[i] = (i * 7 + (i >> 12)) & 0xff;
  d[0] = 0xe9;
  d[12] = chipId;
  d[13] = 0;
  return d;
}

function fakeDevice(kind = 'trmnl_x', flash = image(CHIPS[kind].bytes, CHIPS[kind].chipId)) {
  const c = CHIPS[kind];
  const seen = { writes: [], reads: [], regs: [], closed: 0 };
  const port = { setSignals: async () => {} };
  class Transport {
    constructor(p) { this.device = p; }
    async disconnect() { seen.closed++; }
  }
  class ESPLoader {
    chip = { readMac: async () => 'AA:bb:0C:01:02:03' };
    async main() { return c.desc; }
    async readFlashId() { return c.id; }
    async readFlash(addr, size) { seen.reads.push([addr, size]); return flash.slice(addr, addr + size); }
    async writeFlash(o) { for (const f of o.fileArray) { seen.writes.push({ address: f.address, size: f.data.length, options: o }); flash.set(f.data, f.address); } }
    async flashMd5sum(addr, size) { return seen.badMd5 ?? md5(flash.slice(addr, addr + size)); }
    async writeReg(addr, value) { seen.regs.push([addr, value]); }
  }
  return { port, esptool: { ESPLoader, Transport }, seen, flash };
}

test('backup reads the whole flash and names the file by the MAC', async () => {
  const dev = fakeDevice();
  const steps = [];
  const out = await backup({ port: dev.port, esptool: dev.esptool, progress: (p) => steps.push(p) });
  assert.equal(out.name, 'trmnl-x-aabb0c010203-backup.bin');
  assert.equal(out.data.length, 16 * MB);
  assert.equal(md5(out.data), md5(dev.flash));
  const reads = dev.seen.reads;
  assert.equal(reads[0][0], 0);
  assert.equal(reads.reduce((n, [, s]) => n + s, 0), 16 * MB);
  assert.equal(reads.at(-1)[0] + reads.at(-1)[1], 16 * MB);
  assert.equal(steps.at(-1), 1);
  assert.deepEqual(dev.seen.writes, []);
  assert.equal(dev.seen.closed, 1);
});

test('backup of a short answer is an error, no file', async () => {
  const dev = fakeDevice();
  dev.esptool.ESPLoader.prototype.readFlash = async () => new Uint8Array(10);
  await assert.rejects(backup({ port: dev.port, esptool: dev.esptool }), /less than it should/);
  assert.equal(dev.seen.closed, 1);
});

test('backup of a TRMNL OG is refused, nothing read', async () => {
  const dev = fakeDevice('trmnl_og');
  await assert.rejects(backup({ port: dev.port, esptool: dev.esptool }), /TRMNL OG.*TRMNL X only/);
  assert.deepEqual(dev.seen.reads, []);
});

test('file name', () => {
  assert.equal(backupName('AA:BB:CC:00:11:22'), 'trmnl-x-aabbcc001122-backup.bin');
});

test('restore writes the file at 0, checks the MD5, resets', async () => {
  const file = image(16 * MB, 9);
  file[5000] ^= 0xff;
  const dev = fakeDevice();
  const out = await restore({ port: dev.port, esptool: dev.esptool, data: file, wait: async () => {} });
  assert.equal(out.size, 16 * MB);
  assert.deepEqual(dev.seen.writes.map((w) => [w.address, w.size]), [[0, 16 * MB]]);
  assert.equal(dev.seen.writes[0].options.eraseAll, false);
  assert.equal(md5(dev.flash), md5(file));
  assert.ok(dev.seen.regs.length > 0, 'the device is restarted into the firmware');
  assert.equal(dev.seen.closed, 1);
});

test('restore refuses a file of the wrong size before any write', async () => {
  const dev = fakeDevice();
  await assert.rejects(restore({ port: dev.port, esptool: dev.esptool, data: image(4 * MB, 9) }), /is 4194304 bytes.*Nothing was written/);
  assert.deepEqual(dev.seen.writes, []);
  assert.equal(dev.seen.closed, 1);
});

test('restore refuses a file from another chip, or not a backup, before any write', async () => {
  for (const [data, re] of [[image(16 * MB, 5), /another chip \(esp32c3\)/], [new Uint8Array(16 * MB), /not a flash backup/]]) {
    const dev = fakeDevice();
    await assert.rejects(restore({ port: dev.port, esptool: dev.esptool, data }), re);
    assert.deepEqual(dev.seen.writes, []);
    assert.deepEqual(dev.seen.regs, []);
  }
});

test('restore on a TRMNL OG is refused before any write', async () => {
  const dev = fakeDevice('trmnl_og');
  await assert.rejects(restore({ port: dev.port, esptool: dev.esptool, data: image(4 * MB, 5) }), /TRMNL X only/);
  assert.deepEqual(dev.seen.writes, []);
});

test('restore: a checksum that differs is an error and the device is not reset', async () => {
  const dev = fakeDevice();
  dev.seen.badMd5 = '0'.repeat(32);
  await assert.rejects(restore({ port: dev.port, esptool: dev.esptool, data: image(16 * MB, 9), wait: async () => {} }), /checksum/);
  assert.deepEqual(dev.seen.regs, []);
  assert.equal(dev.seen.closed, 1);
});

test('checkBackup', () => {
  checkBackup(image(16 * MB, 9), 'esp32s3', 16 * MB);
  assert.throws(() => checkBackup(image(16 * MB, 9), 'esp32c3', 16 * MB), /another chip/);
});
