// A panel PNG from packed rows (docs/spec/usb-display.md "The picture"): grayscale, or indexed with the panel palette.
import { crc32 } from '../protocol/bytes.ts';

function chunk(name: string, data: Uint8Array): Uint8Array {
  const b = new Uint8Array(data.length + 12);
  const v = new DataView(b.buffer);
  v.setUint32(0, data.length);
  b.set(new TextEncoder().encode(name), 4);
  b.set(data, 8);
  v.setUint32(b.length - 4, crc32(b.subarray(4, b.length - 4)));
  return b;
}
/** The palette of an indexed panel PNG, by pixel value. */
export const DISPLAY_PALETTE = [[0, 0, 0], [255, 0, 0], [255, 255, 0], [255, 255, 255]] as const;
/** Packed rows (first pixel in the high bits) of any rectangle as a panel PNG: grayscale, or indexed with the panel palette. */
export async function packedPng(rows: Uint8Array, w: number, h: number, depth: number, color: boolean): Promise<Uint8Array> {
  const stride = Math.ceil(w * depth / 8);
  const raw = new Uint8Array((stride + 1) * h);
  for (let y = 0; y < h; y++) raw.set(rows.subarray(y * stride, (y + 1) * stride), y * (stride + 1) + 1);
  const compressed = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'))).arrayBuffer());
  const ihdr = new Uint8Array(13);
  const v = new DataView(ihdr.buffer);
  v.setUint32(0, w); v.setUint32(4, h);
  ihdr[8] = depth; ihdr[9] = color ? 3 : 0;
  const parts = [Uint8Array.of(137, 80, 78, 71, 13, 10, 26, 10), chunk('IHDR', ihdr)];
  if (color) parts.push(chunk('PLTE', Uint8Array.from(DISPLAY_PALETTE.flat())));
  parts.push(chunk('IDAT', compressed), chunk('IEND', new Uint8Array()));
  const png = new Uint8Array(parts.reduce((n, b) => n + b.length, 0));
  let off = 0;
  for (const b of parts) { png.set(b, off); off += b.length; }
  return png;
}
