# SPDX-License-Identifier: MIT
# The display firmware over Bluetooth Low Energy as a port: read, write, close, like the serial port.
"""A serial-port-like object over BLE for the display firmware (docs/spec/usb-display.md "Bluetooth").

The bytes are the USB link's frames: written to RX in pieces of at most MTU-3 bytes, read from TX as notifications of
any size. `bleak` (pip install tiny-paper-display[bluetooth]) runs its asyncio loop in a thread of its own, so the
callers stay plain blocking code. Radio behaviour on a real device is unproven until a person has tried it.
"""
import asyncio
import re
import threading
import time

from .usb_display import DisplayError

SERVICE = "48770001-4516-40a8-8cc6-57e06fe2ff0d"
RX, TX, PAIR = (f"4877000{n}-4516-40a8-8cc6-57e06fe2ff0d" for n in (2, 3, 4))
SCAN_S = 30  # on battery the device advertises 2 s in every 20 s
MIN_MTU = 23
INSTALL_HINT = "Bluetooth needs the bleak library: pip install tiny-paper-display[bluetooth]"
ADDRESS = re.compile(r"^([0-9a-f]{2}[:-]){5}[0-9a-f]{2}$|^[0-9a-f]{8}-([0-9a-f]{4}-){3}[0-9a-f]{12}$", re.I)
NO_ROOM = "0x11"  # ATT error: the device has no room for a write with response right now: send it again
ROOM_TRIES = 5


def load_bleak():
    try:
        import bleak
    except ImportError as exc:
        raise DisplayError(INSTALL_HINT) from exc
    return bleak


def find(timeout=5):
    """[(name, address)] of the devices advertising the display service."""
    bleak = load_bleak()

    async def scan():
        seen = await bleak.BleakScanner.discover(timeout=timeout, return_adv=True)
        return sorted((adv.local_name or dev.name or "", dev.address) for dev, adv in seen.values()
                      if SERVICE in [u.lower() for u in adv.service_uuids or []])

    try:
        return asyncio.run(scan())
    except Exception as exc:  # bleak raises its own and the system's errors
        raise DisplayError(f"Bluetooth scan failed: {exc}") from exc


class BlePort:
    """Connects to the device with this name or address (scans up to scan_s seconds) and reads Pair once, so that
    the host's system pairs. write() returns when the whole frame is written; read() waits up to .timeout."""

    is_ble = True

    def __init__(self, target, scan_s=SCAN_S):
        self.bleak = load_bleak()
        self.timeout, self.write_timeout = 0.05, 2
        self.mtu, self.pair_error = MIN_MTU, None
        self.buf, self.lost, self.client = bytearray(), False, None
        self.cond = threading.Condition()
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        try:
            self.run(self.connect(target, scan_s), scan_s * 2 + 30)
        except BaseException:
            self.close()
            raise

    def run(self, coro, timeout):
        future = asyncio.run_coroutine_threadsafe(coro, self.loop)
        try:
            return future.result(timeout)
        except DisplayError:
            raise
        except TimeoutError as exc:  # concurrent.futures' own, before 3.11 a different class
            future.cancel()
            raise DisplayError("Bluetooth did not answer in time") from exc
        except Exception as exc:
            raise DisplayError(f"Bluetooth: {exc}") from exc

    async def connect(self, target, scan_s):
        scanner = self.bleak.BleakScanner
        find_one = scanner.find_device_by_address if ADDRESS.match(target) else scanner.find_device_by_name
        found = await find_one(target, timeout=scan_s)
        if found is None:
            raise DisplayError(f"no display named {target} found in {scan_s} s: on battery it advertises only 2 s "
                               f"in every 20 s; is Bluetooth on (tiny-paper-display setup) and the device awake?")
        self.client = self.bleak.BleakClient(found, disconnected_callback=self.on_lost, timeout=scan_s)
        await self.client.connect()
        self.mtu = max(MIN_MTU, int(self.client.mtu_size or MIN_MTU))
        try:  # an encrypted read: the host's system pairs now. Refused: the writes say so later
            await self.client.read_gatt_char(PAIR)
        except Exception as exc:
            self.pair_error = exc
        await self.client.start_notify(TX, self.on_data)

    def on_data(self, _char, data):
        with self.cond:
            self.buf += bytes(data)
            self.cond.notify_all()

    def on_lost(self, _client):
        with self.cond:
            self.lost = True
            self.cond.notify_all()

    async def put(self, piece, response):
        for attempt in range(ROOM_TRIES if response else 1):
            try:
                return await self.client.write_gatt_char(RX, piece, response=response)
            except Exception as exc:
                if NO_ROOM not in str(exc) or attempt == ROOM_TRIES - 1 or not response:
                    raise
                await asyncio.sleep(0.05)

    async def put_frame(self, data):
        step = self.mtu - 3
        for at in range(0, len(data), step):
            await self.put(data[at:at + step], at + step >= len(data))

    def write(self, data):
        if self.lost:
            raise OSError("the Bluetooth link is gone")
        try:
            self.run(self.put_frame(bytes(data)), max(self.write_timeout, len(data) / 100))
        except DisplayError as exc:
            hint = " (not paired? run tiny-paper-display setup with the cable and press the device's button)"
            raise OSError(f"{exc}{hint if self.pair_error else ''}") from exc
        return len(data)

    def read(self, size):
        end = time.monotonic() + self.timeout
        with self.cond:
            while not self.buf and not self.lost and time.monotonic() < end:
                self.cond.wait(max(end - time.monotonic(), 0))
            if not self.buf and self.lost:
                raise OSError("the Bluetooth link is gone")
            out = bytes(self.buf[:size])
            del self.buf[:size]
            return out

    def close(self):
        if self.client is not None:
            try:
                self.run(self.client.disconnect(), 10)
            except DisplayError:
                pass
        if self.loop.is_running():
            self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(5)
        if not self.thread.is_alive():
            self.loop.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
