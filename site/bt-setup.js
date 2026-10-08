// SPDX-License-Identifier: MIT
// The link to the display firmware over the cable (DisplayLink, also the Sketch page's), and the Bluetooth setup step
// of the flash page: talk to the device over the cable (Web Serial), read what `hello` says
// about Bluetooth, switch it, open the pairing window, forget the pairs (docs/spec/usb-display.md "Bluetooth").
// Every failure ends as an Error whose message is plain words for the person; nothing else is thrown.
import { FrameReader, decodeMap, encodeFrame, encodeMap } from './frame.js';

const CMD_HELLO = 0x01;
const CMD_BT = 0x0d;

/** The words next to the choice. Estimates are marked as such: nobody has measured the battery yet. */
export const BT_WORDS = {
  cost: 'On, the battery lasts about a third as long (an estimate, nobody has measured it yet). Off, nothing changes.',
  pairing: 'To pair: press the device\'s button or use "Pair a device now" here. The panel shows 6 digits for 2 minutes; type them on the computer or phone. Only paired devices can show pictures.',
  cable: 'This setting can only be changed over the cable.',
  none: 'This firmware has no Bluetooth.',
};

const MSG = {
  noPort: 'No port was chosen. Pick the device\'s port to go on.',
  open: 'The port could not be opened. Unplug the device, plug it in again and press the button once more.',
  silent: 'The device did not answer. Check the cable, wait a few seconds after installing, and try again.',
  closed: 'The port was closed before the device answered. Plug the device in again and try again.',
  garbled: 'The device sent an answer that could not be read.',
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** One request at a time over an open port. `nextSeq` is the seq of the next request (1..255). */
export class DisplayLink {
  constructor(port, { timeoutMs = 3000 } = {}) {
    this.port = port;
    this.timeoutMs = timeoutMs;
    this.nextSeq = 1;
    this.reader = null;
    this.writer = null;
    this.frames = new FrameReader();
    this.pending = null;
  }

  async open() {
    try {
      if (!this.port.readable) await this.port.open({ baudRate: 115200 });
      this.reader = this.port.readable.getReader();
      this.writer = this.port.writable.getWriter();
    } catch {
      this.release();
      throw new Error(MSG.open);
    }
  }

  release() {
    for (const h of [this.reader, this.writer]) {
      try { h?.releaseLock(); } catch { /* a read is still pending: the close below ends it */ }
    }
    this.reader = null;
    this.writer = null;
  }

  async close() {
    const r = this.reader;
    this.reader = null;
    this.pending = null;
    try { await r?.cancel(); } catch { /* already gone */ }
    this.release();
    try { await this.port.close(); } catch { /* already closed */ }
  }

  /** The next chunk, or null after `ms` (the read stays pending for the next call, no byte is lost). */
  async chunk(ms) {
    if (!this.pending) this.pending = this.reader.read();
    let timer;
    const late = new Promise((r) => { timer = setTimeout(() => r(null), ms); });
    let res;
    try {
      res = await Promise.race([this.pending, late]);
    } catch {
      this.pending = null;
      throw new Error(MSG.closed);
    } finally {
      clearTimeout(timer);
    }
    if (res === null) return null;
    this.pending = null;
    if (res.done) throw new Error(MSG.closed);
    return res.value;
  }

  /** Send `cmd` with `obj`; the decoded answer. A refusal `{err, msg}` throws its msg. `seq`: keep it for a repeat. */
  async request(cmd, obj, options = {}) {
    const map = await this.requestRaw(cmd, obj, options);
    if (typeof map.err === 'number' && typeof map.msg === 'string') throw new Error(map.msg);
    return map;
  }

  /** As `request`, but a refusal is returned as it came (`{err, msg, detail}`): the Sketch page reads its code. */
  async requestRaw(cmd, obj, { seq, timeoutMs = this.timeoutMs } = {}) {
    if (seq === undefined) { seq = this.nextSeq; this.nextSeq = this.nextSeq % 255 + 1; }
    this.frames.clear();
    try {
      await this.writer.write(encodeFrame(seq, cmd, encodeMap(obj)));
    } catch {
      throw new Error(MSG.closed);
    }
    const end = Date.now() + timeoutMs;
    for (;;) {
      for (let f = this.frames.next(); f; f = this.frames.next()) {
        if (f.seq !== seq || f.cmd !== (cmd | 0x80)) continue;
        let map;
        try { map = decodeMap(f.payload); } catch { throw new Error(MSG.garbled); }
        return map;
      }
      const left = end - Date.now();
      if (left <= 0) throw new Error(MSG.silent);
      const c = await this.chunk(left);
      if (c) this.frames.push(c);
    }
  }
}

/**
 * Open `port`, ask `hello` until it answers (the device restarts after flashing: for up to `waitMs`), and return the
 * setup: { link, state: { bt: 'off' | 'on' | null, name, pairing }, setOn, pair, forget, close }.
 * `bt: null` means the firmware has no Bluetooth. Throws an Error with plain words.
 */
export async function openBtSetup(port, { waitMs = 8000, tryMs = 1000, timeoutMs = 3000 } = {}) {
  if (!port) throw new Error(MSG.noPort);
  const link = new DisplayLink(port, { timeoutMs });
  const end = Date.now() + waitMs;
  let hello;
  while (!hello) {
    try {
      if (!link.reader) await link.open();
      hello = await link.request(CMD_HELLO, {}, { seq: 1, timeoutMs: tryMs });
      link.nextSeq = 2;
    } catch (e) {
      if (Date.now() >= end || (e.message !== MSG.silent && e.message !== MSG.open && e.message !== MSG.closed)) {
        await link.close();
        throw e;
      }
      if (e.message === MSG.closed) await link.close();
      await sleep(Math.min(250, Math.max(0, end - Date.now())));
    }
  }
  const state = { bt: hello.bt === 'on' || hello.bt === 'off' ? hello.bt : null, name: null, pairing: 0 };
  const call = async (req) => {
    const a = await link.request(CMD_BT, req);
    state.bt = a.on === true ? 'on' : a.on === false ? 'off' : state.bt;
    if (typeof a.name === 'string') state.name = a.name;
    if (typeof a.pairing === 'number') state.pairing = a.pairing;
    return state;
  };
  return {
    link,
    state,
    setOn: (on) => call({ on: !!on }),
    pair: () => call({ pair: true }),
    forget: () => call({ forget: true }),
    close: () => link.close(),
  };
}
