// esbuild 0.28.2, 16 source files of the tiny-paper main repository (MIT): do not edit.
// packages/display/src/client.ts
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

// packages/protocol/src/bytes.ts
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

// packages/display/src/png.ts
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

// packages/display/src/sketchModel.ts
var UNDO_STEPS = 10;
function sketchInk(hello) {
  const partial = hello.modes.includes("partial") && hello.partial_depths.length > 0;
  const color = !partial && !!hello.colors;
  const depth = partial ? Math.max(...hello.partial_depths) : color ? 2 : Math.max(...hello.depths);
  const max = (1 << depth) - 1;
  const swatches = [];
  if (color) {
    const value = { black: 0, red: 1, yellow: 2, white: 3 };
    for (const name of hello.colors) swatches.push({ value: value[name], name, rgb: DISPLAY_PALETTE[value[name]] });
  } else for (let v = 0; v <= max; v++) {
    const g = Math.round(v * 255 / max);
    swatches.push({ value: v, name: v === 0 ? "black" : v === max ? "white" : `gray ${v} of ${max}`, rgb: [g, g, g] });
  }
  const more = hello.colors ? "colours" : Math.max(...hello.depths) > depth ? "grays" : "";
  const note = !partial ? "This panel has no partial refresh: the drawing goes out only with the Full refresh button." : more ? `Partial updates on this panel cannot show ${depth === 1 ? more : "more grays"}, so the sketch is ${depth === 1 ? "black and white" : `${max + 1} grays`}.` : "";
  return { depth, color, partial, paper: max, swatches, note };
}
function sketchBrushes(w) {
  return [Math.max(1, Math.round(w / 470)), Math.max(2, Math.round(w / 156)), Math.max(3, Math.round(w / 58))];
}
function alignRect(r, w, h) {
  const x = Math.floor(r.x / 8) * 8, end = Math.ceil((r.x + r.w) / 8) * 8;
  return end > w ? { x: 0, y: 0, w, h } : { x, y: r.y, w: end - x, h: r.h };
}
var grow = (b, x0, y0, x1, y1) => b ? [Math.min(b[0], x0), Math.min(b[1], y0), Math.max(b[2], x1), Math.max(b[3], y1)] : [x0, y0, x1, y1];
var rectOf = (b) => ({ x: b[0], y: b[1], w: b[2] - b[0], h: b[3] - b[1] });
var SketchModel = class {
  constructor(w, h, paper) {
    this.w = w;
    this.h = h;
    this.paper = paper;
    if (!Number.isInteger(w) || !Number.isInteger(h) || w < 1 || h < 1 || w * h > 4194304) throw new Error("The panel size is invalid");
    this.px = new Uint8Array(w * h).fill(paper);
    this.scratch = new Uint8Array(w * h);
  }
  w;
  h;
  paper;
  px;
  /** Called with every rectangle whose pixels changed (the page repaints it). */
  onPaint = () => {
  };
  dirty = null;
  stroke = null;
  before = null;
  scratch;
  undos = [];
  get canUndo() {
    return this.undos.length > 0;
  }
  get hasDirty() {
    return !!this.dirty;
  }
  get whole() {
    return { x: 0, y: 0, w: this.w, h: this.h };
  }
  /** Start one undo step: a stroke, or a clear. */
  begin() {
    this.scratch.set(this.px);
    this.before = this.scratch;
    this.stroke = null;
  }
  /** A round brush `size` pixels wide dragged from one point to the next (the same point twice: a dot). */
  line(x0, y0, x1, y1, size, value) {
    if (![x0, y0, x1, y1].every(Number.isFinite)) return;
    const r = Math.max(0, (size - 1) / 2), reach = Math.ceil(r);
    const steps = Math.min(8192, Math.ceil(Math.hypot(x1 - x0, y1 - y0) / Math.max(1, r / 2)));
    let box = null;
    for (let i = 0; i <= steps; i++) {
      const cx = Math.round(steps ? x0 + (x1 - x0) * i / steps : x0), cy = Math.round(steps ? y0 + (y1 - y0) * i / steps : y0);
      for (let dy = -reach; dy <= reach; dy++) {
        const y = cy + dy;
        if (y < 0 || y >= this.h) continue;
        const half = Math.floor(Math.sqrt(r * r - dy * dy + 0.25));
        if (Number.isNaN(half)) continue;
        for (let x = Math.max(0, cx - half), end = Math.min(this.w - 1, cx + half); x <= end; x++) {
          if (this.px[y * this.w + x] === value) continue;
          this.px[y * this.w + x] = value;
          box = grow(box, x, y, x + 1, y + 1);
        }
      }
    }
    if (box) this.changed(box);
  }
  /** Close the step; it is kept for undo only when a pixel changed. */
  end() {
    const b = this.stroke, before = this.before;
    this.before = this.stroke = null;
    if (!b || !before) return;
    const rect = rectOf(b);
    this.undos.push({ rect, px: this.copy(before, rect) });
    if (this.undos.length > UNDO_STEPS) this.undos.shift();
  }
  clear() {
    this.begin();
    let box = null;
    for (let y = 0; y < this.h; y++) for (let x = 0; x < this.w; x++) if (this.px[y * this.w + x] !== this.paper) box = grow(box, x, y, x + 1, y + 1);
    if (box) {
      this.px.fill(this.paper);
      this.changed(box);
    }
    this.end();
  }
  undo() {
    const u = this.undos.pop();
    if (!u) return false;
    for (let y = 0; y < u.rect.h; y++) this.px.set(u.px.subarray(y * u.rect.w, (y + 1) * u.rect.w), (u.rect.y + y) * this.w + u.rect.x);
    this.before = this.stroke = null;
    this.changed([u.rect.x, u.rect.y, u.rect.x + u.rect.w, u.rect.y + u.rect.h]);
    return true;
  }
  /** The rectangle changed since the last call, aligned for a `show` with x and y; null when nothing changed. */
  takeDirty() {
    const d = this.dirty;
    this.dirty = null;
    return d ? alignRect(rectOf(d), this.w, this.h) : null;
  }
  /** Put a rectangle back (its send failed). */
  markDirty(r) {
    this.dirty = grow(this.dirty, r.x, r.y, r.x + r.w, r.y + r.h);
  }
  /** The pixel values of a rectangle, rows top to bottom. */
  values(r) {
    return this.copy(this.px, r);
  }
  /** The rectangle as the panel shows it. */
  rgba(r, ink) {
    const out = new Uint8ClampedArray(r.w * r.h * 4), rgb = new Map(ink.swatches.map((s) => [s.value, s.rgb]));
    for (let y = 0; y < r.h; y++) for (let x = 0; x < r.w; x++) {
      const c = rgb.get(this.px[(r.y + y) * this.w + r.x + x]) ?? [255, 255, 255];
      out.set([c[0], c[1], c[2], 255], (y * r.w + x) * 4);
    }
    return out;
  }
  copy(from, r) {
    const out = new Uint8Array(r.w * r.h);
    for (let y = 0; y < r.h; y++) out.set(from.subarray((r.y + y) * this.w + r.x, (r.y + y) * this.w + r.x + r.w), y * r.w);
    return out;
  }
  changed(b) {
    this.dirty = grow(this.dirty, ...b);
    if (this.before) this.stroke = grow(this.stroke, ...b);
    this.onPaint(rectOf(b));
  }
};

// packages/display/src/sketchPen.ts
var BRUSH_NAMES = ["Small", "Medium", "Large"];
var SketchPen = class {
  constructor(sender) {
    this.sender = sender;
    this.brushes = sketchBrushes(sender.model.w);
  }
  sender;
  /** Index into `brushes`. */
  size = 1;
  /** The swatch value drawn with while the eraser is off. */
  color = 0;
  eraser = false;
  brushes;
  last = null;
  get canUndo() {
    return this.sender.model.canUndo;
  }
  get value() {
    return this.eraser ? this.sender.ink.paper : this.color;
  }
  get width() {
    return this.brushes[this.size] ?? this.brushes[1];
  }
  /** A pointer went down; false when another one is already drawing. */
  down(id, p) {
    if (this.last) return false;
    this.last = { id, ...p };
    this.sender.model.begin();
    this.sender.begin();
    this.sender.model.line(p.x, p.y, p.x, p.y, this.width, this.value);
    this.sender.changed();
    return true;
  }
  /** The drawing pointer moved through these points. */
  move(id, points) {
    if (this.last?.id !== id) return;
    for (const p of points) {
      this.sender.model.line(this.last.x, this.last.y, p.x, p.y, this.width, this.value);
      this.last = { id, ...p };
    }
    this.sender.changed();
  }
  /** The drawing pointer lifted (or was cancelled); false for any other pointer. */
  up(id) {
    if (this.last?.id !== id) return false;
    this.last = null;
    this.sender.model.end();
    this.sender.lift();
    return true;
  }
  clear() {
    this.sender.model.clear();
    this.sender.lift();
  }
  undo() {
    if (this.sender.model.undo()) this.sender.lift();
  }
};

// packages/protocol/src/album.ts
var te = new TextEncoder();
var td = new TextDecoder("utf-8", { fatal: false });
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

// packages/display/src/sketchSender.ts
var SKETCH_TIMING = { liftMs: 150, everyMs: 700, busyMs: 5e3 };
var FULL_AFTER = 30;
var LOST = ["first", "refused", "cut", "failed", "memory", "depth", "board"];
var sleep = (ms) => new Promise((r) => setTimeout(r, ms));
var SketchSender = class {
  constructor(model, ink, timing = SKETCH_TIMING) {
    this.model = model;
    this.ink = ink;
    this.timing = timing;
  }
  model;
  ink;
  timing;
  /** Partial sends before an automatic full refresh; 0: never. */
  fullAfter = FULL_AFTER;
  onStatus = () => {
  };
  status = { state: "idle", text: "Nothing sent yet." };
  client = null;
  hello = null;
  link = 0;
  timer = null;
  due = 0;
  sending = false;
  drawing = false;
  whole = true;
  asked = false;
  partials = 0;
  strokeAt = 0;
  sentAt = 0;
  /** A device (again): it shows something else, so the whole drawing goes out once. null: no device. */
  attach(client, hello = null) {
    this.client = client;
    this.hello = hello;
    this.link++;
    this.whole = true;
    this.partials = 0;
    this.drawing = false;
    this.cancel();
    if (client && this.ink.partial) this.schedule(Date.now(), true);
    else if (!client) this.say("idle", "No device connected: the drawing is kept and sent when one connects.");
  }
  /** The pen went down. */
  begin() {
    this.drawing = true;
    this.strokeAt = Date.now();
  }
  /** The drawing changed under the pen: at most one send every `everyMs`. */
  changed() {
    if (this.drawing) this.schedule(Math.max(this.strokeAt, this.sentAt) + this.timing.everyMs, false);
  }
  /** The pen lifted, or an undo or clear changed the drawing: send shortly. */
  lift() {
    this.drawing = false;
    this.schedule(Date.now() + this.timing.liftMs, true);
  }
  /** Send the whole drawing in full mode. */
  fullRefresh() {
    this.asked = true;
    this.schedule(Date.now(), true);
  }
  stop() {
    this.cancel();
  }
  cancel() {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
  }
  schedule(at, replace) {
    if (this.timer && !replace && this.due <= at) return;
    this.cancel();
    this.due = at;
    this.timer = setTimeout(() => {
      this.timer = null;
      void this.flush();
    }, Math.max(0, at - Date.now()));
  }
  say(state, text) {
    this.status = { state, text };
    this.onStatus(this.status);
  }
  async flush() {
    const client = this.client, hello = this.hello, link = this.link, m = this.model;
    if (!client || !hello || this.sending) return;
    const full = this.asked || this.ink.partial && (this.whole || this.fullAfter > 0 && this.partials >= this.fullAfter);
    if (!full && !this.ink.partial) return;
    const dirty = m.takeDirty();
    if (!full && !dirty) return;
    const rect = full ? m.whole : dirty;
    const all = rect.w === m.w && rect.h === m.h;
    this.asked = false;
    this.sending = true;
    this.sentAt = Date.now();
    const rows = packRows(m.values(rect), rect.w, rect.h, this.ink.depth);
    const what = full ? `a full refresh of the whole drawing (${m.w} x ${m.h})` : `a partial update of ${rect.w} x ${rect.h} pixels at ${rect.x}, ${rect.y}`;
    this.say("sending", `Asked for ${what}; waiting for the panel...`);
    try {
      const png = await packedPng(rows, rect.w, rect.h, this.ink.depth, this.ink.color);
      const options = full ? { mode: "full" } : all ? { mode: "partial" } : { x: rect.x, y: rect.y, mode: "partial" };
      const r = await this.showOnce(client, hello, png, options, what);
      if (link !== this.link) return;
      const lost = !full && r.mode === "full" && LOST.includes(String(r.why));
      if (full || all) this.whole = false;
      if (lost && !all) this.whole = true;
      if (r.mode === "full") this.partials = 0;
      else if (r.mode !== "none") this.partials++;
      this.say("ok", `Asked for ${what}. ${displayAnswer(r)} ${Date.now() - this.sentAt} ms in all.${lost && !all ? " The device had lost its picture: sending the whole drawing." : ""}`);
    } catch (e) {
      if (link !== this.link) return;
      if (dirty) m.markDirty(dirty);
      if (full) this.asked = true;
      this.whole = true;
      this.say("error", `Asked for ${what}. Not shown: ${e.message}. ${this.client === client ? "Draw on or press Full refresh to send the whole drawing again." : ""}`.trim());
      return;
    } finally {
      this.sending = false;
      if (link !== this.link && this.client && this.ink.partial) this.schedule(Date.now(), true);
    }
    const again = this.whole || m.hasDirty || this.asked || this.fullAfter > 0 && this.partials >= this.fullAfter;
    if (again && this.ink.partial) this.schedule(this.drawing && !this.whole ? this.sentAt + this.timing.everyMs : Date.now(), false);
  }
  /** BUSY: wait as long as the device says (bounded) and try once more. */
  async showOnce(client, hello, png, options, what) {
    try {
      return await client.show(png, hello, options);
    } catch (e) {
      if (!(e instanceof DisplayError) || e.codeName !== "BUSY") throw e;
      const ms = Math.min(this.timing.busyMs, Math.max(100, Number(e.detail?.retry_ms) || 2e3));
      this.say("sending", `Asked for ${what}. The panel is busy: trying once more in ${(ms / 1e3).toFixed(1)} s...`);
      await sleep(ms);
      return client.show(png, hello, options);
    }
  }
};
var kept = null;
function sketchSession(hello) {
  const ink = sketchInk(hello);
  const key = [hello.w, hello.h, ink.depth, ink.color, ink.partial].join();
  if (kept?.key !== key) kept = { key, sender: new SketchSender(new SketchModel(hello.w, hello.h, ink.paper), ink) };
  return kept.sender;
}

// packages/protocol/src/cbor.ts
var CborError = class extends Error {
  name = "CborError";
};
var MAX_DEPTH = 8;
var CBOR_KEY_WORK_MAX = 4194304;
var te2 = new TextEncoder();
var td2 = new TextDecoder("utf-8", { fatal: true });
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
    const b = te2.encode(v);
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
          s = td2.decode(this.b.subarray(this.pos, this.pos + n));
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

// packages/protocol/src/link-frame.ts
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

// packages/display/src/mockDevice.ts
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
  /** `x`, `og`, `picpak`: that board's panel as its hello describes it (usb-display.md "Boards"); else a 16 x 8 test panel. */
  constructor(panel = "") {
    if (panel === "x") Object.assign(this.hello, {
      board: "trmnl_x",
      w: 1872,
      h: 1404,
      depths: [1, 2, 4],
      max_png: 1319644,
      modes: ["full", "partial", "partial_all", "inverse", "passes"]
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
      if (cmd === 1) res = this.hello;
      else if (this.error) {
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
          } else if (b.mode === "partial") res = { mode: "partial", why: "changed", depth: show.bits, ms: 0, x: b.x ?? 0, y: b.y ?? 0, w: show.w, h: show.h };
        }
      } else res = { err: 1, msg: "not a display command" };
      setTimeout(() => this.output(encodeFrame({ seq, cmd: cmd | 128, payload: cborEncode(res) })), 1);
    }
  }
};
export {
  BRUSH_NAMES,
  DISPLAY_ERRORS,
  DISPLAY_MODES,
  DISPLAY_PALETTE,
  DisplayClient,
  DisplayError,
  FULL_AFTER,
  MOCK_PANELS,
  MockDisplayDevice,
  SKETCH_TIMING,
  SketchModel,
  SketchPen,
  SketchSender,
  UNDO_STEPS,
  alignRect,
  displayAnswer,
  displayHello,
  packedPng,
  sketchBrushes,
  sketchInk,
  sketchSession
};
