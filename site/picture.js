// esbuild 0.28.2, 15 source files of site/src (MIT): do not edit, run `npm run build` in site/.
// src/display/client.ts
var DISPLAY_MODES = ["full", "partial", "partial_all", "inverse", "passes"];
var DISPLAY_ERRORS = {
  1: "UNKNOWN_CMD",
  2: "BAD_REQUEST",
  3: "BUSY",
  10: "IO",
  11: "SEQUENCE",
  13: "TOO_LARGE",
  14: "NOMEM",
  32: "NOT_PNG",
  33: "WRONG_SIZE",
  34: "DEPTH",
  35: "BAD_PNG",
  36: "MODE",
  37: "RECT"
};
var refusal = {
  1: "This firmware does not know that command",
  2: "The request is invalid",
  3: "The panel is busy; try again after it finishes",
  10: "The panel could not finish the update",
  11: "The picture transfer was interrupted; send it again",
  13: "The picture is too large",
  14: "The device ran out of memory",
  32: "The picture is not a PNG",
  33: "The picture does not fit the panel",
  34: "The panel cannot show these colors or bit depth",
  35: "The PNG is damaged",
  36: "The panel cannot run this refresh mode",
  37: "The rectangle is outside the panel or misaligned"
};
var DisplayError = class extends Error {
  constructor(code, detail, deviceMessage) {
    super(`${refusal[code] ?? "The device refused the request"}${deviceMessage ? `: ${deviceMessage}` : ""}`);
    this.code = code;
    this.detail = detail;
    this.deviceMessage = deviceMessage;
    this.codeName = DISPLAY_ERRORS[code] ?? String(code);
  }
  code;
  detail;
  deviceMessage;
  codeName;
};
function displayHello(value) {
  const h = value;
  if (!h || h.project !== "display") throw new Error("The connected device does not run the display firmware");
  if (typeof h.board !== "string" || typeof h.fw !== "string" || h.proto !== 1 || !Number.isInteger(h.w) || !Number.isInteger(h.h) || h.w < 1 || h.h < 1 || h.w * h.h > 4194304 || !Number.isInteger(h.max_png) || h.max_png < 57 || h.max_png > 1319644 || h.max_payload !== 6144 || h.chunk !== 4096 || !Array.isArray(h.depths) || !h.depths.length || h.depths.some((d) => ![1, 2, 4].includes(d)) || !Array.isArray(h.modes) || h.modes.some((m) => !DISPLAY_MODES.includes(m)) || !h.modes.includes("full") || !Array.isArray(h.partial_depths) || h.partial_depths.some((d) => !h.depths.includes(d)) || !Number.isInteger(h.full_every) || h.full_every < 0 || h.full_every > 250 || h.colors !== void 0 && (!Array.isArray(h.colors) || h.colors.join(",") !== "black,white,red,yellow")) throw new Error("The display firmware sent invalid panel limits");
  return h;
}
var DisplayClient = class {
  constructor(wire) {
    this.wire = wire;
  }
  wire;
  queue = Promise.resolve();
  async request(cmd, req) {
    const r = await this.wire.requestRaw(cmd, req);
    if (typeof r.err === "number") throw new DisplayError(r.err, r.detail, String(r.msg ?? ""));
    return r;
  }
  async hello() {
    return displayHello(await this.request(1, { proto: 1, app: "tiny-paper-webapp" }));
  }
  exclusive(run) {
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => void 0);
    return p;
  }
  show(png, hello, options = {}) {
    return this.exclusive(async () => {
      displayHello(hello);
      if (png.length < 57 || png.length > hello.max_png) throw new Error("The PNG is outside the device size limit");
      if (options.mode && !hello.modes.includes(options.mode)) throw new Error("The panel cannot run this refresh mode");
      if (options.mode === "passes" !== (options.passes !== void 0) || options.passes !== void 0 && (!Number.isInteger(options.passes) || options.passes < 1 || options.passes > 4)) throw new Error("Passes must be 1 to 4 with the passes mode");
      const begin = await this.request(8, { size: png.length, ...options });
      if (begin.chunk !== 4096 || begin.next !== 0) throw new Error("Invalid display transfer start");
      for (let off = 0; off < png.length; off += 4096) {
        const data = png.subarray(off, off + 4096);
        const r = await this.request(9, { off, data });
        if (r.next !== off + data.length) throw new Error("Invalid display transfer offset; send the picture again");
      }
      return this.finish(await this.request(10, {}));
    });
  }
  finish(r) {
    if (!["full", "partial", "partial_all", "inverse", "passes", "none"].includes(String(r.mode)) || !["asked", "changed", "unchanged", "first", "refused", "cut", "failed", "memory", "depth", "board", "cleanup"].includes(String(r.why)) || ![1, 2, 4].includes(Number(r.depth)) || ["ms", "x", "y", "w", "h"].some((k) => typeof r[k] !== "number" || !Number.isSafeInteger(r[k]) || Number(r[k]) < 0)) throw new Error("Invalid display answer; check the panel before sending again");
    return r;
  }
  clear() {
    return this.exclusive(async () => this.finish(await this.request(11, {})));
  }
};
var reasons = {
  asked: "as requested",
  changed: "changed pixels",
  unchanged: "the picture was already shown",
  first: "the first picture",
  refused: "recovering after a refused transfer",
  cut: "recovering after an interrupted transfer",
  failed: "recovering after a panel failure",
  memory: "no memory for the previous frame",
  depth: "the bit depth changed",
  board: "this panel needs a full refresh",
  cleanup: "the periodic full refresh"
};
function displayAnswer(r) {
  const modes = { full: "Full refresh", partial: "Changed area refreshed", partial_all: "Whole area refreshed", inverse: "Inverse then picture refreshed", passes: "Repeated area refresh", none: "No refresh needed" };
  return `${modes[String(r.mode)] ?? String(r.mode)}: ${reasons[String(r.why)] ?? String(r.why)} (${r.ms} ms).${r.warn ? ` Check the panel: ${r.warn}.` : ""}${r.ckv && typeof r.ckv === "object" && "ok" in r.ckv && r.ckv.ok === false ? " The panel clock check failed; inspect the panel." : ""}`;
}

// src/protocol/cbor.ts
var CborError = class extends Error {
  name = "CborError";
};
var MAX_DEPTH = 8;
var CBOR_KEY_WORK_MAX = 4194304;
var te = new TextEncoder();
var td = new TextDecoder("utf-8", { fatal: true });
var Writer = class {
  buf = new Uint8Array(256);
  len = 0;
  /** Items written so far, and the key work (CBOR_KEY_WORK_MAX). */
  items = 0;
  work = 0;
  /** Before a key of the map whose pairs start after item `start`. */
  key(start) {
    this.work += this.items - start;
    if (this.work > CBOR_KEY_WORK_MAX) throw new CborError("map key work over the limit");
  }
  ensure(n) {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const b = new Uint8Array(cap);
    b.set(this.buf.subarray(0, this.len));
    this.buf = b;
  }
  byte(b) {
    this.ensure(1);
    this.buf[this.len++] = b;
  }
  bytes(b) {
    this.ensure(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
  }
  head(major, n) {
    const m = major << 5;
    if (typeof n === "bigint") {
      if (n < 0n || n > 0xffffffffffffffffn) throw new CborError("integer out of range");
      if (n <= 0xffffffffn) return this.head(major, Number(n));
      this.byte(m | 27);
      const v = new DataView(new ArrayBuffer(8));
      v.setBigUint64(0, n);
      this.bytes(new Uint8Array(v.buffer));
      return;
    }
    if (n < 24) this.byte(m | n);
    else if (n < 256) {
      this.byte(m | 24);
      this.byte(n);
    } else if (n < 65536) {
      this.byte(m | 25);
      this.byte(n >> 8);
      this.byte(n & 255);
    } else if (n < 4294967296) {
      this.byte(m | 26);
      this.byte(n >>> 24 & 255);
      this.byte(n >>> 16 & 255);
      this.byte(n >>> 8 & 255);
      this.byte(n & 255);
    } else {
      this.head(major, BigInt(n));
    }
  }
};
function encodeInto(w, v, depth, short = false) {
  if (depth > MAX_DEPTH) throw new CborError("nesting deeper than 8");
  w.items++;
  if (v === null) return w.byte(246);
  if (v === true) return w.byte(245);
  if (v === false) return w.byte(244);
  if (typeof v === "number") {
    if (Number.isInteger(v) && !Object.is(v, -0)) {
      if (!Number.isSafeInteger(v)) {
        w.items--;
        return encodeInto(w, BigInt(v), depth);
      }
      return v >= 0 ? w.head(0, v) : w.head(1, -1 - v);
    }
    if (short) {
      const h = floatToHalf(v);
      if (h >= 0) return w.bytes(Uint8Array.of(249, h >> 8, h & 255));
      if (Math.fround(v) === v || Number.isNaN(v)) {
        const d4 = new DataView(new ArrayBuffer(4));
        d4.setFloat32(0, v);
        w.byte(250);
        return w.bytes(new Uint8Array(d4.buffer));
      }
    }
    w.byte(251);
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v);
    return w.bytes(new Uint8Array(dv.buffer));
  }
  if (typeof v === "bigint") return v >= 0n ? w.head(0, v) : w.head(1, -1n - v);
  if (typeof v === "string") {
    const b = te.encode(v);
    w.head(3, b.length);
    return w.bytes(b);
  }
  if (v instanceof Uint8Array) {
    w.head(2, v.length);
    return w.bytes(v);
  }
  if (Array.isArray(v)) {
    w.head(4, v.length);
    for (const x of v) encodeInto(w, x, depth + 1, short);
    return;
  }
  if (v instanceof Map) {
    w.head(5, v.size);
    const start = w.items;
    for (const [k, x] of v) {
      if (typeof k !== "string") throw new CborError("map keys must be strings");
      w.key(start);
      encodeInto(w, k, depth + 1, short);
      encodeInto(w, x, depth + 1, short);
    }
    return;
  }
  if (typeof v === "object") {
    const entries = Object.entries(v).filter(([, x]) => x !== void 0);
    w.head(5, entries.length);
    const start = w.items;
    for (const [k, x] of entries) {
      w.key(start);
      encodeInto(w, k, depth + 1, short);
      encodeInto(w, x, depth + 1, short);
    }
    return;
  }
  throw new CborError(`cannot encode ${typeof v}`);
}
function cborEncode(v) {
  const w = new Writer();
  encodeInto(w, v, 1);
  return w.buf.slice(0, w.len);
}
function floatToHalf(v) {
  if (!Number.isFinite(v)) return -1;
  if (v === 0) return Object.is(v, -0) ? 32768 : 0;
  const a = Math.abs(v);
  const e = Math.floor(Math.log2(a));
  if (e < -14 || e > 15) return -1;
  const m = a / 2 ** e - 1;
  const f = m * 1024;
  if (!Number.isInteger(f) || f < 0 || f > 1023 || (1 + f / 1024) * 2 ** e !== a) return -1;
  return (v < 0 ? 32768 : 0) | e + 15 << 10 | f;
}
function halfToFloat(h) {
  const s = h & 32768 ? -1 : 1;
  const e = h >> 10 & 31;
  const f = h & 1023;
  if (e === 0) return s * f * 2 ** -24;
  if (e === 31) return f ? NaN : s * Infinity;
  return s * (1 + f / 1024) * 2 ** (e - 15);
}
var Reader = class {
  pos = 0;
  b;
  constructor(b) {
    this.b = b;
  }
  need(n) {
    if (this.pos + n > this.b.length) throw new CborError("truncated");
  }
  u8() {
    this.need(1);
    return this.b[this.pos++];
  }
  arg(ai) {
    const dv = new DataView(this.b.buffer, this.b.byteOffset, this.b.byteLength);
    let v;
    if (ai < 24) return ai;
    if (ai === 24) {
      v = this.u8();
      if (v < 24) throw new CborError("non-shortest integer");
    } else if (ai === 25) {
      this.need(2);
      v = dv.getUint16(this.pos);
      this.pos += 2;
      if (v < 256) throw new CborError("non-shortest integer");
    } else if (ai === 26) {
      this.need(4);
      v = dv.getUint32(this.pos);
      this.pos += 4;
      if (v < 65536) throw new CborError("non-shortest integer");
    } else if (ai === 27) {
      this.need(8);
      const big = dv.getBigUint64(this.pos);
      this.pos += 8;
      if (big < 0x100000000n) throw new CborError("non-shortest integer");
      v = big <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(big) : big;
    } else if (ai === 31) throw new CborError("indefinite length");
    else throw new CborError("reserved additional info");
    return v;
  }
  len(ai) {
    const n = this.arg(ai);
    if (typeof n === "bigint" || n > this.b.length) throw new CborError("length too large");
    return n;
  }
  /** Items read so far, and the key work (CBOR_KEY_WORK_MAX). */
  items = 0;
  work = 0;
  item(depth) {
    if (depth > MAX_DEPTH) throw new CborError("nesting deeper than 8");
    this.items++;
    const ib = this.u8();
    const major = ib >> 5;
    const ai = ib & 31;
    switch (major) {
      case 0:
        return this.arg(ai);
      case 1: {
        const n = this.arg(ai);
        if (typeof n === "bigint") return -1n - n;
        const r = -1 - n;
        return Number.isSafeInteger(r) ? r : -1n - BigInt(n);
      }
      case 2: {
        const n = this.len(ai);
        this.need(n);
        const out = this.b.slice(this.pos, this.pos + n);
        this.pos += n;
        return out;
      }
      case 3: {
        const n = this.len(ai);
        this.need(n);
        let s;
        try {
          s = td.decode(this.b.subarray(this.pos, this.pos + n));
        } catch {
          throw new CborError("invalid UTF-8");
        }
        this.pos += n;
        return s;
      }
      case 4: {
        const n = this.len(ai);
        const out = [];
        for (let i = 0; i < n; i++) out.push(this.item(depth + 1));
        return out;
      }
      case 5: {
        const n = this.len(ai);
        const out = {};
        const seen = /* @__PURE__ */ new Set();
        const start = this.items;
        for (let i = 0; i < n; i++) {
          this.work += this.items - start;
          if (this.work > CBOR_KEY_WORK_MAX) throw new CborError("map key work over the limit");
          const k = this.item(depth + 1);
          if (typeof k !== "string") throw new CborError("map key is not a text string");
          if (seen.has(k)) throw new CborError(`duplicate map key "${k}"`);
          seen.add(k);
          const v = this.item(depth + 1);
          Object.defineProperty(out, k, { value: v, enumerable: true, writable: true, configurable: true });
        }
        return out;
      }
      case 6:
        throw new CborError("tags are not allowed");
      default: {
        if (ai === 20) return false;
        if (ai === 21) return true;
        if (ai === 22) return null;
        const dv = new DataView(this.b.buffer, this.b.byteOffset, this.b.byteLength);
        if (ai === 25) {
          this.need(2);
          const v = halfToFloat(dv.getUint16(this.pos));
          this.pos += 2;
          return v;
        }
        if (ai === 26) {
          this.need(4);
          const v = dv.getFloat32(this.pos);
          this.pos += 4;
          return v;
        }
        if (ai === 27) {
          this.need(8);
          const v = dv.getFloat64(this.pos);
          this.pos += 8;
          return v;
        }
        if (ai === 31) throw new CborError("indefinite length");
        throw new CborError(`unsupported simple value ${ai}`);
      }
    }
  }
};
function cborDecode(b) {
  const r = new Reader(b);
  const v = r.item(1);
  if (r.pos !== b.length) throw new CborError("trailing bytes");
  return v;
}
function isCborMap(v) {
  return typeof v === "object" && v !== null && !Array.isArray(v) && !(v instanceof Uint8Array);
}
function cborDecodeMap(b) {
  const v = cborDecode(b);
  if (!isCborMap(v)) throw new CborError("expected a map");
  return v;
}

// src/protocol/bytes.ts
var CRC32_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 3988292384 ^ c >>> 1 : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(data, crc = 0) {
  let c = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) c = CRC32_TABLE[(c ^ data[i]) & 255] ^ c >>> 8;
  return ~c >>> 0;
}
function crc16(data, crc = 65535) {
  let c = crc;
  for (let i = 0; i < data.length; i++) {
    c ^= data[i] << 8;
    for (let k = 0; k < 8; k++) c = c & 32768 ? (c << 1 ^ 4129) & 65535 : c << 1 & 65535;
  }
  return c;
}

// src/protocol/link-frame.ts
var SYNC0 = 165;
var SYNC1 = 90;
var MAX_PAYLOAD = 6144;
function encodeFrame(f) {
  if (f.payload.length > MAX_PAYLOAD) throw new RangeError(`payload ${f.payload.length} > ${MAX_PAYLOAD}`);
  const out = new Uint8Array(8 + f.payload.length);
  out[0] = SYNC0;
  out[1] = SYNC1;
  out[2] = f.payload.length & 255;
  out[3] = f.payload.length >> 8;
  out[4] = f.seq & 255;
  out[5] = f.cmd & 255;
  out.set(f.payload, 6);
  const c = crc16(out.subarray(2, 6 + f.payload.length));
  out[6 + f.payload.length] = c & 255;
  out[7 + f.payload.length] = c >> 8;
  return out;
}
var FrameParser = class {
  buf = new Uint8Array(0);
  textDecoder = new TextDecoder("utf-8", { fatal: false });
  push(chunk2) {
    const b = new Uint8Array(this.buf.length + chunk2.length);
    b.set(this.buf);
    b.set(chunk2, this.buf.length);
    const out = [];
    let i = 0;
    let textStart = 0;
    const flushText = (end) => {
      if (end > textStart) out.push({ type: "text", text: this.textDecoder.decode(b.subarray(textStart, end), { stream: true }) });
    };
    while (i < b.length) {
      if (b[i] !== SYNC0) {
        i++;
        continue;
      }
      if (i + 1 >= b.length) break;
      if (b[i + 1] !== SYNC1) {
        i++;
        continue;
      }
      if (i + 6 > b.length) break;
      const len = b[i + 2] | b[i + 3] << 8;
      if (len > MAX_PAYLOAD) {
        i++;
        continue;
      }
      if (i + 8 + len > b.length) break;
      const crc = b[i + 6 + len] | b[i + 7 + len] << 8;
      if (crc16(b.subarray(i + 2, i + 6 + len)) !== crc) {
        i++;
        continue;
      }
      flushText(i);
      out.push({ type: "frame", frame: { seq: b[i + 4], cmd: b[i + 5], payload: b.slice(i + 6, i + 6 + len) } });
      i += 8 + len;
      textStart = i;
    }
    let keep = i;
    if (i >= b.length) keep = b.length;
    flushText(Math.min(keep, b.length));
    this.buf = b.slice(keep);
    return out;
  }
};
function decodePayload(p) {
  return p.length === 0 ? {} : cborDecodeMap(p);
}

// src/display/mockDevice.ts
var X_KNOBS = [
  { name: "gray_table", min: 0, max: 1, def: 1, words: ["fastepd", "stock"] },
  { name: "gray_partial", min: 0, max: 1, def: 1, words: ["off", "on"] },
  { name: "gray_dark", min: 0, max: 14, def: 0 },
  { name: "gray_light", min: 0, max: 14, def: 3 },
  { name: "passes_partial", min: 1, max: 14, def: 4 },
  { name: "passes_full", min: 1, max: 14, def: 3 },
  { name: "wave_kick", min: 0, max: 14, def: 2 },
  { name: "wave_extra", min: 0, max: 14, def: 0 },
  { name: "wave_redrive", min: 0, max: 3, def: 2 },
  { name: "row_settle", min: 0, max: 20, def: 2 },
  { name: "row_hold", min: 0, max: 20, def: 2 },
  { name: "row_drive", min: 0, max: 200, def: 32 },
  { name: "clear", min: 0, max: 3, def: 0, words: ["auto", "none", "fast", "slow"] }
];
function tuningMap(values, knobs = X_KNOBS) {
  return Object.fromEntries(knobs.map((k, i) => [k.name, k.words ? k.words[values[i] - k.min] : values[i]]));
}
function applyTuning(values, request, knobs = X_KNOBS) {
  const refused = {};
  const range = (k) => k.words ? `allowed ${k.words.join(", ")}` : `allowed ${k.min} to ${k.max}`;
  if (request === "defaults") {
    knobs.forEach((k, i) => {
      values[i] = k.def;
    });
    return refused;
  }
  if (typeof request !== "object" || request === null || Array.isArray(request)) return { tuning: 'tuning is a map of keys, or "defaults"' };
  for (const [key, v] of Object.entries(request)) {
    if (Object.keys(refused).length >= 8) break;
    const i = knobs.findIndex((k2) => k2.name === key);
    if (i < 0) {
      refused[key] = `no such key; known: ${knobs.map((k2) => k2.name).join(" ")}`.slice(0, 120);
      continue;
    }
    const k = knobs[i];
    let n;
    if (typeof v === "number" && Number.isInteger(v)) n = v;
    else if (typeof v === "boolean" && k.words && k.max === 1) n = v ? 1 : 0;
    else if (typeof v === "string" && k.words) {
      const w = k.words.indexOf(v);
      n = w < 0 ? void 0 : w + k.min;
    } else if (typeof v === "object") {
      refused[key] = "the value is a number or a word";
      continue;
    }
    if (n === void 0) refused[key] = `${k.words ? "not one of the words" : "not a whole number"}; ${range(k)}`;
    else if (n < k.min || n > k.max) refused[key] = `out of range; ${range(k)}`;
    else values[i] = n;
  }
  return refused;
}
var MOCK_PANELS = { x: "TRMNL X", og: "TRMNL OG", picpak: "PicPak" };
var MockDisplayDevice = class {
  hello = {
    proto: 1,
    project: "display",
    fw: "test",
    board: "trmnl_og",
    w: 16,
    h: 8,
    depths: [1, 2],
    partial_depths: [1],
    modes: ["full", "partial", "inverse", "passes"],
    full_every: 20,
    max_png: 8192,
    max_payload: 6144,
    chunk: 4096
  };
  /** Every finished show: what was asked and the PNG's header. */
  shows = [];
  /** One answer as after a lost frame: a full refresh nobody asked for. */
  lost = false;
  detail = void 0;
  begin = {};
  calls = [];
  shown = [];
  error = 0;
  parser = new FrameParser();
  output = () => {
  };
  png = new Uint8Array();
  next = 0;
  /** The TRMNL X's knobs, now (`x` panel only). */
  knobs = X_KNOBS.map((k) => k.def);
  /** `x`, `og`, `picpak`: that board's panel as its hello describes it (usb-display.md "Boards"); else a 16 x 8 test panel. */
  constructor(panel = "") {
    if (panel === "x") Object.assign(this.hello, {
      board: "trmnl_x",
      w: 1872,
      h: 1404,
      depths: [1, 2, 4],
      partial_depths: [1, 2, 4],
      max_png: 1319644,
      modes: ["full", "partial", "partial_all", "inverse", "passes"],
      tuning: tuningMap(this.knobs)
    });
    else if (panel === "og") Object.assign(this.hello, { w: 800, h: 480, max_png: 131072, modes: ["full", "partial", "inverse"] });
    else if (panel === "picpak") Object.assign(this.hello, {
      board: "picpak",
      w: 400,
      h: 300,
      depths: [1],
      colors: ["black", "white", "red", "yellow"],
      partial_depths: [],
      modes: ["full"],
      full_every: 0,
      max_png: 65536
    });
  }
  onOutput(cb) {
    this.output = cb;
  }
  write(bytes) {
    for (const ev of this.parser.push(bytes)) {
      if (ev.type !== "frame") continue;
      const { seq, cmd, payload } = ev.frame;
      const req = decodePayload(payload);
      this.calls.push({ cmd, req });
      let res;
      if (cmd === 1) res = this.hello.tuning ? { ...this.hello, tuning: tuningMap(this.knobs) } : this.hello;
      else if (cmd === 12 && !this.error) {
        const refused = req.tuning === void 0 ? {} : this.hello.tuning ? applyTuning(this.knobs, req.tuning) : { tuning: "this board has no panel tuning" };
        res = { full_every: this.hello.full_every, ...this.hello.tuning ? { tuning: tuningMap(this.knobs) } : {}, ...Object.keys(refused).length ? { refused } : {} };
      } else if (this.error) {
        res = { err: this.error, msg: "fake refusal", ...this.detail === void 0 ? {} : { detail: this.detail } };
        this.error = 0;
      } else if (cmd === 8) {
        this.png = new Uint8Array(Number(req.size));
        this.next = 0;
        this.begin = req;
        res = { next: 0, chunk: 4096 };
      } else if (cmd === 9) {
        const data = req.data;
        this.png.set(data, Number(req.off));
        this.next += data.length;
        res = { next: this.next };
      } else if (cmd === 10 || cmd === 11) {
        res = { mode: "full", why: "asked", depth: 1, ms: 0, x: 0, y: 0, w: this.hello.w, h: this.hello.h };
        if (cmd === 10) {
          this.shown.push(Array.from(this.png));
          const v = new DataView(this.png.buffer), b = this.begin;
          const show = { ...b.mode === void 0 ? {} : { mode: b.mode }, ...b.x === void 0 ? {} : { x: b.x, y: b.y }, w: v.getUint32(16), h: v.getUint32(20), bits: this.png[24], png: this.png };
          this.shows.push(show);
          if (this.lost) {
            this.lost = false;
            res = { ...res, why: "cut" };
          } else if (b.mode && b.mode !== "full") res = { mode: b.mode, why: b.mode === "partial" ? "changed" : "asked", depth: show.bits, ms: 0, x: b.x ?? 0, y: b.y ?? 0, w: show.w, h: show.h };
        }
      } else res = { err: 1, msg: "not a display command" };
      setTimeout(() => this.output(encodeFrame({ seq, cmd: cmd | 128, payload: cborEncode(res) })), 1);
    }
  }
};

// src/protocol/album.ts
var te2 = new TextEncoder();
var td2 = new TextDecoder("utf-8", { fatal: false });
var rasterStride = (w, depth) => Math.ceil(w * depth / 8);
var MIB = 1024 * 1024;
function packRows(px, w, h, depth) {
  const st = rasterStride(w, depth);
  const out = new Uint8Array(st * h);
  const per = 8 / depth;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) out[y * st + Math.floor(x / per)] |= px[y * w + x] << 8 - depth * (x % per + 1);
  return out;
}

// src/display/png.ts
function chunk(name, data) {
  const b = new Uint8Array(data.length + 12);
  const v = new DataView(b.buffer);
  v.setUint32(0, data.length);
  b.set(new TextEncoder().encode(name), 4);
  b.set(data, 8);
  v.setUint32(b.length - 4, crc32(b.subarray(4, b.length - 4)));
  return b;
}
var DISPLAY_PALETTE = [[0, 0, 0], [255, 0, 0], [255, 255, 0], [255, 255, 255]];
async function packedPng(rows, w, h, depth, color) {
  const stride = Math.ceil(w * depth / 8);
  const raw = new Uint8Array((stride + 1) * h);
  for (let y = 0; y < h; y++) raw.set(rows.subarray(y * stride, (y + 1) * stride), y * (stride + 1) + 1);
  const compressed = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream("deflate"))).arrayBuffer());
  const ihdr = new Uint8Array(13);
  const v = new DataView(ihdr.buffer);
  v.setUint32(0, w);
  v.setUint32(4, h);
  ihdr[8] = depth;
  ihdr[9] = color ? 3 : 0;
  const parts = [Uint8Array.of(137, 80, 78, 71, 13, 10, 26, 10), chunk("IHDR", ihdr)];
  if (color) parts.push(chunk("PLTE", Uint8Array.from(DISPLAY_PALETTE.flat())));
  parts.push(chunk("IDAT", compressed), chunk("IEND", new Uint8Array()));
  const png = new Uint8Array(parts.reduce((n, b) => n + b.length, 0));
  let off = 0;
  for (const b of parts) {
    png.set(b, off);
    off += b.length;
  }
  return png;
}

// src/picture/resample.ts
function axisTaps(srcN, from, to, dstN) {
  const scale = (to - from) / dstN;
  const out = [];
  for (let i = 0; i < dstN; i++) {
    if (scale > 1) {
      const a = from + i * scale, b = a + scale;
      const k0 = Math.max(0, Math.floor(a)), k1 = Math.min(srcN - 1, Math.ceil(b) - 1);
      const w = [];
      for (let k = k0; k <= k1; k++) w.push(Math.max(0, Math.min(b, k + 1) - Math.max(a, k)));
      const sum = w.reduce((s, v) => s + v, 0) || 1;
      out.push({ k0, w: w.map((v) => v / sum) });
    } else {
      const p = from + (i + 0.5) * scale - 0.5, k = Math.floor(p), f = p - k;
      const a = Math.min(srcN - 1, Math.max(0, k)), b = Math.min(srcN - 1, Math.max(0, k + 1));
      out.push(a === b ? { k0: a, w: [1] } : { k0: a, w: [1 - f, f] });
    }
  }
  return out;
}
function resamplePlane(src, sw, sh, rect, dw, dh) {
  const tx = axisTaps(sw, rect.x0, rect.x1, dw), ty = axisTaps(sh, rect.y0, rect.y1, dh);
  const r0 = Math.min(...ty.map((t) => t.k0)), r1 = Math.max(...ty.map((t) => t.k0 + t.w.length - 1));
  const mid = new Float32Array(dw * (r1 - r0 + 1));
  for (let y = r0; y <= r1; y++) {
    const row = y * sw, o = (y - r0) * dw;
    for (let x = 0; x < dw; x++) {
      const t = tx[x];
      let s = 0;
      for (let j = 0; j < t.w.length; j++) s += src[row + t.k0 + j] * t.w[j];
      mid[o + x] = s;
    }
  }
  const out = new Float32Array(dw * dh);
  for (let y = 0; y < dh; y++) {
    const t = ty[y];
    for (let x = 0; x < dw; x++) {
      let s = 0;
      for (let j = 0; j < t.w.length; j++) s += mid[(t.k0 + j - r0) * dw + x] * t.w[j];
      out[y * dw + x] = s;
    }
  }
  return out;
}

// src/picture/prepare.ts
var DEFAULT_SETTINGS = { fit: "fit", border: "white", rotate: "auto", depth: 1, color: false, dither: "floyd", threshold: 128 };
var MAX_SOURCE_PIXELS = 48e6;
function rotateSource(s, deg) {
  if (deg === 0) return s;
  const w = deg === 180 ? s.w : s.h, h = deg === 180 ? s.h : s.w;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < s.h; y++) for (let x = 0; x < s.w; x++) {
    const nx = deg === 90 ? s.h - 1 - y : deg === 180 ? s.w - 1 - x : y;
    const ny = deg === 90 ? x : deg === 180 ? s.h - 1 - y : s.w - 1 - x;
    const a = (y * s.w + x) * 4, b = (ny * w + nx) * 4;
    for (let c = 0; c < 4; c++) out[b + c] = s.rgba[a + c];
  }
  return { w, h, rgba: out };
}
function pickRotation(src, panel, rotate) {
  if (rotate !== "auto") return rotate;
  const s0 = Math.min(panel.w / src.w, panel.h / src.h), s90 = Math.min(panel.w / src.h, panel.h / src.w);
  return s90 > s0 ? 90 : 0;
}
function compose(source, panel, s) {
  if (source.w < 1 || source.h < 1 || source.w * source.h > MAX_SOURCE_PIXELS) throw new Error("The picture is too large or empty");
  const rotation = pickRotation(source, panel, s.rotate);
  const src = rotateSource(source, rotation);
  const edge = s.border === "white" ? 255 : 0;
  const n = s.color ? 3 : 1;
  const raw = Array.from({ length: n }, () => new Float32Array(src.w * src.h));
  for (let i = 0; i < src.w * src.h; i++) {
    const a = src.rgba[i * 4 + 3] / 255;
    const ch = [src.rgba[i * 4], src.rgba[i * 4 + 1], src.rgba[i * 4 + 2]].map((v) => v * a + edge * (1 - a));
    if (n === 1) raw[0][i] = 0.299 * ch[0] + 0.587 * ch[1] + 0.114 * ch[2];
    else for (let c = 0; c < 3; c++) raw[c][i] = ch[c];
  }
  let rect = { x0: 0, y0: 0, x1: src.w, y1: src.h };
  let box = { x: 0, y: 0, w: panel.w, h: panel.h };
  if (s.fit === "fit") {
    const k = Math.min(panel.w / src.w, panel.h / src.h);
    const w = Math.min(panel.w, Math.max(1, Math.round(src.w * k))), h = Math.min(panel.h, Math.max(1, Math.round(src.h * k)));
    box = { x: Math.floor((panel.w - w) / 2), y: Math.floor((panel.h - h) / 2), w, h };
  } else if (s.fit === "fill") {
    const k = Math.max(panel.w / src.w, panel.h / src.h), cw = panel.w / k, ch = panel.h / k;
    rect = { x0: (src.w - cw) / 2, y0: (src.h - ch) / 2, x1: (src.w + cw) / 2, y1: (src.h + ch) / 2 };
  }
  const planes = raw.map((p) => {
    const small = resamplePlane(p, src.w, src.h, rect, box.w, box.h);
    if (box.w === panel.w && box.h === panel.h) return small;
    const full = new Float32Array(panel.w * panel.h).fill(edge);
    for (let y = 0; y < box.h; y++) full.set(small.subarray(y * box.w, (y + 1) * box.w), (box.y + y) * panel.w + box.x);
    return full;
  });
  return { w: panel.w, h: panel.h, planes, rotation, box };
}
var BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];
var clamp = (v, lo, hi) => v < lo ? lo : v > hi ? hi : v;
function grayLevels(plane, w, h, depth, dither, threshold) {
  const n1 = (1 << depth) - 1, out = new Uint8Array(w * h);
  if (dither === "none") {
    for (let i = 0; i < out.length; i++) out[i] = depth === 1 ? plane[i] >= threshold ? 1 : 0 : clamp(Math.round(plane[i] * n1 / 255), 0, n1);
  } else if (dither === "ordered") {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const t = (BAYER4[y % 4][x % 4] + 0.5) / 16;
      out[y * w + x] = clamp(Math.floor(plane[y * w + x] * n1 / 255 + t), 0, n1);
    }
  } else {
    const buf = Float32Array.from(plane);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, old = buf[i], q = clamp(Math.round(old * n1 / 255), 0, n1);
      out[i] = q;
      const err = old - q * 255 / n1;
      if (x + 1 < w) buf[i + 1] += err * 7 / 16;
      if (y + 1 < h) {
        if (x > 0) buf[i + w - 1] += err * 3 / 16;
        buf[i + w] += err * 5 / 16;
        if (x + 1 < w) buf[i + w + 1] += err / 16;
      }
    }
  }
  return out;
}
function nearestColor(r, g, b) {
  let best = 0, bd = Infinity;
  DISPLAY_PALETTE.forEach((p, i) => {
    const d = (r - p[0]) ** 2 + (g - p[1]) ** 2 + (b - p[2]) ** 2;
    if (d < bd) {
      bd = d;
      best = i;
    }
  });
  return best;
}
function colorLevels(planes, w, h, dither) {
  const out = new Uint8Array(w * h);
  if (dither === "floyd") {
    const buf = planes.map((p) => Float32Array.from(p));
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, q = nearestColor(buf[0][i], buf[1][i], buf[2][i]);
      out[i] = q;
      for (let c = 0; c < 3; c++) {
        const err = buf[c][i] - DISPLAY_PALETTE[q][c], p = buf[c];
        if (x + 1 < w) p[i + 1] += err * 7 / 16;
        if (y + 1 < h) {
          if (x > 0) p[i + w - 1] += err * 3 / 16;
          p[i + w] += err * 5 / 16;
          if (x + 1 < w) p[i + w + 1] += err / 16;
        }
      }
    }
  } else {
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, bias = dither === "ordered" ? ((BAYER4[y % 4][x % 4] + 0.5) / 16 - 0.5) * 128 : 0;
      out[i] = nearestColor(planes[0][i] + bias, planes[1][i] + bias, planes[2][i] + bias);
    }
  }
  return out;
}
function quantise(c, s) {
  return s.color ? colorLevels(c.planes, c.w, c.h, s.dither) : grayLevels(c.planes[0], c.w, c.h, s.depth, s.dither, s.threshold);
}
function levelsToRgba(levels, depth, color) {
  const out = new Uint8ClampedArray(levels.length * 4), max = (1 << depth) - 1;
  for (let i = 0; i < levels.length; i++) {
    const v = levels[i], rgb = color ? DISPLAY_PALETTE[v] : [Math.round(v * 255 / max), Math.round(v * 255 / max), Math.round(v * 255 / max)];
    out.set([rgb[0], rgb[1], rgb[2], 255], i * 4);
  }
  return out;
}
function preparePicture(source, panel, s, composed = compose(source, panel, s)) {
  return { w: panel.w, h: panel.h, depth: s.color ? 2 : s.depth, color: s.color, levels: quantise(composed, s), rotation: composed.rotation, settings: s };
}
function preparedPng(p) {
  return packedPng(packRows(p.levels, p.w, p.h, p.depth), p.w, p.h, p.depth, p.color);
}

// src/picture/font.ts
var G = {
  A: ".###./#...#/#...#/#####/#...#/#...#/#...#",
  B: "####./#...#/#...#/####./#...#/#...#/####.",
  C: ".###./#...#/#..../#..../#..../#...#/.###.",
  D: "####./#...#/#...#/#...#/#...#/#...#/####.",
  E: "#####/#..../#..../####./#..../#..../#####",
  F: "#####/#..../#..../####./#..../#..../#....",
  G: ".###./#...#/#..../#.###/#...#/#...#/.###.",
  H: "#...#/#...#/#...#/#####/#...#/#...#/#...#",
  I: ".###./..#../..#../..#../..#../..#../.###.",
  J: "..###/...#./...#./...#./...#./#..#./.##..",
  K: "#...#/#..#./#.#../##.../#.#../#..#./#...#",
  L: "#..../#..../#..../#..../#..../#..../#####",
  M: "#...#/##.##/#.#.#/#.#.#/#...#/#...#/#...#",
  N: "#...#/##..#/#.#.#/#..##/#...#/#...#/#...#",
  O: ".###./#...#/#...#/#...#/#...#/#...#/.###.",
  P: "####./#...#/#...#/####./#..../#..../#....",
  Q: ".###./#...#/#...#/#...#/#.#.#/#..#./.##.#",
  R: "####./#...#/#...#/####./#.#../#..#./#...#",
  S: ".####/#..../#..../.###./....#/....#/####.",
  T: "#####/..#../..#../..#../..#../..#../..#..",
  U: "#...#/#...#/#...#/#...#/#...#/#...#/.###.",
  V: "#...#/#...#/#...#/#...#/#...#/.#.#./..#..",
  W: "#...#/#...#/#...#/#.#.#/#.#.#/##.##/#...#",
  X: "#...#/#...#/.#.#./..#../.#.#./#...#/#...#",
  Y: "#...#/#...#/.#.#./..#../..#../..#../..#..",
  Z: "#####/....#/...#./..#../.#.../#..../#####",
  0: ".###./#...#/#..##/#.#.#/##..#/#...#/.###.",
  1: "..#../.##../..#../..#../..#../..#../.###.",
  2: ".###./#...#/....#/...#./..#../.#.../#####",
  3: "####./....#/....#/.###./....#/....#/####.",
  4: "...#./..##./.#.#./#..#./#####/...#./...#.",
  5: "#####/#..../####./....#/....#/#...#/.###.",
  6: "..##./.#.../#..../####./#...#/#...#/.###.",
  7: "#####/....#/...#./..#../.#.../.#.../.#...",
  8: ".###./#...#/#...#/.###./#...#/#...#/.###.",
  9: ".###./#...#/#...#/.####/....#/...#./.##..",
  ".": "...../...../...../...../...../.##../.##..",
  ",": "...../...../...../...../.##../..#../.#...",
  ":": "...../.##../.##../...../.##../.##../.....",
  "-": "...../...../...../#####/...../...../.....",
  "!": "..#../..#../..#../..#../..#../...../..#..",
  "?": ".###./#...#/....#/...#./..#../...../..#..",
  "/": "....#/....#/...#./..#../.#.../#..../#....",
  "%": "##..#/##..#/...#./..#../.#.../#..##/#..##",
  " ": "...../...../...../...../...../...../....."
};
var GLYPH_W = 6;
var GLYPH_H = 7;
var textWidth = (text, scale) => Math.max(0, text.length * GLYPH_W * scale - scale);
function drawText(text, x, y, scale, dot) {
  let cx = x;
  for (const ch of text.toUpperCase()) {
    const rows = (G[ch] ?? G["?"]).split("/");
    rows.forEach((row, r) => {
      for (let c = 0; c < 5; c++) if (row[c] === "#") for (let dy = 0; dy < scale; dy++) for (let dx = 0; dx < scale; dx++) dot(cx + c * scale + dx, y + r * scale + dy);
    });
    cx += GLYPH_W * scale;
  }
}

// src/picture/builtin.ts
var BUILTINS = ["gradient", "scene", "lines"];
var BUILTIN_NAMES = { gradient: "Gray gradient and step wedge", scene: "Photo-like scene", lines: "Line drawing" };
var smooth = (a, b, v) => {
  const t = Math.min(1, Math.max(0, (v - a) / (b - a)));
  return t * t * (3 - 2 * t);
};
var mix = (a, b, t) => a + (b - a) * t;
function noise(x, y, seed = 0) {
  let h = Math.imul(x, 374761393) ^ Math.imul(y, 668265263) ^ Math.imul(seed + 1, 2147483647);
  h = Math.imul(h ^ h >>> 13, 1274126177);
  return ((h ^ h >>> 16) >>> 0) / 4294967296;
}
var Canvas = class {
  constructor(w, h, bg) {
    this.w = w;
    this.h = h;
    this.rgba = new Uint8ClampedArray(w * h * 4).fill(255);
    if (bg !== 255) for (let i = 0; i < w * h; i++) this.rgba.fill(bg, i * 4, i * 4 + 3);
  }
  w;
  h;
  rgba;
  set(x, y, r, g = r, b = r) {
    if (x < 0 || y < 0 || x >= this.w || y >= this.h) return;
    const i = (y * this.w + x) * 4;
    this.rgba[i] = r;
    this.rgba[i + 1] = g;
    this.rgba[i + 2] = b;
  }
  rect(x, y, w, h, v) {
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) this.set(x + i, y + j, v);
  }
  text(s, x, y, scale, v) {
    drawText(s, x, y, scale, (px, py) => this.set(px, py, v));
  }
  /** A line `t` pixels thick (a square stamp), black unless `v` says otherwise. */
  line(x0, y0, x1, y1, t = 1, v = 0) {
    const n = Math.max(Math.abs(x1 - x0), Math.abs(y1 - y0), 1);
    for (let i = 0; i <= n; i++) {
      const x = Math.round(x0 + (x1 - x0) * i / n), y = Math.round(y0 + (y1 - y0) * i / n);
      this.rect(x - Math.floor((t - 1) / 2), y - Math.floor((t - 1) / 2), t, t, v);
    }
  }
  circle(cx, cy, r, t = 1, v = 0) {
    const n = Math.max(16, Math.ceil(r * 8));
    for (let i = 0; i < n; i++) {
      const a = i * 2 * Math.PI / n;
      this.rect(Math.round(cx + r * Math.cos(a)), Math.round(cy + r * Math.sin(a)), t, t, v);
    }
  }
};
function gradient(w, h, levels) {
  const c = new Canvas(w, h, 255);
  const ramp = Math.floor(h * 0.4), wedge = Math.floor(h * 0.3), unit = Math.max(1, Math.round(Math.min(w, h) / 200));
  for (let y = 0; y < ramp; y++) for (let x = 0; x < w; x++) c.set(x, y, Math.round(255 * x / Math.max(1, w - 1)));
  for (let i = 0; i < levels; i++) {
    const x0 = Math.floor(i * w / levels), x1 = Math.floor((i + 1) * w / levels);
    c.rect(x0, ramp, x1 - x0, wedge, Math.round(255 * i / Math.max(1, levels - 1)));
    if (levels <= 16 && x1 - x0 > 8 * unit) c.text(String(i), x0 + 2 * unit, ramp + wedge - 9 * unit, unit, i * 2 < levels ? 255 : 0);
  }
  const y2 = ramp + wedge, y3 = Math.floor(h * 0.9);
  for (let y = y2; y < y3; y++) for (let x = 0; x < w; x++) c.set(x, y, Math.round(255 * (1 - x / Math.max(1, w - 1))));
  for (let y = y3; y < h; y++) for (let x = 0; x < w; x++) c.set(x, y, x < w / 2 ? x + y & 1 ? 255 : 0 : 128);
  c.text("GRAY GRADIENT", 2 * unit, 2 * unit, 2 * unit, 255);
  return { w, h, rgba: c.rgba };
}
function scene(w, h) {
  const c = new Canvas(w, h, 255), m = Math.min(w, h);
  const fx = w * 0.3, fy = h * 0.58, rx = m * 0.17, ry = m * 0.23;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const u = x / w, v = y / h;
    let r = mix(70, 190, v * 1.2), g = mix(120, 215, v * 1.2), b = mix(205, 245, v);
    const d = Math.hypot(x - w * 0.78, y - h * 0.2) / m;
    const sun = smooth(0.1, 0.06, d), glow = smooth(0.45, 0.06, d) * 0.45;
    r = mix(r, 255, glow);
    g = mix(g, 245, glow);
    b = mix(b, 200, glow * 0.7);
    r = mix(r, 255, sun);
    g = mix(g, 250, sun);
    b = mix(b, 215, sun);
    const far = 0.55 + 0.05 * Math.sin(u * 7 + 1) + 0.02 * Math.sin(u * 19), near = 0.7 + 0.05 * Math.sin(u * 4 + 3) + 0.015 * Math.sin(u * 23);
    const a1 = smooth(far + 6e-3, far - 6e-3, v), a2 = smooth(near + 6e-3, near - 6e-3, v);
    r = mix(r, 95, a1);
    g = mix(g, 120, a1);
    b = mix(b, 135, a1);
    const grass = (noise(x, y, 1) - 0.5) * 46 + (noise(Math.floor(x / 3), y, 2) - 0.5) * 30;
    r = mix(r, 70 + grass, a2);
    g = mix(g, 135 + grass, a2);
    b = mix(b, 55 + grass * 0.6, a2);
    const e = ((x - fx) / rx) ** 2 + ((y - fy) / ry) ** 2, face = smooth(1.06, 0.94, e);
    if (face > 0) {
      const shade = 1 - 0.35 * e + 0.12 * ((fx - x) / rx);
      const hair = smooth(-0.1, 0.1, (fy - ry * 0.45 - y) / ry) * face;
      r = mix(r, 235 * shade, face);
      g = mix(g, 185 * shade, face);
      b = mix(b, 150 * shade, face);
      r = mix(r, 60, hair);
      g = mix(g, 40, hair);
      b = mix(b, 30, hair);
      for (const s of [-1, 1]) {
        const ee = ((x - fx - s * rx * 0.4) / (rx * 0.16)) ** 2 + ((y - fy + ry * 0.12) / (ry * 0.07)) ** 2, eye = smooth(1.1, 0.8, ee);
        r = mix(r, 40, eye);
        g = mix(g, 35, eye);
        b = mix(b, 35, eye);
      }
      const mo = ((x - fx) / (rx * 0.4)) ** 2 + ((y - fy - ry * 0.45) / (ry * 0.07)) ** 2, mouth = smooth(1.1, 0.8, mo);
      r = mix(r, 150, mouth);
      g = mix(g, 50, mouth);
      b = mix(b, 60, mouth);
    }
    c.set(x, y, r, g, b);
  }
  const k = Math.max(1, Math.round(m / 330)), lines2 = [["Small text 1234567890", k], ["MEDIUM TEXT", 2 * k], ["LARGE", 4 * k]];
  const pw = Math.max(...lines2.map(([s, z]) => textWidth(s, z))) + 8 * k, ph = lines2.reduce((n, [, z]) => n + (GLYPH_H + 2) * z, 0) + 6 * k;
  const px = Math.max(0, w - pw - 4 * k), py = Math.max(0, h - ph - 4 * k);
  c.rect(px, py, pw, ph, 250);
  let ty = py + 3 * k;
  for (const [s, z] of lines2) {
    c.text(s, px + 4 * k, ty, z, 20);
    ty += (GLYPH_H + 2) * z;
  }
  return { w, h, rgba: c.rgba };
}
function lines(w, h) {
  const c = new Canvas(w, h, 255), m = Math.min(w, h), u = Math.max(1, Math.round(m / 300));
  c.rect(0, 0, w, 1, 0);
  c.rect(0, h - 1, w, 1, 0);
  c.rect(0, 0, 1, h, 0);
  c.rect(w - 1, 0, 1, h, 0);
  for (let i = 0; i < 3; i++) c.line(w * 0.05, h * (0.1 + 0.05 * i), w * 0.45, h * (0.1 + 0.05 * i) + h * 0.05, i + 1);
  for (let i = 0; i < 5; i++) {
    const gap = i + 1, x0 = Math.round(w * 0.55);
    for (let j = 0; j < 12; j++) c.line(x0 + j * 2 * gap, Math.round(h * (0.08 + 0.05 * i)), x0 + j * 2 * gap, Math.round(h * (0.08 + 0.05 * i)) + Math.round(h * 0.04));
  }
  for (let i = 0; i <= 12; i++) c.line(w * 0.05, h * 0.9, w * 0.45, h * (0.35 + 0.05 * i));
  for (let r = 1; r <= 6; r++) c.circle(w * 0.75, h * 0.55, r * m * 0.05, 1);
  const bx = w * 0.08, by = h * 0.5, bw = w * 0.25, bh = h * 0.25;
  c.line(bx, by + bh * 0.3, bx + bw / 2, by, 2);
  c.line(bx + bw / 2, by, bx + bw, by + bh * 0.3, 2);
  c.line(bx + 4, by + bh * 0.3, bx + 4, by + bh, 2);
  c.line(bx + bw - 4, by + bh * 0.3, bx + bw - 4, by + bh, 2);
  c.line(bx + 4, by + bh, bx + bw - 4, by + bh, 2);
  c.rect(Math.round(bx + bw * 0.42), Math.round(by + bh * 0.6), Math.round(bw * 0.16), Math.round(bh * 0.4), 0);
  c.text("LINE DRAWING 1 PX AND 2 PX", Math.round(w * 0.05), Math.round(h * 0.93), u, 0);
  c.text("SMALL TEXT FOR READING", Math.round(w * 0.55), Math.round(h * 0.84), u, 0);
  c.text("MEDIUM TEXT", Math.round(w * 0.55), Math.round(h * 0.88), 2 * u, 0);
  c.text("BIG", Math.round(w * 0.55), Math.round(h * 0.94), 3 * u, 0);
  return { w, h, rgba: c.rgba };
}
function builtinPicture(kind, w, h, levels) {
  return kind === "gradient" ? gradient(w, h, levels) : kind === "scene" ? scene(w, h) : lines(w, h);
}

// src/picture/plan.ts
var BOARD_TITLES = { trmnl_x: "TRMNL X", trmnl_og: "TRMNL OG", picpak: "PicPak" };
var DITHER_NAMES = { none: "nearest level", floyd: "Floyd-Steinberg", ordered: "ordered 4x4" };
function partialLine(hello) {
  const d = [...hello.partial_depths].sort((a, b) => a - b);
  if (!d.length) return "Partial updates at: none (full refreshes only)";
  const list = d.length === 1 ? `${d[0]}` : `${d.slice(0, -1).join(", ")} and ${d.at(-1)}`;
  return `Partial updates at: ${list} bit${d.at(-1) > 1 ? ` (${2 ** d.at(-1)} grays)` : ""}`;
}
function deviceLine(hello) {
  return `${BOARD_TITLES[hello.board] ?? hello.board}, ${hello.w} x ${hello.h}, display firmware ${hello.fw}.`;
}
function depthChoices(hello) {
  const out = [...hello.depths].sort((a, b) => a - b).map((d) => ({
    key: String(d),
    depth: d,
    color: false,
    label: d === 1 ? "1 bit (black and white)" : `${d} bit (${2 ** d} grays)`
  }));
  if (hello.colors) out.push({ key: "color", depth: 2, color: true, label: "4 colours (black, white, red, yellow)" });
  return out;
}
var deepest = (hello) => depthChoices(hello).at(-1);
function refreshChoices(hello, depth) {
  let why = "";
  if (!hello.modes.includes("partial") || !hello.partial_depths.length) why = "this device has no partial refresh";
  else if (depth.color || !hello.partial_depths.includes(depth.depth)) why = `this device does partial updates at ${[...hello.partial_depths].sort((a, b) => a - b).join(", ")} bit only, not at ${depth.color ? "colour" : `${depth.depth} bit`}`;
  return [
    { key: "auto", label: "Auto (the device decides)", enabled: true, why: "" },
    { key: "partial", label: "Partial (only what changed)", enabled: !why, why },
    { key: "full", label: "Full (flashes the whole panel)", enabled: true, why: "" }
  ];
}
var refreshOptions = (r) => r === "auto" ? {} : { mode: r };
function describeSettings(s, rotation, refresh) {
  const depth = s.color ? "4 colours" : s.depth === 1 ? "1 bit" : `${s.depth} bit gray`;
  const dither = s.dither === "none" && !s.color && s.depth === 1 ? `nearest level (threshold ${s.threshold})` : DITHER_NAMES[s.dither];
  const fit = { fit: "whole picture", fill: "fill and crop", stretch: "stretched" }[s.fit];
  return `${depth}, ${dither}, ${fit}, border ${s.border}, rotated ${rotation}${s.rotate === "auto" ? " (auto)" : ""}, ${refresh} refresh`;
}
function comparePlan(hello) {
  const one = depthChoices(hello).find((c) => c.depth === 1 && !c.color);
  const top = deepest(hello);
  const steps = [];
  const add = (choice, dither) => {
    steps.push({ label: `${choice.color ? "4 colours" : `${choice.depth} bit`} ${DITHER_NAMES[dither]}`, choice, dither });
  };
  if (one) {
    add(one, "none");
    add(one, "floyd");
  }
  if (top !== one) {
    add(top, "none");
    add(top, "floyd");
  }
  return steps;
}
function reportText(r) {
  const lines2 = ["tiny-paper display, picture report", `Device: ${r.hello ? `${deviceLine(r.hello)} ${partialLine(r.hello)}` : "none connected"}`, `Picture: ${r.picture || "none chosen"}`];
  r.sends.forEach((s, i) => lines2.push(`${i + 1}. ${s.label ? `${s.label}: ` : ""}${s.settings}. ${s.ok ? "" : "Not shown: "}${s.answer} Total ${s.totalMs} ms.`));
  if (!r.sends.length) lines2.push("Nothing sent yet.");
  lines2.push(`What I saw: ${r.saw}`);
  return lines2.join("\n");
}

// src/picture/mockPort.ts
function mockPort(device) {
  const port = {
    readable: null,
    writable: null,
    async open() {
      port.readable = new ReadableStream({ start(c) {
        device.onOutput((b) => {
          try {
            c.enqueue(b);
          } catch {
          }
        });
      } });
      port.writable = new WritableStream({ write(chunk2) {
        device.write(chunk2);
      } });
    },
    async close() {
      device.onOutput(() => {
      });
      port.readable = null;
      port.writable = null;
    }
  };
  return port;
}
export {
  BOARD_TITLES,
  BUILTINS,
  BUILTIN_NAMES,
  DEFAULT_SETTINGS,
  DITHER_NAMES,
  DisplayClient,
  DisplayError,
  MAX_SOURCE_PIXELS,
  MOCK_PANELS,
  MockDisplayDevice,
  axisTaps,
  builtinPicture,
  comparePlan,
  compose,
  deepest,
  depthChoices,
  describeSettings,
  deviceLine,
  displayAnswer,
  displayHello,
  levelsToRgba,
  mockPort,
  noise,
  partialLine,
  pickRotation,
  preparePicture,
  preparedPng,
  quantise,
  refreshChoices,
  refreshOptions,
  reportText,
  resamplePlane,
  rotateSource
};
