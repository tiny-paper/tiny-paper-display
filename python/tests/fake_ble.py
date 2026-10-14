# SPDX-License-Identifier: MIT
"""A fake `bleak` for the tests (the real one is not needed): the air between the host and a FakeDevice. Writes to RX
are the host's stream; whole request frames go to the fake firmware, and its answers come back as notifications split
at the sizes given (any byte), or are lost. When the firmware drops the link (a refusal, a frame that did not open),
the host's disconnect callback runs and nothing more of that connection arrives."""
import struct
import types

from tiny_paper_display.ble_port import RX, SERVICE, TX

# made-up addresses (locally administered), put together here so that no file holds a device address
FAKE_ADDRESS, OTHER_ADDRESS = ":".join(["02"] * 6), ":".join(["06"] * 6)


class Air:
    def __init__(self, device, name="tp-display-VEC0", address=FAKE_ADDRESS, mtu=247, notify_sizes=(20,)):
        self.dev, self.name, self.address, self.mtu = device, name, address, mtu
        self.notify_sizes, self.sizes_at = notify_sizes, 0
        self.stream = bytearray()
        self.writes = []            # (piece, response) as the host wrote them
        self.lose_answers = {}      # cmd -> how many of its answers are lost
        self.lose_requests = {}     # cmd -> how many of its requests never arrive
        self.no_room = 0            # that many writes with response fail with ATT error 0x11
        self.client = None
        self.advertises = True
        self.gone = False           # the device dropped this connection
        self.tamper = None          # a function of an answer frame's bytes: what arrives instead

    def write(self, piece, response):
        if self.gone:
            raise RuntimeError("not connected")
        if response and self.no_room:
            self.no_room -= 1
            raise RuntimeError("ATT error 0x11: insufficient resources")
        self.writes.append((bytes(piece), response))
        self.stream += piece
        while len(self.stream) >= 8:
            total = 8 + struct.unpack("<H", self.stream[2:4])[0]
            if len(self.stream) < total:
                return
            wire, self.stream = bytes(self.stream[:total]), self.stream[total:]
            self.request(wire)

    def request(self, wire):
        cmd = wire[5]
        if self.lose_requests.get(cmd):
            self.lose_requests[cmd] -= 1
            return
        answer = self.dev.respond(wire, link="ble")
        if self.lose_answers.get(cmd):
            self.lose_answers[cmd] -= 1
            answer = b""
        if answer and self.tamper:
            answer = self.tamper(answer)
        while answer:
            n = self.notify_sizes[self.sizes_at % len(self.notify_sizes)]
            self.sizes_at += 1
            self.client.notify(answer[:n])
            answer = answer[n:]
        if self.dev.drop:
            self.gone, self.stream = True, bytearray()
            self.client.drop_link()

    def frames(self):
        """The writes grouped as the frames they made: [[(piece, response), ...], ...]; a frame ends at the write with
        response."""
        out, cur = [], []
        for w in self.writes:
            cur.append(w)
            if w[1]:
                out, cur = out + [cur], []
        return out + ([cur] if cur else [])


def install(monkeypatch, air):
    """Put a fake bleak in sys.modules whose scanner finds `air` and whose client talks to it."""
    mod = types.ModuleType("bleak")

    class Device:
        name, address = air.name, air.address

    class Adv:
        local_name, service_uuids = air.name, [SERVICE.upper()]

    class Scanner:
        @staticmethod
        async def find_device_by_name(name, timeout=10.0, **_):
            return Device() if air.advertises and name == air.name else None

        @staticmethod
        async def find_device_by_address(address, timeout=10.0, **_):
            return Device() if air.advertises and address.lower() == air.address.lower() else None

        @staticmethod
        async def discover(timeout=5.0, return_adv=False, **_):
            other = types.SimpleNamespace(name="other", address=OTHER_ADDRESS)
            plain = types.SimpleNamespace(local_name="other", service_uuids=["0000180f-0000-1000-8000-00805f9b34fb"])
            seen = {air.address: (Device(), Adv()), other.address: (other, plain)}
            return seen if return_adv else [d for d, _ in seen.values()]

    class Client:
        def __init__(self, device, disconnected_callback=None, timeout=10.0, **_):
            self.mtu_size, self.gone, self.callback = air.mtu, disconnected_callback, None
            air.client = self

        async def connect(self):
            air.gone = False
            air.dev.new_connection()
            return True

        async def disconnect(self):
            return True

        async def start_notify(self, char, callback):
            assert str(char).lower() == TX
            self.callback = callback

        async def write_gatt_char(self, char, data, response=None):
            assert str(char).lower() == RX and response is not None
            air.write(data, response)

        def notify(self, data):
            self.callback(None, bytearray(data))

        def drop_link(self):
            self.gone(self)

    mod.BleakScanner, mod.BleakClient = Scanner, Client
    monkeypatch.setitem(__import__("sys").modules, "bleak", mod)
    return air
