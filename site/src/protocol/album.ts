// Photo album on the device (docs/spec/album.md): the index file, raster files, limits per board, thumbnails, the
// crop of an upload and the manifest key `album`. Vectors: docs/spec/vectors/album.json.

import { crc32 } from './bytes.ts';
import { parseDuration, validStoreKey } from './rules.ts';

export const ALBUM_VERSION = 1;
export const ALBUM_HDR = 24;
export const ALBUM_ENTRY = 128;
export const ALBUM_CAPTION_MAX = 88;
export const ALBUM_SKIP_MAX = 32;
export const ALBUM_GONE_MAX = 8;
export const ALBUM_THUMB_BYTES = 4096;
export const ALBUM_LIST_PAGE = 16;
export const RASTER_HDR = 20;

/** Raster flags (store.md "Images" byte 10). */
export const RF_FITTED = 1;
export const RF_FILL = 2;
export const RF_BWRY = 4;
export const RF_COLOR = 8;
/** A fitted photo whose bars take its tone (image-fetch.md "Tone"). */
export const RF_TONE = 16;
/** Kind jpeg: a pick could not decode it (an upload, kept; never fits). */
export const RF_BAD = 0x80;

// ------------------------------------------------------------------ the photo file (album.md "Photo file")

export const PHOTO_HDR = 16;
export const KIND_JPEG = 1;
export const KIND_RASTER = 2;
/** A picture as it was served (JPEG or PNG), kept for a layout source (album.md "Layout sources"). */
export const KIND_FILE = 3;
/** Index entry `depth` of kind jpeg, `wall` of a photo file (older firmware never shows either). */
export const DEPTH_JPEG = 8;
export const WALL_FILE = 0xff;

export interface PhotoHeader {
  kind: number;
  /** Bytes of the info map after the header. */
  info: number;
  /** Bytes of the image after the info. */
  n: number;
  w: number;
  h: number;
}

export function encodePhotoHeader(p: PhotoHeader): Uint8Array {
  const b = new Uint8Array(PHOTO_HDR);
  const v = new DataView(b.buffer);
  b.set([0x54, 0x41, 0x50, 0x48, 1, p.kind]); // TAPH
  v.setUint16(6, p.info, true);
  v.setUint32(8, p.n, true);
  v.setUint16(12, p.w, true);
  v.setUint16(14, p.h, true);
  return b;
}

/** null: not a photo file (older firmware's raster starts with TIMG) or a bad header. */
export function decodePhotoHeader(b: Uint8Array): PhotoHeader | null {
  if (b.length < PHOTO_HDR || b[0] !== 0x54 || b[1] !== 0x41 || b[2] !== 0x50 || b[3] !== 0x48 || b[4] !== 1) return null;
  const v = new DataView(b.buffer, b.byteOffset, PHOTO_HDR);
  const p = { kind: b[5]!, info: v.getUint16(6, true), n: v.getUint32(8, true), w: v.getUint16(12, true), h: v.getUint16(14, true) };
  return p.kind >= KIND_JPEG && p.kind <= KIND_FILE && p.info <= 512 && p.n && p.w && p.h ? p : null;
}

/** The largest photo file of kind jpeg for a screen: 1.5 times a 2-bit raster, whole blocks. */
export const jpegMax = (w: number, h: number): number => albumBlocks(Math.floor((rasterSize(w, h, 2) * 3) / 2));

export interface AlbumScreen {
  w: number;
  h: number;
  depth: number;
  color: boolean;
  wall: number;
}

/** talb_fits: an entry shown on this screen (kind jpeg at any depth and palette, unless marked bad). */
export function albumFits(e: Pick<AlbumEntry, 'w' | 'h' | 'depth' | 'flags' | 'wall'>, s: AlbumScreen): boolean {
  if (e.w !== s.w || e.h !== s.h || (e.wall !== 0 && e.wall !== WALL_FILE && e.wall !== s.wall)) return false;
  if (e.depth === DEPTH_JPEG) return !(e.flags & RF_BAD);
  return e.depth === s.depth && !!(e.flags & RF_COLOR) === s.color;
}

export type AlbumSource = 'usb' | 'link';
export type AlbumPalette = 'gray' | 'bw' | 'bwry';

export interface AlbumEntry {
  id: number;
  /** A link photo's image key, 0 for uploads. */
  key: number;
  src: 1 | 2;
  depth: number;
  flags: number;
  wall: number;
  w: number;
  h: number;
  size: number;
  crc: number;
  added: number;
  /** YYYY-MM-DD or "". */
  date: string;
  caption: string;
  /** An older firmware's info sidecar exists (album.md "Info"); absent: no (a photo file holds its info). */
  meta?: boolean;
}

export interface AlbumIndex {
  gen: number;
  next_id: number;
  synced: number;
  /** The window the last album run found (album.md index `want`), 0 unknown. */
  want?: number;
  entries: AlbumEntry[];
  skip: number[];
  gone: number[];
}

const te = new TextEncoder();
const td = new TextDecoder('utf-8', { fatal: false });

export const rasterStride = (w: number, depth: number): number => Math.ceil((w * depth) / 8);
export const rasterSize = (w: number, h: number, depth: number): number => RASTER_HDR + h * rasterStride(w, depth);

/** A caption cut to 88 bytes at a character boundary. */
export const albumCaption = (s: string): string => albumCut(s, ALBUM_CAPTION_MAX);

export const validAlbumDate = (s: string): boolean => /^\d{4}-\d{2}-\d{2}$/.test(s);

export function encodeAlbumIndex(ix: AlbumIndex): Uint8Array {
  const n = ix.entries.length;
  const b = new Uint8Array(ALBUM_HDR + ALBUM_ENTRY * n + 4 + 4 * (ix.skip.length + ix.gone.length) + 4);
  const v = new DataView(b.buffer);
  b.set(te.encode('TALB'), 0);
  b[4] = ALBUM_VERSION;
  v.setUint16(6, n, true);
  v.setUint32(8, ix.gen >>> 0, true);
  v.setUint32(12, ix.next_id >>> 0, true);
  v.setUint32(16, ix.synced >>> 0, true);
  v.setUint32(20, (ix.want ?? 0) >>> 0, true);
  let o = ALBUM_HDR;
  for (const e of ix.entries) {
    v.setUint32(o, e.id >>> 0, true);
    v.setUint32(o + 4, e.key >>> 0, true);
    b[o + 8] = e.src;
    b[o + 9] = e.depth;
    b[o + 10] = e.flags;
    b[o + 11] = e.wall;
    v.setUint16(o + 12, e.w, true);
    v.setUint16(o + 14, e.h, true);
    v.setUint32(o + 16, e.size >>> 0, true);
    v.setUint32(o + 20, e.crc >>> 0, true);
    v.setUint32(o + 24, e.added >>> 0, true);
    b.set(te.encode(e.date).subarray(0, 10), o + 28);
    const c = te.encode(albumCaption(e.caption));
    b[o + 38] = c.length;
    b[o + 39] = e.meta ? 1 : 0;
    b.set(c, o + 40);
    o += ALBUM_ENTRY;
  }
  b[o] = ix.skip.length;
  b[o + 1] = ix.gone.length;
  o += 4;
  for (const k of [...ix.skip, ...ix.gone]) {
    v.setUint32(o, k >>> 0, true);
    o += 4;
  }
  v.setUint32(o, crc32(b.subarray(0, o)), true);
  return b;
}

/** The index, or null when it is not a valid one (album.md "Bad index"). */
export function decodeAlbumIndex(b: Uint8Array): AlbumIndex | null {
  if (b.length < ALBUM_HDR + 8 || td.decode(b.subarray(0, 4)) !== 'TALB' || b[4] !== ALBUM_VERSION) return null;
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const n = v.getUint16(6, true);
  let o = ALBUM_HDR + ALBUM_ENTRY * n;
  if (b.length < o + 8) return null;
  const ns = b[o]!, ng = b[o + 1]!;
  const end = o + 4 + 4 * (ns + ng);
  if (b.length !== end + 4 || v.getUint32(end, true) !== crc32(b.subarray(0, end))) return null;
  const entries: AlbumEntry[] = [];
  for (let i = 0; i < n; i++) {
    const e = ALBUM_HDR + ALBUM_ENTRY * i;
    const d = b.subarray(e + 28, e + 38);
    const entry: AlbumEntry = {
      id: v.getUint32(e, true), key: v.getUint32(e + 4, true), src: b[e + 8] === 2 ? 2 : 1, depth: b[e + 9]!,
      flags: b[e + 10]!, wall: b[e + 11]!, w: v.getUint16(e + 12, true), h: v.getUint16(e + 14, true),
      size: v.getUint32(e + 16, true), crc: v.getUint32(e + 20, true), added: v.getUint32(e + 24, true),
      date: d[0] ? td.decode(d) : '', caption: td.decode(b.subarray(e + 40, e + 40 + Math.min(b[e + 38]!, ALBUM_CAPTION_MAX))),
    };
    if (b[e + 39]) entry.meta = true;
    entries.push(entry);
  }
  o += 4;
  const words = (k: number): number[] => Array.from({ length: k }, () => { const x = v.getUint32(o, true); o += 4; return x; });
  const skip = words(ns);
  const gone = words(ng);
  return { gen: v.getUint32(8, true), next_id: v.getUint32(12, true), synced: v.getUint32(16, true), want: v.getUint32(20, true), entries, skip, gone };
}

// ------------------------------------------------------------------ kept info (album.md "Info")

export const ALBUM_META_MAX = 512;
export const ALBUM_NAME_MAX = 64;
export const ALBUM_META_TEXTS = ['name', 'album', 'camera', 'exposure', 'focal', 'place'] as const;
export const ALBUM_META_NUMS = ['w', 'h', 'bytes'] as const;
const META_CAP: Record<string, number> = { name: 64, album: 64, camera: 64, exposure: 48, focal: 16, place: 64 };

/** A photo's kept info: texts cut at their limit, sizes as uints; every field optional. */
export interface AlbumMetaInfo {
  name?: string;
  album?: string;
  camera?: string;
  exposure?: string;
  focal?: string;
  place?: string;
  w?: number;
  h?: number;
  bytes?: number;
}

/** A text cut to `max` bytes at a character boundary. */
export function albumCut(s: string, max: number): string {
  let out = '';
  let n = 0;
  for (const ch of s) {
    const l = te.encode(ch).length;
    if (n + l > max) break;
    out += ch;
    n += l;
  }
  return out;
}

/** The map the device keeps (album.md "Info"): known fields of the right type, in field order, texts cut; {} when none. */
export function albumMetaPack(m: Record<string, unknown>): AlbumMetaInfo {
  const out: AlbumMetaInfo = {};
  for (const k of ALBUM_META_TEXTS) {
    const v = m[k];
    if (typeof v === 'string' && v.length) out[k] = albumCut(v, META_CAP[k]!);
  }
  for (const k of ALBUM_META_NUMS) {
    const v = m[k];
    if (typeof v === 'number' && Number.isInteger(v) && v >= 0 && v < 2 ** 32) out[k] = v;
  }
  return out;
}

// ------------------------------------------------------------------ budget (album.md "Budget")

const MIB = 1024 * 1024;
export const ALBUM_BLOCK = 4096;
export const albumMax = (fsTotal: number): number => (fsTotal < 2 * MIB ? 16 : 512);
/** Bytes on flash: whole 4 KiB blocks. */
export const albumBlocks = (n: number): number => Math.ceil(n / ALBUM_BLOCK) * ALBUM_BLOCK;
/** The photo budget: free (less the image area's unused part) plus the album, less a quarter of the filesystem. */
export const albumBudget = (fsTotal: number, free: number, album: number): number => Math.max(0, free + album - Math.floor(fsTotal / 4));

/** The budget, one photo of `raster` bytes on flash and how many more fit. */
export function albumRoom(fsTotal: number, free: number, album: number, raster: number): { budget: number; photo: number; more: number } {
  const budget = albumBudget(fsTotal, free, album), photo = albumBlocks(raster);
  return { budget, photo, more: photo > 0 ? Math.floor(Math.max(0, budget - album) / photo) : 0 };
}

// ------------------------------------------------------------------ rasters

export interface RasterHeader {
  w: number;
  h: number;
  depth: number;
  flags: number;
  wall?: number;
  id?: number;
  prev?: number;
}

export function rasterHeader(r: RasterHeader): Uint8Array {
  const b = new Uint8Array(RASTER_HDR);
  const v = new DataView(b.buffer);
  b.set(te.encode('TIMG'), 0);
  b[4] = 1;
  b[5] = r.depth;
  v.setUint16(6, r.w, true);
  v.setUint16(8, r.h, true);
  b[10] = r.flags;
  b[11] = r.wall ?? 0;
  v.setUint32(12, (r.id ?? 0) >>> 0, true);
  v.setUint32(16, (r.prev ?? 0) >>> 0, true);
  return b;
}

/** Pack pixel values (0..2^depth-1, rows top to bottom) as the raster rows (assets.md: first pixel in the high bits). */
export function packRows(px: ArrayLike<number>, w: number, h: number, depth: number): Uint8Array {
  const st = rasterStride(w, depth);
  const out = new Uint8Array(st * h);
  const per = 8 / depth;
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) out[y * st + Math.floor(x / per)]! |= px[y * w + x]! << (8 - depth * ((x % per) + 1));
  return out;
}

export function unpackRows(rows: Uint8Array, w: number, h: number, depth: number): Uint8Array {
  const st = rasterStride(w, depth);
  const per = 8 / depth;
  const mask = (1 << depth) - 1;
  const out = new Uint8Array(w * h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) out[y * w + x] = (rows[y * st + Math.floor(x / per)]! >> (8 - depth * ((x % per) + 1))) & mask;
  return out;
}

/** The upload file: header plus the packed rows of the device's screen (album.md "Browser side"). */
export function albumRasterFile(rows: Uint8Array, r: RasterHeader): Uint8Array {
  if (rows.length !== r.h * rasterStride(r.w, r.depth)) throw new RangeError('rows do not match the size');
  const f = new Uint8Array(RASTER_HDR + rows.length);
  f.set(rasterHeader(r), 0);
  f.set(rows, RASTER_HDR);
  return f;
}

/** The upload flags byte: fitted or filled, BWRY codes, the color panel bit of album_info. */
export const uploadFlags = (mode: 'fit' | 'fill', palette: AlbumPalette, infoFlags: number, tone = false): number =>
  (mode === 'fit' ? RF_FITTED : RF_FILL) | (palette === 'bwry' ? RF_BWRY : 0) | (infoFlags & RF_COLOR) | (tone && mode === 'fit' ? RF_TONE : 0);

// ------------------------------------------------------------------ thumbnails (album_thumb)

export function thumbGeometry(W: number, H: number, depth: number, max = 96): { s: number; w: number; h: number } {
  for (let s = 1; ; s++) {
    const w = Math.ceil(W / s), h = Math.ceil(H / s);
    if (w <= max && h <= max && rasterStride(w, depth) * h <= ALBUM_THUMB_BYTES) return { s, w, h };
  }
}

/** What the device answers: pixel (x s, y s) of the raster rows. */
export function thumbOf(rows: Uint8Array, W: number, H: number, depth: number, max = 96): { w: number; h: number; data: Uint8Array } {
  const g = thumbGeometry(W, H, depth, max);
  const px = unpackRows(rows, W, H, depth);
  const t = new Uint8Array(g.w * g.h);
  for (let y = 0; y < g.h; y++) for (let x = 0; x < g.w; x++) t[y * g.w + x] = px[y * g.s * W + x * g.s]!;
  return { w: g.w, h: g.h, data: packRows(t, g.w, g.h, depth) };
}

/** RGBA pixels of a kind jpeg thumbnail (album_thumb): depth 16 RGB565 little-endian, depth 8 gray. */
export function jpegThumbRGBA(data: Uint8Array, depth: number): Uint8ClampedArray {
  const n = depth === 16 ? data.length >> 1 : data.length;
  const out = new Uint8ClampedArray(n * 4);
  for (let i = 0; i < n; i++) {
    let r: number, g: number, b: number;
    if (depth === 16) {
      const v = data[2 * i]! | (data[2 * i + 1]! << 8);
      r = ((v >> 11) * 255 + 15) / 31;
      g = (((v >> 5) & 63) * 255 + 31) / 63;
      b = ((v & 31) * 255 + 15) / 31;
    } else {
      r = g = b = data[i]!;
    }
    out.set([r, g, b, 255], i * 4);
  }
  return out;
}

/** RGBA pixels of raster values: gray levels, or the BWRY palette (assets.md "Photos"). */
export function albumRGBA(values: Uint8Array, depth: number, bwry: boolean): Uint8ClampedArray {
  const out = new Uint8ClampedArray(values.length * 4);
  const pal = [[0, 0, 0], [194, 38, 51], [237, 186, 8], [255, 255, 255]]; // photo.ts BWRY_PALETTE
  const L = (1 << depth) - 1;
  for (let i = 0; i < values.length; i++) {
    const v = values[i]!;
    const [r, g, b] = bwry && depth === 2 ? pal[v]! : [Math.round((v * 255) / L), Math.round((v * 255) / L), Math.round((v * 255) / L)];
    out[i * 4] = r!;
    out[i * 4 + 1] = g!;
    out[i * 4 + 2] = b!;
    out[i * 4 + 3] = 255;
  }
  return out;
}

// ------------------------------------------------------------------ the browser's crop (album.md "Browser side")

export interface AlbumCrop {
  mode: 'fit' | 'fill';
  /** fill: the crop's center, 0..1 of the room left on each axis (0.5 = centered). */
  x?: number;
  y?: number;
  /** fill: 1 (the largest crop of the screen's aspect) .. 4. */
  zoom?: number;
}

/** The source rectangle the raster is made from: fit takes the whole photo, fill the screen's aspect at x, y, zoom. */
export function cropRect(sw: number, sh: number, W: number, H: number, c: AlbumCrop): { x: number; y: number; w: number; h: number } {
  if (c.mode === 'fit') return { x: 0, y: 0, w: sw, h: sh };
  const zoom = Math.min(4, Math.max(1, c.zoom ?? 1));
  let w: number, h: number;
  if (W * sh >= H * sw) { w = sw; h = (sw * H) / W; } else { h = sh; w = (sh * W) / H; }
  w = Math.max(1, Math.round(w / zoom));
  h = Math.max(1, Math.round(h / zoom));
  const fx = Math.min(1, Math.max(0, c.x ?? 0.5)), fy = Math.min(1, Math.max(0, c.y ?? 0.5));
  return { x: Math.round((sw - w) * fx), y: Math.round((sh - h) * fy), w, h };
}

/** Upload chunks of a file: [offset, bytes]. */
export function* albumChunks(file: Uint8Array, chunk: number): Generator<[number, Uint8Array]> {
  for (let off = 0; off < file.length; off += chunk) yield [off, file.subarray(off, Math.min(file.length, off + chunk))];
}

// ------------------------------------------------------------------ manifest `album` (album.md "Plugin")

export interface AlbumTaskKey {
  when?: unknown;
  keep?: unknown;
  sync?: unknown;
  order?: unknown;
  caption?: unknown;
  date?: unknown;
  index?: unknown;
  count?: unknown;
  clear?: unknown;
  meta?: unknown;
  source?: unknown;
  filling?: unknown;
}

const ALBUM_KEYS = ['when', 'keep', 'sync', 'order', 'caption', 'date', 'index', 'count', 'clear', 'meta', 'source', 'filling'];
const RESERVED = /\.(fetched|image|error|pick|list|skip)$|^sys\./;
const storeKeyOk = (x: unknown): boolean => typeof x === 'string' && validStoreKey(x) && !RESERVED.test(x);

/** Install checks of a steps task's `album` (album.md "Plugin"): [subpath, message] pairs. `fields`: key -> type. */
export function albumKeyProblems(a: unknown, fields: Record<string, string>): [string, string][] {
  const out: [string, string][] = [];
  if (!a || typeof a !== 'object' || Array.isArray(a)) return [['album', 'a map']];
  const m = a as Record<string, unknown>;
  for (const k of Object.keys(m)) if (!ALBUM_KEYS.includes(k)) out.push([`album.${k}`, 'unknown key']);
  const w = m.when as { setting?: unknown; pattern?: unknown } | undefined;
  if (!w || typeof w !== 'object' || typeof w.setting !== 'string' || !['secret', 'string', 'select'].includes(fields[w.setting] ?? '') ||
      (w.pattern !== undefined && typeof w.pattern !== 'string'))
    out.push(['album.when', '{setting, pattern} of a select, string or secret setting']);
  if (m.keep !== undefined && fields[m.keep as string] !== 'number') out.push(['album.keep', 'must name a number setting']);
  if (m.sync !== undefined) {
    const d = typeof m.sync === 'string' ? parseDuration(m.sync) : null;
    if (d === null || d < 60 || d > 7 * 1440) out.push(['album.sync', 'a duration 1h..7d']);
  }
  if (m.order !== undefined && (typeof m.order !== 'string' || m.order.length < 1 || m.order.length > 64)) out.push(['album.order', 'a template']);
  for (const k of ['caption', 'date', 'index', 'count', 'filling'])
    if (m[k] !== undefined && !storeKeyOk(m[k]))
      out.push([`album.${k}`, 'a store key']);
  if (m.clear !== undefined) {
    const c = m.clear;
    if (!Array.isArray(c) || c.length < 1 || c.length > 16 || c.some((x) => !storeKeyOk(x)))
      out.push(['album.clear', '1..16 store keys']);
  }
  if (m.meta !== undefined) {
    const c = m.meta;
    const keys = c && typeof c === 'object' && !Array.isArray(c) ? Object.entries(c as Record<string, unknown>) : null;
    if (!keys || !keys.length || keys.some(([k, v]) => ![...ALBUM_META_TEXTS, ...ALBUM_META_NUMS].includes(k as never) || !storeKeyOk(v)))
      out.push(['album.meta', 'a map of name, album, camera, exposure, focal, place, w, h, bytes to store keys']);
  }
  // album.md "Fill" Source: the settings that name the source
  if (m.source !== undefined) {
    const c = m.source;
    if (!Array.isArray(c) || c.length < 1 || c.length > 16 || c.some((x) => typeof x !== 'string' || !(x in fields)))
      out.push(['album.source', '1..16 setting keys']);
  }
  return out;
}
