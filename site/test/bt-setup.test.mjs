// SPDX-License-Identifier: MIT
// The Bluetooth setup step against a fake serial port that answers with the spec's vector bytes: the default shown as
// off, the requests byte for byte, hello without bt, log text and split answers, a bad CRC, silence, a refusal, a pulled
// cable. Run: node --test site/test/*.test.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { BT_WORDS, openBtSetup } from '../bt-setup.js';
import { decodeMap, encodeMap } from '../frame.js';
import { fakePort, frameHex, fromHex, steps, toHex, vectorDevice } from './fake-port.mjs';

const FAST = { waitMs: 300, tryMs: 100, timeoutMs: 150 };

test('the encoder gives the vector requests and the decoder reads the vector answers', () => {
  assert.equal(toHex(encodeMap({ on: true })), 'a1626f6ef5');
  assert.equal(toHex(encodeMap({ pair: true })), 'a16470616972f5');
  assert.equal(toHex(encodeMap({ forget: true })), 'a166666f72676574f5');
  const hello = decodeMap(fromHex(steps.hello_off.rx.slice(12, -4)));
  assert.deepEqual(hello.depths, [1, 2]);
  assert.equal(hello.max_payload, 6144);
  assert.equal(hello.bt, 'off');
  assert.equal(hello.fw, '0.0.0+vec');
});

test('a device with Bluetooth off shows off; switching on sends the vector request and shows on', async () => {
  const port = fakePort(vectorDevice());
  const s = await openBtSetup(port, FAST);
  assert.equal(s.state.bt, 'off');
  assert.equal(port.sent[0], steps.hello_off.tx);
  s.link.nextSeq = steps.on.seq;
  await s.setOn(true);
  assert.equal(port.sent[1], steps.on.tx);
  assert.equal(s.state.bt, 'on');
  assert.equal(s.state.name, 'tp-display-VEC0');
  await s.close();
});

test('pair sends the vector request and reports the open window; forget and off likewise', async () => {
  const port = fakePort(vectorDevice());
  const s = await openBtSetup(port, FAST);
  s.link.nextSeq = steps.pair.seq;
  assert.equal((await s.pair()).pairing, 120);
  assert.equal(port.sent[1], steps.pair.tx);
  s.link.nextSeq = steps.forget.seq;
  await s.forget();
  assert.equal(port.sent[2], steps.forget.tx);
  s.link.nextSeq = steps.off.seq;
  assert.equal((await s.setOn(false)).bt, 'off');
  assert.equal(port.sent[3], steps.off.tx);
  await s.close();
});

test('a hello without bt: no Bluetooth, and the words say so', async () => {
  const noBt = frameHex(1, 0x81, toHex(encodeMap({ proto: 1, project: 'display', fw: '0.0.0' })));
  const s = await openBtSetup(fakePort(() => [noBt]), FAST);
  assert.equal(s.state.bt, null);
  assert.match(BT_WORDS.none, /no Bluetooth/);
  await s.close();
});

test('log text, an answer split over reads, and a frame behind a bad one are handled', async () => {
  const rx = steps.hello_off.rx;
  const log = toHex(new TextEncoder().encode('# boot: waiting\nrst:0x1 A5\n'));
  const s = await openBtSetup(fakePort(() => [log + rx.slice(0, 2), rx.slice(2, 10), rx.slice(10, 40), rx.slice(40)]), FAST);
  assert.equal(s.state.bt, 'off');
  await s.close();
  const bad = rx.slice(0, -2) + (rx.slice(-2) === '00' ? '01' : '00');
  const s2 = await openBtSetup(fakePort(() => [bad + rx]), FAST);
  assert.equal(s2.state.bt, 'off');
  await s2.close();
});

test('an answer with a bad CRC and a silent port end in the timeout message', async () => {
  const rx = steps.hello_off.rx;
  const bad = rx.slice(0, -2) + (rx.slice(-2) === '00' ? '01' : '00');
  await assert.rejects(openBtSetup(fakePort(() => [bad]), FAST), /did not answer/);
  await assert.rejects(openBtSetup(fakePort(() => []), FAST), /did not answer/);
});

test('a device that is slow to restart is asked again until it answers', async () => {
  let n = 0;
  const port = fakePort((tx) => (++n < 3 ? [] : [steps.hello_off.rx]));
  const s = await openBtSetup(port, { waitMs: 2000, tryMs: 50, timeoutMs: 150 });
  assert.equal(s.state.bt, 'off');
  assert.ok(port.sent.length >= 3);
  await s.close();
});

test('a refusal shows its msg', async () => {
  const s = await openBtSetup(fakePort(vectorDevice()), FAST);
  s.link.nextSeq = steps.pair_while_off.seq;
  await assert.rejects(s.pair(), /Bluetooth is off: bt \{ on: true \} first/);
  assert.equal(s.state.bt, 'off');
  await s.close();
});

test('no port, a port that will not open, a cable pulled mid-way: plain messages, no other error', async () => {
  await assert.rejects(openBtSetup(null, FAST), /No port was chosen/);
  await assert.rejects(openBtSetup(fakePort(() => [], { openFails: true }), FAST), /could not be opened/);
  const port = fakePort(vectorDevice({ [steps.on.tx]: [] }));
  const s = await openBtSetup(port, FAST);
  s.link.nextSeq = steps.on.seq;
  setTimeout(() => port.closeNow(), 30);
  await assert.rejects(s.setOn(true), /port was closed/);
  await s.close();
});
