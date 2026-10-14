# SPDX-License-Identifier: MIT
# USB album uploads with existing diagnostic framing and host PNG conversion.
"""USB album upload using the repository's existing Python link codec.

Keep this example in the tiny-paper checkout: no second CBOR/frame parser.
See docs/spec/link-protocol.md and docs/spec/album.md, Link commands.
"""
import struct
import time
import zlib
from pathlib import Path

from . import codec

ROOT = Path(__file__).resolve().parents[3]


class DisplayError(Exception):
    """A bounded, user-facing failure, including a device error response."""


class Disconnected(DisplayError):
    """The port is gone, stalled or stopped answering: a transfer was cut."""


class NotAuthorized(DisplayError):
    """AUTH (12): a request over Bluetooth before the handshake with the device's key."""


class CableOnly(DisplayError):
    """CABLE (38): the Bluetooth setting and key change only over the USB cable."""


PLAIN = {12: (NotAuthorized, "not authorized: make a Bluetooth key over the USB cable (tiny-paper-display setup --key)"),
         38: (CableOnly, "this works only over the USB cable (the Bluetooth setting and key)")}


def refusal_error(reply):
    """The clear error for AUTH and CABLE answers, else None."""
    if reply.get("err") in PLAIN:
        cls, words = PLAIN[reply["err"]]
        return cls(words)
    return None


def open_port(port=None, bluetooth=None, scan_s=None, key_file=None):
    """The serial port `port`, or the display named or addressed `bluetooth`, with the timeouts the examples use.
    Over Bluetooth the frames are sealed with the device's key: key_file, else the key folder's file of its name
    (bt_key.load); no key: bt_key.NoKey before the radio is used when the name is given."""
    if bluetooth:
        from . import bt_key
        from .ble_port import ADDRESS, SCAN_S, BlePort, load_bleak
        load_bleak()  # a missing library is said first
        bt_key.require()  # the same for cryptography
        by_address = ADDRESS.match(bluetooth) and not key_file
        key = None if by_address else bt_key.load(bluetooth, key_file)  # no key: said before the radio is used
        raw = BlePort(bluetooth, scan_s or SCAN_S)
        try:
            key = key or bt_key.load(raw.name)  # an address: the key file goes by the name it advertises
            opened = bt_key.SealedPort(raw, key["key"])
        except BaseException:
            raw.close()
            raise
    else:
        opened = codec.open_serial(port)
    opened.timeout, opened.write_timeout = 0.05, 2
    return opened


class CheckedPort:
    """Complete short writes and turn disconnects into clean errors."""

    def __init__(self, port):
        self.port = port

    def write(self, data):
        deadline = time.monotonic() + 2
        off = 0
        try:
            while off < len(data):
                n = self.port.write(data[off:])
                if not isinstance(n, int) or not 0 < n <= len(data) - off:
                    raise Disconnected("port closed or stalled mid-transfer")
                off += n
                if time.monotonic() > deadline:
                    raise Disconnected("port write timed out")
        except OSError as exc:
            raise Disconnected(f"port closed mid-transfer: {exc}") from exc
        return off

    def read(self, size):
        try:
            return self.port.read(size)
        except OSError as exc:
            raise Disconnected(f"port closed while waiting for reply: {exc}") from exc


class Display:
    def __init__(self, port, timeout=3):
        self.link = codec.LinkClient(CheckedPort(port))
        self.ble = bool(getattr(port, "is_ble", False))  # lost bytes: an unanswered request is sent again once
        self.timeout = timeout
        self.max_payload = 6144

    def request(self, cmd, data, timeout=None):
        if len(codec.cbor_encode(data)) > self.max_payload:
            raise DisplayError("request exceeds max_payload")
        try:
            reply = self.link.request(cmd, data, timeout or self.timeout, retries=1)
        except (ValueError, TypeError, IndexError, struct.error) as exc:
            raise DisplayError(f"invalid device reply: {exc}") from exc
        if not isinstance(reply, dict):
            raise DisplayError("device reply is not a map")
        if reply.get("_timeout"):
            raise Disconnected(f"no reply for command 0x{cmd:02x} after two bounded attempts")
        if "err" in reply:
            if refusal_error(reply):
                raise refusal_error(reply)
            name = codec.ERR_NAMES.get(reply["err"], str(reply["err"]))
            raise DisplayError(f"device answered {name}: {reply}")
        if "_cbor_error" in reply:
            raise DisplayError(f"invalid device reply: {reply}")
        return reply

    def hello(self):
        self._display_last = None
        self._display_area_last = None
        self._display_known = None
        reply = self.request(0x01, {"proto": 1, "app": "usb-show"})
        self._display_connection = self.link
        self._display_known = reply.get("fingerprint")
        return reply

    def screen(self, hello=None):
        hello = hello or self.hello()
        if hello.get("proto") != 1 or hello.get("album") != 1:
            raise DisplayError("device has no protocol-1 album upload; no direct screen-show command exists")
        cap = hello.get("max_payload")
        if type(cap) is not int or not 64 <= cap <= 6144:
            raise DisplayError("invalid max_payload in hello")
        self.max_payload = cap
        info = self.request(0x28, {})
        screen_size(info)
        return hello, info

    def upload(self, raster, info):
        expected = screen_size(info)
        # Check before album_put_begin: a failed validation must write no flash.
        if len(raster) != expected or raster[:10] != raster_header(info)[:10]:
            raise DisplayError("wrong size or depth for the device's current screen")
        if (raster[10] & 8) != (info.get("flags", 0) & 8):
            raise DisplayError("wrong color-panel flag")
        begin = self.request(0x2A, {"size": len(raster), "crc": zlib.crc32(raster)})
        chunk = begin.get("chunk")
        if type(chunk) is not int or not 1 <= chunk <= 4096:
            raise DisplayError("invalid upload chunk size")
        chunk = min(chunk, self.max_payload - 32)
        for off in range(0, len(raster), chunk):
            data = raster[off:off + chunk]
            reply = self.request(0x2B, {"off": off, "data": data})
            if reply.get("next") != off + len(data):
                raise DisplayError(f"device expected another offset: {reply}")
        return self.request(0x2C, {}, timeout=10)


def screen_size(info):
    w, h, depth = (info.get(k) for k in ("w", "h", "depth"))
    if (type(w) is not int or type(h) is not int or not 1 <= w <= 1872
            or not 1 <= h <= 1872 or type(depth) is not int or depth not in (1, 2, 4)):
        raise DisplayError("invalid screen dimensions or depth")
    if info.get("palette") not in ("gray", "bw", "bwry"):
        raise DisplayError("unknown screen palette")
    if info["palette"] == "bwry" and depth != 2:
        raise DisplayError("BWRY requires depth 2")
    return 20 + h * ((w * depth + 7) // 8)


def raster_header(info):
    flags = 1 | (info.get("flags", 0) & 8) | (4 if info["palette"] == "bwry" else 0)
    return struct.pack("<4sBBHHBBII", b"TIMG", 1, info["depth"], info["w"], info["h"], flags, 0, 0, 0)


def convert(png, info):
    """Fit with white margins, nearest-neighbour sampling and nearest ink.

    Deliberately simple quantization, unlike the web app's photo dithering.
    Uses album_info (rotation/depth/levels), not hello's native panel size.
    """
    from .png_image import decode_png

    size = screen_size(info)
    sw, sh, pixels = decode_png(png)
    w, h, depth = info["w"], info["h"], info["depth"]
    if sw * h > sh * w:
        dw, dh = w, max(1, sh * w // sw)
    else:
        dw, dh = max(1, sw * h // sh), h
    left, top = (w - dw) // 2, (h - dh) // 2
    stride = (w * depth + 7) // 8
    rows = bytearray(size - 20)
    maximum = (1 << depth) - 1
    levels = info.get("levels", (1 << (maximum + 1)) - 1)
    if type(levels) is not int or not levels & ((1 << (maximum + 1)) - 1):
        raise DisplayError("invalid photo levels")
    allowed = [k for k in range(maximum + 1) if levels & (1 << k)]
    palette = ((0, 0, 0), (194, 38, 51), (237, 186, 8), (255, 255, 255))
    for y in range(h):
        for x in range(w):
            rgb = (255, 255, 255)
            if left <= x < left + dw and top <= y < top + dh:
                sx, sy = (x - left) * sw // dw, (y - top) * sh // dh
                rgb = pixels[sy * sw + sx]
            if info["palette"] == "bwry":
                value = min(range(4), key=lambda k: sum((rgb[c] - palette[k][c]) ** 2 for c in range(3)))
            else:
                gray = (299 * rgb[0] + 587 * rgb[1] + 114 * rgb[2] + 500) // 1000
                value = min(allowed, key=lambda k: abs(gray * maximum - k * 255))
            bit = x * depth
            rows[y * stride + bit // 8] |= value << (8 - depth - bit % 8)
    return raster_header(info) + rows
