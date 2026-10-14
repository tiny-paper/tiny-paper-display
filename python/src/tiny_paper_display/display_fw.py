# SPDX-License-Identifier: MIT
# The display firmware's path: a PNG of the panel's size, or of a rectangle of it, is shown as it is.
"""Show a picture on a board running the display firmware (firmware/display, docs/spec/usb-display.md).

Unlike the album path of usb_display.py this needs no plugin, account or WiFi on the device, and the answer of
show_end comes after the panel's refresh. Requests go through the same link client (usb_display.Display).

The device keeps what it last showed: without a mode it updates only what changed (a partial refresh, no flash) and
does a full refresh by itself when it must. The answer says which ran and why: {"mode": .., "why": .., "x", "y", "w",
"h", "ms", "ckv"?, "steps"?}; steps: the device's ms for each step of the show.
"""
from dataclasses import dataclass
import struct
import time
import zlib

import logging

from .usb_display import Disconnected, DisplayError, convert, refusal_error
from .display_fingerprint import fingerprint

HELLO, SHOW_BEGIN, SHOW_CHUNK, SHOW_END, CLEAR, SET, BT, HAVE = 0x01, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E
BUSY, SEQUENCE = 3, 11
NAMES = {1: "UNKNOWN_CMD", 2: "BAD_REQUEST", 3: "BUSY", 10: "IO", 11: "SEQUENCE", 12: "AUTH", 13: "TOO_LARGE", 14: "NOMEM",
         32: "NOT_PNG", 33: "WRONG_SIZE", 34: "DEPTH", 35: "BAD_PNG", 36: "MODE", 37: "RECT", 38: "CABLE"}
MODES = ("full", "partial", "partial_all", "inverse", "passes")
BWRY = b"\x00\x00\x00\xff\x00\x00\xff\xff\x00\xff\xff\xff"  # palette index = the panel's code: black, red, yellow, white


log = logging.getLogger(__name__)


class Refused(DisplayError):
    """The device's refusal: err is the reason code, name its name (MODE 36), msg its text, detail its map."""

    def __init__(self, reply):
        self.err, self.msg, self.detail = reply["err"], reply.get("msg", ""), reply.get("detail") or {}
        self.name = NAMES.get(self.err, "code")
        super().__init__(f"display refused: {self.msg} ({self.name} {self.err})")


class Busy(Refused):
    """BUSY for longer than the wait: the panel is still refreshing (or another transfer holds the device)."""


class PanelFault(DisplayError):
    """The device counted a wrong gate clock: the picture is not where it should be. reply: the device's answer."""

    def __init__(self, reply):
        self.reply = reply
        super().__init__("the device counted a wrong gate clock: the picture is not where it should be")


def _brief(data):
    """A map for the log: bytes are shown by their length, never their content."""
    return {k: (f"<{len(v)} bytes>" if isinstance(v, (bytes, bytearray)) else v) for k, v in (data or {}).items()}


def is_display_firmware(hello):
    return hello.get("proto") == 1 and hello.get("project") == "display"


def request(display, cmd, data=None, timeout=3, busy_wait=60):
    """One request; a refreshing panel is waited for. Over USB without a resend (this firmware keeps no answer to
    repeat); over Bluetooth bytes get lost, so an unanswered request is sent once more ("Lost bytes")."""
    end = time.monotonic() + busy_wait
    while True:
        if log.isEnabledFor(logging.DEBUG):
            log.debug("request 0x%02x %s", cmd, _brief(data))
        try:
            reply = display.link.request(cmd, data or {}, timeout, retries=int(display.ble))
        except (ValueError, TypeError, IndexError, struct.error) as exc:
            raise DisplayError(f"invalid device reply: {exc}") from exc
        if not isinstance(reply, dict) or "_cbor_error" in reply:
            raise DisplayError(f"invalid device reply: {reply}")
        if reply.get("_timeout"):
            display._display_last = None
            raise Disconnected(f"no reply for command 0x{cmd:02x} within {timeout} s")
        if log.isEnabledFor(logging.DEBUG):
            log.debug("answer 0x%02x %s", cmd, _brief(reply))
        if "err" not in reply:
            return reply
        if refusal_error(reply):
            raise refusal_error(reply)
        if reply["err"] != BUSY or time.monotonic() >= end:
            display._display_last = None
            raise (Busy if reply["err"] == BUSY else Refused)(reply)
        retry = reply.get("detail", {}).get("retry_ms", 2000)
        time.sleep(min(retry, 5000) / 1000 if type(retry) is int else 2)


def target(hello, x=None, y=None, mode=None, passes=None):
    """The keys of show_begin after size, checked against what the device said it does (hello.modes)."""
    if (x is None) != (y is None):
        raise DisplayError("x and y come together")
    if mode is not None and mode not in MODES:
        raise DisplayError(f"mode is one of {', '.join(MODES)}")
    if (passes is not None) != (mode == "passes"):
        raise DisplayError("passes comes with the mode passes, and only with it")
    can = hello.get("modes")
    if can is None and (x is not None or mode not in (None, "full")):
        raise DisplayError("this display firmware has no area updates or refresh modes: update it")
    if mode is not None and can is not None and mode not in can:
        raise DisplayError(f"{hello.get('board', 'this board')} does not do {mode} (it does {', '.join(can)})")
    if x is not None and not hello.get("partial_depths"):
        raise DisplayError(f"{hello.get('board', 'this board')} has no partial refresh: send the whole picture")
    out = {} if x is None else {"x": x, "y": y}
    if mode is not None and can is not None:
        out["mode"] = mode
    if passes is not None:
        out["passes"] = passes
    return out


def show_png(display, png, hello, x=None, y=None, mode=None, passes=None, force=False):
    """Send a PNG the panel takes; the answer of show_end after the refresh: {depth, ms, mode, why, x, y, w, h, ...}.

    Without a mode the device updates only what changed and does a full refresh when it has to; mode: "full",
    "partial", "partial_all", "inverse" or "passes" (with passes=1..4) asks for that one, and a board that cannot do
    it refuses. x, y: the PNG is a rectangle with its top left pixel there (show_area)."""
    # A hello belongs to this link only. New/replaced links cannot use a caller's old hello or cache.
    if getattr(display, "_display_connection", None) is not display.link:
        hello = display.hello()
    if len(png) > hello["max_png"]:
        raise DisplayError(f"the PNG has {len(png)} bytes, the device takes {hello['max_png']}")
    options = target(hello, x, y, mode, passes)
    value = None
    if hello.get("fingerprint_hash") == "fnv1a64":
        try:
            value = fingerprint(png, hello)
        except (ValueError, KeyError, IndexError, struct.error):
            pass  # Let the device give its bounded, specific refusal for an invalid PNG.
    try:
        previous = getattr(display, "_display_last", None)
        area_key = (value, x, y)
        cached_area = getattr(display, "_display_area_last", None)
        panel_value = value if x is None else (cached_area[1] if cached_area and cached_area[0] == area_key else None)
        if (value and not force and mode in (None, "partial")
                and panel_value and (x is not None or value in (hello.get("fingerprint"), previous,
                                                               getattr(display, "_display_known", None)))):
            # A cut/refused transfer may have invalidated the frame since hello or our last send.
            if request(display, HAVE, {"fingerprint": panel_value}).get("same") is True:
                return {"same": True, "skipped": True, "mode": "none", "why": "unchanged", "ms": 0,
                        "depth": 2 if hello.get("colors") else png[24], "x": 0, "y": 0, "w": 0, "h": 0}
        display._display_last = None
        display._display_area_last = None
        begin = request(display, SHOW_BEGIN, {"size": len(png), **options})
        chunk, off = begin.get("chunk"), begin.get("next")
        if type(chunk) is not int or not 29 <= chunk <= 4096 or off != 0:
            raise DisplayError(f"invalid show_begin answer: {begin}")
        while off < len(png):
            data = png[off:off + chunk]
            try:
                reply = request(display, SHOW_CHUNK, {"off": off, "data": data})
            except Refused as r:  # the first copy arrived and its answer was lost: go on where the device is
                if not (display.ble and r.err == SEQUENCE and off < r.detail.get("next", 0) <= off + len(data)):
                    raise
                off = r.detail["next"]
                continue
            if reply.get("next") != off + len(data):
                raise DisplayError(f"device expected another offset: {reply}")
            off += len(data)
        reply = request(display, SHOW_END, timeout=90)  # a PicPak refresh takes about 18 s
        if x is None:
            display._display_last = value
            display._display_known = value
        elif value:
            # An area fingerprint is only a cache key. Preflight always uses the whole panel's fingerprint.
            current = request(display, HELLO)
            display._display_known = current.get("fingerprint")
            display._display_area_last = (area_key, display._display_known)
        return reply
    except Exception:
        display._display_last = None
        display._display_area_last = None
        display._display_known = None
        raise


def show_area(display, png, hello, x, y, mode=None, passes=None, force=False):
    """A rectangle of the panel: the PNG (its width and x multiples of 8) with its top left pixel at x, y."""
    return show_png(display, png, hello, x, y, mode, passes, force)


def clear(display):
    display._display_last = None
    display._display_area_last = None
    display._display_known = None
    return request(display, CLEAR, timeout=90)


def full_every(display, n=None, clear=False):
    """The device's setting: a full refresh after this many partial ones (0: never; the default is 0, clean_every does the work).
    n: store a new value. clear=True: remove the stored value, the default applies again. Returns the value in use."""
    if clear and n is not None:
        raise DisplayError("full_every: n or clear, not both")
    return request(display, SET, {"full_every": None} if clear else {} if n is None else {"full_every": n})["full_every"]


CLEANUP_KEYS = {"areas": "clean_every", "screen": "full_every"}  # the setting's keys and the protocol's


def cleanup_payload(value):
    """`cleanup` as the keys of a set request. None: both null (clear the stored values, the defaults apply). A dict
    {"areas": N, "screen": M}: only the keys given (0..250, 0 never); the others keep their value."""
    if value is None:
        return {"clean_every": None, "full_every": None}
    if not isinstance(value, dict):
        raise DisplayError('cleanup: None or a dict like {"areas": 15, "screen": 0}')
    unknown = set(value) - set(CLEANUP_KEYS)
    if unknown:
        raise DisplayError(f"cleanup: unknown key {', '.join(sorted(map(str, unknown)))} (areas, screen)")
    out = {}
    for k, v in value.items():
        if type(v) is not int or not 0 <= v <= 250:
            raise DisplayError(f"cleanup: {k} is a whole number 0 to 250 (0 never), not {v!r}")
        out[CLEANUP_KEYS[k]] = v
    return out


def cleanup_of(full_every, clean_every):
    """The `cleanup` dict for the device's two values; None while the firmware reports no full_every."""
    if full_every is None:
        return None
    return {"areas": 0 if clean_every is None else clean_every, "screen": full_every}  # no clean_every: an older firmware


def set_cleanup(display, value):
    """Store `cleanup` (cleanup_payload) on the device in one request. Returns the device's answer."""
    return request(display, SET, cleanup_payload(value))


@dataclass(frozen=True)
class Capabilities:
    """What hello says the board can do. depth_modes: {depth: [modes]}, None on a firmware from before the field
    (then try a mode and read the refusal). full_every_set: False while the default is in use, None when unknown."""
    w: int
    h: int
    depths: list
    partial_depths: list
    depth_modes: dict
    colors: list
    max_png: int
    full_every: int
    full_every_set: bool = None
    modes: list = None
    clean_every: int = None
    clean_every_set: bool = None

    @property
    def cleanup(self):
        """The one setting for ghosting, {"areas": N, "screen": M} (None: unknown); see Device.configure."""
        return cleanup_of(self.full_every, self.clean_every)


def capabilities(display):
    """Ask the board (hello) for its canvas, bit depths, refresh modes per depth and the full_every setting."""
    h = request(display, HELLO)
    return parse_capabilities(h)


def parse_capabilities(h):
    dm = h.get("depth_modes")
    return Capabilities(
        w=h["w"], h=h["h"], depths=list(h["depths"]), partial_depths=list(h.get("partial_depths") or []),
        depth_modes=None if dm is None else {int(k): list(v) for k, v in dm.items()},
        colors=list(h.get("colors") or []), max_png=h["max_png"], full_every=h.get("full_every"),
        full_every_set=h.get("full_every_set"), modes=list(h["modes"]) if "modes" in h else None,
        clean_every=h.get("clean_every"), clean_every_set=h.get("clean_every_set"))


def bluetooth(hello):
    """hello's Bluetooth setting, "off" or "on"; None on a firmware without Bluetooth."""
    return hello.get("bt")


def bt(display, on=None, pair=None, forget=None):
    """The `bt` command, over the USB cable only (over Bluetooth the device refuses it, CableOnly): the keys given
    are applied as forget, on, pair. Answer: {"on", "pairing" (seconds the pairing window stays open), "name"}."""
    keys = {k: v for k, v in (("on", on), ("pair", pair), ("forget", forget)) if v is not None}
    return request(display, BT, keys)


STEP_KEYS = ("recv", "prep", "inflate", "convert", "diff", "fingerprint", "panel", "total")


def steps_text(reply):
    """The device's milliseconds of each step of a show ("steps recv 11, prep 2, ..."); empty from a firmware without them."""
    steps = reply.get("steps")
    return "steps " + ", ".join(f"{k} {steps[k]}" for k in STEP_KEYS if k in steps) if steps else ""


def clean_text(reply):
    """The tiles the device cleaned after a show ("clean 2 tiles 390 ms"); empty when none, or from a firmware without it.
    `clean` is a list of {x, y, w, h, mode, ms}."""
    tiles = reply.get("clean")
    if isinstance(tiles, dict):  # the first firmware answered one rectangle
        tiles = [tiles]
    if not tiles:
        return ""
    return f"clean {len(tiles)} tile{'s' if len(tiles) != 1 else ''} {sum(t.get('ms', 0) for t in tiles)} ms"


def describe(reply):
    """One line of an answer: which refresh ran, why, where, how long, and the gate clocks the device counted."""
    if reply.get("skipped"):
        return "skipped upload: same picture (unchanged)"
    out = f"{reply.get('mode', 'full')} ({reply.get('why', '-')})"
    if reply.get("mode") != "none" and "w" in reply:
        out += f" {reply['x']},{reply['y']} {reply['w']}x{reply['h']}"
    out += f" depth {reply.get('depth')} {reply.get('ms')} ms"
    ckv = reply.get("ckv")
    if ckv:
        out += f" ckv {ckv['per']}/frame for {ckv['rows']} rows" + ("" if ckv["ok"] else " WRONG GATE CLOCK")
    if clean_text(reply):
        out += f" {clean_text(reply)}"
    if reply.get("steps"):
        out += f" [{steps_text(reply)}]"
    if "prepare_ms" in reply:  # the host's own time, apart from the device's ms
        out += f"; host: prepare {reply['prepare_ms']} ms, transfer {reply.get('transfer_ms')} ms"
    return out


def gate_clock_wrong(reply):
    return bool(reply.get("ckv")) and not reply["ckv"]["ok"]


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


def png_fits(png, hello, depths=None):
    """True when the device takes this PNG as it is: the panel's size and a gray depth it shows (or one of depths)."""
    if len(png) < 29 or png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) > hello["max_png"]:
        return False
    w, h, bits, ctype, _, _, lace = struct.unpack(">IIBBBBB", png[16:29])
    return (w, h, lace, ctype) == (hello["w"], hello["h"], 0, 0) and bits in (depths or hello["depths"])


def panel_png(png, hello, depth=None):
    """Any PNG the example reads, as one the panel takes: itself when it fits, else fitted on white at the panel's
    deepest depth (or `depth`) or its four colors by usb_display.convert (nearest level, no dithering)."""
    if png_fits(png, hello, depth and [depth]):
        return png
    color = bool(hello.get("colors"))
    depth = 2 if color else depth or max(hello["depths"])
    info = {"w": hello["w"], "h": hello["h"], "depth": depth, "palette": "bwry" if color else "gray"}
    return write_png(info["w"], info["h"], depth, bytes(convert(png, info)[20:]), indexed=color)


def png_rows(png):
    """(w, h, bits, rows) of a gray PNG this module wrote (write_png, panel_png): its packed rows, no filter bytes."""
    w, h, bits, ctype = struct.unpack(">IIBB", png[16:26])
    at, data = 8, b""
    while at + 8 <= len(png):
        n, kind = struct.unpack(">I4s", png[at:at + 8])
        if kind == b"IDAT":
            data += png[at + 8:at + 8 + n]
        at += 12 + n
    raw, stride = zlib.decompress(data), (w * bits + 7) // 8
    if ctype != 0 or len(raw) != (stride + 1) * h or any(raw[y * (stride + 1)] for y in range(h)):
        raise DisplayError("not a gray PNG with unfiltered rows")
    return w, h, bits, b"".join(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h))


def changed_rect(old, new, w, h, bits):
    """The rectangle (x, y, w, h) that holds every byte that differs between two frames of packed rows, x and w in
    whole groups of 8 pixels as the device wants them; None when the frames are the same."""
    stride = (w * bits + 7) // 8
    ys = [y for y in range(h) if old[y * stride:(y + 1) * stride] != new[y * stride:(y + 1) * stride]]
    if not ys:
        return None
    x0, x1 = stride, 0
    for y in ys:
        a, b = old[y * stride:(y + 1) * stride], new[y * stride:(y + 1) * stride]
        i = next(k for k in range(stride) if a[k] != b[k])
        j = next(k for k in range(stride - 1, -1, -1) if a[k] != b[k])
        x0, x1 = min(x0, i), max(x1, j)
    px0, px1 = x0 // bits * 8, min((x1 // bits + 1) * 8, w)
    return px0, ys[0], px1 - px0, ys[-1] - ys[0] + 1


def crop_png(rows, w, bits, rect):
    """The PNG of a rectangle (changed_rect) of a frame of packed rows."""
    x, y, rw, rh = rect
    stride, a, n = (w * bits + 7) // 8, x * bits // 8, (rw * bits + 7) // 8
    return write_png(rw, rh, bits, b"".join(rows[(y + j) * stride + a:(y + j) * stride + a + n] for j in range(rh)))
