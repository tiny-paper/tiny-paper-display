// A pretend device as a Web Serial port: the page talks to it exactly as to a real one (the preview, `?mock=`).
import type { MockDisplayDevice } from '../display/mockDevice.ts';

export function mockPort(device: MockDisplayDevice): { readable: ReadableStream<Uint8Array> | null; writable: WritableStream<Uint8Array> | null; open(): Promise<void>; close(): Promise<void> } {
  const port = {
    readable: null as ReadableStream<Uint8Array> | null,
    writable: null as WritableStream<Uint8Array> | null,
    async open(): Promise<void> {
      port.readable = new ReadableStream<Uint8Array>({ start(c) { device.onOutput((b) => { try { c.enqueue(b); } catch { /* closed */ } }); } });
      port.writable = new WritableStream<Uint8Array>({ write(chunk) { device.write(chunk); } });
    },
    async close(): Promise<void> { device.onOutput(() => {}); port.readable = null; port.writable = null; },
  };
  return port;
}
