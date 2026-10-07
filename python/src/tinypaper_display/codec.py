# SPDX-License-Identifier: MIT
"""Link framing, CBOR and the serial port. Generated from tools/diag/tiny_diag.py of the tiny-paper main repository: do not edit."""
import os
import struct
import subprocess
import sys
import time


SYNC = b"\xa5\x5a"


MAX_PAYLOAD = 6144


ERR_NAMES = {1: "UNKNOWN_CMD", 2: "BAD_REQUEST", 3: "BUSY", 4: "NOT_FOUND", 5: "DENIED", 6: "NO_SPACE",
             7: "HASH_MISMATCH", 8: "INVALID_PACKAGE", 9: "UNSUPPORTED", 10: "IO", 11: "SEQUENCE",
             12: "AUTH", 13: "TOO_LARGE"}


def crc16_ccitt_false(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _cbor_head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < (1 << (8 * struct.calcsize(fmt))):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise ValueError("int too large")


def cbor_encode(v):
    """Canonical-enough CBOR: definite lengths, shortest ints, map keys in insertion order."""
    if v is None:
        return b"\xf6"
    if v is True:
        return b"\xf5"
    if v is False:
        return b"\xf4"
    if isinstance(v, int):
        return _cbor_head(0, v) if v >= 0 else _cbor_head(1, -1 - v)
    if isinstance(v, float):
        return b"\xfb" + struct.pack(">d", v)
    if isinstance(v, (bytes, bytearray)):
        return _cbor_head(2, len(v)) + bytes(v)
    if isinstance(v, str):
        b = v.encode("utf-8")
        return _cbor_head(3, len(b)) + b
    if isinstance(v, (list, tuple)):
        return _cbor_head(4, len(v)) + b"".join(cbor_encode(x) for x in v)
    if isinstance(v, dict):
        return _cbor_head(5, len(v)) + b"".join(cbor_encode(k) + cbor_encode(x) for k, x in v.items())
    raise TypeError("cannot encode %r" % type(v))


class CborError(ValueError):
    pass


def cbor_decode(data):
    v, off = _cbor_item(bytes(data), 0)
    if off != len(data):
        raise CborError("trailing bytes")
    return v


def _cbor_item(d, i):
    if i >= len(d):
        raise CborError("truncated")
    ib = d[i]
    major, ai = ib >> 5, ib & 31
    i += 1
    if major == 7:
        if ai == 20:
            return False, i
        if ai == 21:
            return True, i
        if ai in (22, 23):
            return None, i
        if ai == 25:
            return _half(struct.unpack_from(">H", d, i)[0]), i + 2
        if ai == 26:
            return struct.unpack_from(">f", d, i)[0], i + 4
        if ai == 27:
            return struct.unpack_from(">d", d, i)[0], i + 8
        raise CborError("simple %d" % ai)
    if ai < 24:
        n = ai
    elif ai in (24, 25, 26, 27):
        size = 1 << (ai - 24)
        if i + size > len(d):
            raise CborError("truncated")
        n = int.from_bytes(d[i:i + size], "big")
        i += size
    elif ai == 31 and major in (2, 3, 4, 5):
        n = None
    else:
        raise CborError("bad additional info %d" % ai)
    if major == 0:
        return n, i
    if major == 1:
        return -1 - n, i
    if major in (2, 3):
        if n is None:
            parts = []
            while d[i] != 0xFF:
                p, i = _cbor_item(d, i)
                parts.append(p)
            i += 1
            return (b"".join(parts) if major == 2 else "".join(parts)), i
        if i + n > len(d):
            raise CborError("truncated")
        raw = d[i:i + n]
        return (raw if major == 2 else raw.decode("utf-8", "replace")), i + n
    if major == 4:
        out = []
        while (len(out) < n) if n is not None else d[i] != 0xFF:
            x, i = _cbor_item(d, i)
            out.append(x)
        return out, i + (1 if n is None else 0)
    if major == 5:
        out = {}
        while (len(out) < n) if n is not None else d[i] != 0xFF:
            k, i = _cbor_item(d, i)
            x, i = _cbor_item(d, i)
            out[k if isinstance(k, (str, int)) else repr(k)] = x
        return out, i + (1 if n is None else 0)
    if major == 6:
        return _cbor_item(d, i)  # tag: keep the content
    raise CborError("major %d" % major)


def _half(h):
    s, e, f = h >> 15, (h >> 10) & 0x1F, h & 0x3FF
    v = f * 2.0 ** -24 if e == 0 else (float("inf") if f == 0 else float("nan")) if e == 31 else (1 + f / 1024.0) * 2.0 ** (e - 15)
    return -v if s else v


def build_frame(seq, cmd, payload):
    if not isinstance(payload, (bytes, bytearray)):
        payload = cbor_encode(payload)
    body = struct.pack("<HBB", len(payload), seq, cmd) + bytes(payload)
    return SYNC + body + struct.pack("<H", crc16_ccitt_false(body))


class LinkParser:
    """Feed bytes; get ('frame', seq, cmd, payload_bytes) and ('text', bytes) items."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            k = self.buf.find(SYNC)
            if k < 0:
                keep = 1 if self.buf.endswith(b"\xa5") else 0
                if len(self.buf) > keep:
                    out.append(("text", bytes(self.buf[:len(self.buf) - keep])))
                    del self.buf[:len(self.buf) - keep]
                return out
            if k > 0:
                out.append(("text", bytes(self.buf[:k])))
                del self.buf[:k]
            if len(self.buf) < 6:
                return out
            ln, seq, cmd = struct.unpack_from("<HBB", self.buf, 2)
            if ln > MAX_PAYLOAD:
                out.append(("text", bytes(self.buf[:1])))
                del self.buf[:1]
                continue
            if len(self.buf) < 8 + ln:
                return out
            crc = struct.unpack_from("<H", self.buf, 6 + ln)[0]
            if crc16_ccitt_false(self.buf[2:6 + ln]) != crc:
                out.append(("text", bytes(self.buf[:1])))
                del self.buf[:1]
                continue
            out.append(("frame", seq, cmd, bytes(self.buf[6:6 + ln])))
            del self.buf[:8 + ln]


UDEV_RULE = ('SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="1001", MODE="0660", '
             'TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"')


def linux_permission_help(dev):
    return "\n".join([
        "  No permission to open %s." % dev,
        "  Fix (pick one, then unplug and replug the device):",
        "   a) add yourself to the dialout group, then log out and back in:",
        "        sudo usermod -aG dialout $USER",
        "   b) or a udev rule for Espressif USB (also tells ModemManager to leave it alone):",
        "        echo '%s' | sudo tee /etc/udev/rules.d/60-tiny-paper.rules" % UDEV_RULE,
        "        sudo udevadm control --reload-rules && sudo udevadm trigger",
    ])


def modemmanager_active():
    if not sys.platform.startswith("linux"):
        return False
    try:
        return subprocess.call(["systemctl", "is-active", "--quiet", "ModemManager"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3) == 0
    except (OSError, subprocess.SubprocessError):
        return False


def modemmanager_help():
    return "\n".join([
        "  ModemManager is running. It probes new /dev/ttyACM* ports and can disturb esptool.",
        "  Stop it for this session:   sudo systemctl stop ModemManager",
        "  Or ignore Espressif USB for good (udev rule):",
        "        echo '%s' | sudo tee /etc/udev/rules.d/60-tiny-paper.rules" % UDEV_RULE,
        "        sudo udevadm control --reload-rules && sudo udevadm trigger",
    ])


class PortError(Exception):
    pass


def open_serial(dev, ui=None):
    import serial
    if "://" in dev:   # e.g. socket://localhost:5555 (QEMU tests)
        s = serial.serial_for_url(dev, timeout=0.05, do_not_open=True)
        s.open()
        return s
    if sys.platform.startswith("linux") and dev.startswith("/dev/") and os.path.exists(dev) \
            and not os.access(dev, os.R_OK | os.W_OK):
        raise PortError(linux_permission_help(dev))
    s = serial.Serial()
    s.port = dev
    s.baudrate = 115200
    s.timeout = 0.05
    s.write_timeout = 2
    # USB Serial/JTAG resets the chip while RTS is high and DTR low. The OS raises both on open; keep both
    # high (like serial_log.py and terminals) so the chip never sees RTS-only, then both drop on close.
    s.dtr = True
    s.rts = True
    if os.name == "posix":
        s.exclusive = True
    try:
        s.open()
    except serial.SerialException as e:
        msg = str(e)
        if "Permission denied" in msg or "Errno 13" in msg or "Access is denied" in msg:
            if sys.platform.startswith("linux"):
                raise PortError(linux_permission_help(dev))
            raise PortError("  No permission to open %s (Windows: close other programs using the port)." % dev)
        if "busy" in msg.lower() or "Errno 16" in msg or "Could not exclusively lock" in msg:
            extra = ("\n" + modemmanager_help()) if modemmanager_active() else ""
            raise PortError("  %s is busy: close serial_log.py, a terminal, the browser flasher or Arduino IDE.%s"
                            % (dev, extra))
        raise PortError("  cannot open %s: %s" % (dev, msg))
    return s


class LinkClient:
    def __init__(self, ser):
        self.ser = ser
        self.parser = LinkParser()
        self.seq = 0
        self.text = bytearray()
        self.events = []

    def request(self, cmd, payload, timeout=3.0, retries=1):
        self.seq = self.seq % 255 + 1
        frame = build_frame(self.seq, cmd, payload)
        for attempt in range(retries + 1):
            self.ser.write(frame)
            t_end = time.monotonic() + timeout
            while time.monotonic() < t_end:
                chunk = self.ser.read(4096)
                if not chunk:
                    continue
                for it in self.parser.feed(chunk):
                    if it[0] == "text":
                        self.text += it[1]
                        continue
                    _, seq, rcmd, pl = it
                    try:
                        val = cbor_decode(pl) if pl else {}
                    except CborError as e:
                        val = {"_cbor_error": str(e), "_hex": pl.hex()}
                    if seq == 0:
                        self.events.append({"cmd": rcmd, "payload": val})
                    elif seq == self.seq and rcmd == (cmd | 0x80):
                        if isinstance(val, dict) and "err" in val:
                            val["err_name"] = ERR_NAMES.get(val["err"], "?")
                        return val
        return {"_timeout": True, "attempts": retries + 1}
