# SPDX-License-Identifier: MIT
# The display firmware's path: a PNG of the panel's size is shown as it is, with a full refresh.
"""Show a picture on a board running the display firmware (firmware/display, docs/spec/usb-display.md).

Unlike the album path of usb_display.py this needs no plugin, account or WiFi on the device, and the answer of
show_end comes after the panel's refresh. Requests go through the same link client (usb_display.Display).
"""
import struct
import time
import zlib

from .usb_display import DisplayError, convert

HELLO, SHOW_BEGIN, SHOW_CHUNK, SHOW_END, CLEAR = 0x01, 0x08, 0x09, 0x0A, 0x0B
BUSY = 3
NAMES = {1: "UNKNOWN_CMD", 2: "BAD_REQUEST", 3: "BUSY", 10: "IO", 11: "SEQUENCE", 13: "TOO_LARGE", 14: "NOMEM",
         32: "NOT_PNG", 33: "WRONG_SIZE", 34: "DEPTH", 35: "BAD_PNG"}
BWRY = b"\x00\x00\x00\xff\x00\x00\xff\xff\x00\xff\xff\xff"  # palette index = the panel's code: black, red, yellow, white


class Refused(DisplayError):
    """The device's refusal: err is the reason code, msg its text, detail its map."""

    def __init__(self, reply):
        self.err, self.msg, self.detail = reply["err"], reply.get("msg", ""), reply.get("detail") or {}
        super().__init__(f"display refused: {self.msg} ({NAMES.get(self.err, 'code')} {self.err})")


def is_display_firmware(hello):
    return hello.get("proto") == 1 and hello.get("project") == "display"


def request(display, cmd, data=None, timeout=3, busy_wait=60):
    """One request without a resend (this firmware keeps no answer to repeat); a refreshing panel is waited for."""
    end = time.monotonic() + busy_wait
    while True:
        try:
            reply = display.link.request(cmd, data or {}, timeout, retries=0)
        except (ValueError, TypeError, IndexError, struct.error) as exc:
            raise DisplayError(f"invalid device reply: {exc}") from exc
        if not isinstance(reply, dict) or "_cbor_error" in reply:
            raise DisplayError(f"invalid device reply: {reply}")
        if reply.get("_timeout"):
            raise DisplayError(f"no reply for command 0x{cmd:02x} within {timeout} s")
        if "err" not in reply:
            return reply
        if reply["err"] != BUSY or time.monotonic() >= end:
            raise Refused(reply)
        retry = reply.get("detail", {}).get("retry_ms", 2000)
        time.sleep(min(retry, 5000) / 1000 if type(retry) is int else 2)


def show_png(display, png, hello):
    """Send a PNG the panel takes; the answer of show_end ({depth, ms, warn?}) after the refresh."""
    if len(png) > hello["max_png"]:
        raise DisplayError(f"the PNG has {len(png)} bytes, the device takes {hello['max_png']}")
    begin = request(display, SHOW_BEGIN, {"size": len(png)})
    chunk, off = begin.get("chunk"), begin.get("next")
    if type(chunk) is not int or not 29 <= chunk <= 4096 or off != 0:
        raise DisplayError(f"invalid show_begin answer: {begin}")
    while off < len(png):
        data = png[off:off + chunk]
        reply = request(display, SHOW_CHUNK, {"off": off, "data": data})
        if reply.get("next") != off + len(data):
            raise DisplayError(f"device expected another offset: {reply}")
        off += len(data)
    return request(display, SHOW_END, timeout=90)  # a PicPak refresh takes about 18 s


def clear(display):
    return request(display, CLEAR, timeout=90)


def _chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def write_png(w, h, bits, rows, indexed=False):
    """rows: h rows of ceil(w * bits / 8) bytes, left pixel in the high bits (gray levels, or palette codes)."""
    stride = (w * bits + 7) // 8
    if len(rows) != stride * h:
        raise DisplayError("rows do not match the size")
    raw = b"".join(b"\x00" + rows[y * stride:(y + 1) * stride] for y in range(h))
    out = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bits, 3 if indexed else 0, 0, 0, 0))
    if indexed:
        out += _chunk(b"PLTE", BWRY)
    return out + _chunk(b"IDAT", zlib.compress(raw, 9)) + _chunk(b"IEND", b"")


def png_fits(png, hello):
    """True when the device takes this PNG as it is: the panel's size and a gray depth it shows."""
    if len(png) < 29 or png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) > hello["max_png"]:
        return False
    w, h, bits, ctype, _, _, lace = struct.unpack(">IIBBBBB", png[16:29])
    return (w, h, lace, ctype) == (hello["w"], hello["h"], 0, 0) and bits in hello["depths"]


def panel_png(png, hello):
    """Any PNG the example reads, as one the panel takes: itself when it fits, else fitted on white at the panel's
    deepest depth or its four colors by usb_display.convert (nearest level, no dithering)."""
    if png_fits(png, hello):
        return png
    color = bool(hello.get("colors"))
    depth = 2 if color else max(hello["depths"])
    info = {"w": hello["w"], "h": hello["h"], "depth": depth, "palette": "bwry" if color else "gray"}
    return write_png(info["w"], info["h"], depth, bytes(convert(png, info)[20:]), indexed=color)
