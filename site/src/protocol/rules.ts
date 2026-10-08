// Durations, cron dialect, name rules (manifest.md, host-abi.md). Ported one to one
// from firmware/components/common/src/util.c so both sides accept the same inputs.

/** "15m" | "6h" | "1d" -> minutes; "0m" only when allowZero. null when invalid. */
export function parseDuration(s: string, allowZero = false): number | null {
  if (s === '0m') return allowZero ? 0 : null;
  const m = /^([1-9][0-9]{0,3})(m|h|d)$/.exec(s);
  if (!m) return null;
  const n = Number(m[1]);
  return m[2] === 'm' ? n : m[2] === 'h' ? n * 60 : n * 1440;
}

/** manifest.md "Full refresh": manifest `full_refresh` and the settings key `_full_refresh`. */
export type FullRefresh = number | 'always' | 'never';
export const FULL_REFRESH_KEY = '_full_refresh';
/** manifest.md "Settings documents": `_notify: false` turns the plugin's notifications off ("Notifications"). */
export const NOTIFY_KEY = '_notify';

/** "always", "never" or a duration 1m..1d (minutes); null when invalid. */
export function parseFullRefresh(s: unknown): FullRefresh | null {
  if (s === 'always' || s === 'never') return s;
  if (typeof s !== 'string') return null;
  const d = parseDuration(s);
  return d !== null && d >= 1 && d <= 1440 ? d : null;
}

export interface Cron {
  minute: bigint; // bit n = minute n
  hour: number; // bits 0..23
  dom: number; // bits 1..31
  month: number; // bits 1..12
  dow: number; // bits 0..6, Sunday = 0 (7 folded into 0)
  domAny: boolean;
  dowAny: boolean;
}

function parseNum(s: string, p: { i: number }, end: number): number | null {
  let n = 0;
  let digits = 0;
  while (p.i < end && /[0-9]/.test(s[p.i]!) && digits < 3) {
    n = n * 10 + (s.charCodeAt(p.i) - 48);
    p.i++;
    digits++;
  }
  if (digits === 0 || (p.i < end && /[0-9]/.test(s[p.i]!))) return null;
  return n;
}

function parseField(s: string, start: number, end: number, lo: number, hi: number): { bits: bigint; any: boolean } | null {
  const p = { i: start };
  let bits = 0n;
  const any = end - start === 1 && s[start] === '*';
  while (p.i < end) {
    let a: number;
    let b: number;
    let step = 1;
    let star = false;
    let range = false;
    if (s[p.i] === '*') {
      star = true;
      a = lo;
      b = hi;
      p.i++;
    } else {
      const x = parseNum(s, p, end);
      if (x === null) return null;
      a = b = x;
      if (p.i < end && s[p.i] === '-') {
        range = true;
        p.i++;
        const y = parseNum(s, p, end);
        if (y === null) return null;
        b = y;
      }
    }
    if (p.i < end && s[p.i] === '/') {
      p.i++;
      const st = parseNum(s, p, end);
      if (st === null || st === 0) return null;
      step = st;
      if (!star && !range) b = hi;
    }
    if (a < lo || b > hi || a > b) return null;
    for (let v = a; v <= b; v += step) bits |= 1n << BigInt(v);
    if (p.i < end) {
      if (s[p.i] !== ',' || p.i + 1 === end) return null;
      p.i++;
    }
  }
  return p.i === end && start !== end ? { bits, any } : null;
}

/** Parse the 5-field dialect. null when invalid (same acceptance as tiny_cron_parse). */
export function parseCron(s: string): Cron | null {
  const lo = [0, 0, 1, 1, 0];
  const hi = [59, 23, 31, 12, 7];
  // The firmware works on bytes; a non-ASCII byte is never valid there either.
  if (s.length === 0 || new TextEncoder().encode(s).length > 64) return null;
  const fields: { bits: bigint; any: boolean }[] = [];
  let p = 0;
  const end = s.length;
  for (let f = 0; f < 5; f++) {
    if (p >= end || s[p] === ' ') return null;
    let fe = p;
    while (fe < end && s[fe] !== ' ') fe++;
    const r = parseField(s, p, fe, lo[f]!, hi[f]!);
    if (!r) return null;
    fields.push(r);
    p = fe;
    if (f < 4) {
      if (p >= end) return null;
      p++;
    }
  }
  if (p !== end) return null;
  const dowBits = Number(fields[4]!.bits);
  return {
    minute: fields[0]!.bits,
    hour: Number(fields[1]!.bits),
    dom: Number(fields[2]!.bits),
    month: Number(fields[3]!.bits),
    dow: (dowBits | (dowBits >> 7)) & 0x7f,
    domAny: fields[2]!.any,
    dowAny: fields[4]!.any,
  };
}

export interface WallTime {
  month: number;
  day: number;
  hour: number;
  minute: number;
  wday: number;
}

export function cronMatch(c: Cron, t: WallTime): boolean {
  if (!((c.minute >> BigInt(t.minute)) & 1n) || !((c.hour >> t.hour) & 1) || !((c.month >> t.month) & 1)) return false;
  const dom = ((c.dom >> t.day) & 1) === 1;
  const dow = ((c.dow >> t.wday) & 1) === 1;
  if (c.domAny || c.dowAny) return dom && dow;
  return dom || dow;
}

/** Minimum circular gap of the minute field (60 for a single value). */
export function cronMinGap(c: Cron): number {
  let first = -1;
  let prev = -1;
  let gap = 60;
  for (let m = 0; m < 60; m++) {
    if (!((c.minute >> BigInt(m)) & 1n)) continue;
    if (prev >= 0 && m - prev < gap) gap = m - prev;
    if (first < 0) first = m;
    prev = m;
  }
  if (first >= 0 && prev !== first && first + 60 - prev < gap) gap = first + 60 - prev;
  return gap;
}

// ------------------------------------------------------------------ names

const LABEL = /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/;

function labels(s: string): string[] | null {
  const parts = s.split('.');
  return parts.every((l) => LABEL.test(l)) ? parts : null;
}

export function validPluginId(s: string): boolean {
  const l = labels(s);
  return s.length >= 3 && s.length <= 64 && l !== null && l.length >= 2;
}

const BAD_HOST_SUFFIX = ['.local', '.localhost', '.internal', '.lan', '.home.arpa', '.arpa'];

export function validHost(s: string): boolean {
  if (s.length < 4 || s.length > 253) return false;
  const l = labels(s);
  if (!l || l.length < 2 || !/^[a-z]+$/.test(l[l.length - 1]!)) return false;
  return !BAD_HOST_SUFFIX.some((x) => s.endsWith(x));
}

export const validTaskName = (s: string): boolean => /^[a-z][a-z0-9_]{0,15}$/.test(s);
export const validSettingKey = (s: string): boolean => /^[a-z][a-z0-9_]{0,31}$/.test(s);
export const validStoreKey = (s: string): boolean => /^[A-Za-z0-9_.-]{1,32}$/.test(s);

export const SEMVER = /^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]{1,20})?$/;

export interface Semver {
  major: number;
  minor: number;
  patch: number;
  pre?: string;
}

export function parseSemver(s: string): Semver | null {
  const m = SEMVER.exec(s);
  if (!m) return null;
  const [major, minor, patch] = [Number(m[1]), Number(m[2]), Number(m[3])];
  if (major > 65535 || minor > 65535 || patch > 65535) return null;
  return m[4] ? { major, minor, patch, pre: m[4].slice(1) } : { major, minor, patch };
}

/** `>=MAJOR.MINOR` (api) or `>=MAJOR.MINOR[.PATCH]` (firmware). */
export function parseRequirement(s: string, allowPatch: boolean): { major: number; minor: number; patch: number } | null {
  const m = (allowPatch ? /^>=(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:\.(0|[1-9][0-9]*))?$/ : /^>=(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$/).exec(s);
  if (!m) return null;
  return { major: Number(m[1]), minor: Number(m[2]), patch: m[3] ? Number(m[3]) : 0 };
}

/** API accepted when host major == MAJOR and host minor >= MINOR. */
export function apiSatisfied(req: string, host = '1.0'): boolean {
  const r = parseRequirement(req, false);
  const [maj, min] = host.split('.').map(Number);
  return !!r && r.major === maj && (min ?? 0) >= r.minor;
}

/** Semver comparison of a firmware version with `>=X.Y[.Z]` (pre-release sorts below). */
export function firmwareSatisfied(req: string, fw: string): boolean {
  const r = parseRequirement(req, true);
  const v = parseSemver(fw);
  if (!r || !v) return false;
  const a = [v.major, v.minor, v.patch];
  const b = [r.major, r.minor, r.patch];
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i]! > b[i]!;
  return v.pre === undefined;
}
