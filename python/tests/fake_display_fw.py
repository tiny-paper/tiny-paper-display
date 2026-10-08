# SPDX-License-Identifier: MIT
"""A display firmware in Python for the tests: the area and mode rules of docs/spec/usb-display.md on a gray panel,
with the glass it would show. test_display_fw.py holds it to the vectors (every answer of the gray session), so the
bench and movie tests run against the firmware's own behaviour, not a guess."""
import struct
import zlib

from tiny_paper_display import display_fw as fw
from tiny_paper_display.display_fingerprint import fingerprint_rows
from tiny_paper_display.usb_display import codec

SIG = b"\x89PNG\r\n\x1a\n"


class Refuse(Exception):
    def __init__(self, err, msg="", **detail):
        self.reply = {"err": err, "msg": msg, **({"detail": detail} if detail else {})}


class FakeDevice:
    def __init__(self, w=16, h=8, depths=(1, 2), partial_depths=(1,), partial_all=True, full_every=20,
                 board="fake", fw_version="0.0.0+fake", ckv=None, hello=None, bt=None, name="tp-display-FAKE"):
        self.w, self.h, self.depths, self.partial_depths = w, h, list(depths), list(partial_depths)
        self.partial_all, self.full_every, self.board, self.fw = partial_all, full_every, board, fw_version
        self.max_png = ((w * max(depths) + 7) // 8 + 1) * h + 4096
        self.ckv, self.hello_override = ckv, hello  # ckv: the gate clock map of every answer (a TRMNL X)
        self.png = None                # the open transfer: [size, bytes, target]
        self.glass = None              # what the panel shows: packed rows at glass_depth (the whole panel)
        self.glass_depth = 0
        self.kept = False              # the device knows the glass (the frame kept)
        self.no_prev, self.since_full = "first", 0
        self.fingerprint = None
        self.requests, self.answers, self.refreshes = [], [], 0
        self.bt, self.name, self.pairing, self.bonded = bt, name, 0, False  # bt: "off", "on", None (no Bluetooth)
        self.auth_refused = 0

    # ---------------------------------------------------------------- the link
    def respond(self, wire, link="usb", secure=False):
        """The answer frame to one request frame. link "ble": over Bluetooth, `secure` says the link is bonded and
        encrypted; a link that is not gets AUTH for everything and nothing is looked at, let alone drawn."""
        seq, cmd = wire[4:6]
        if link == "ble" and not secure:
            self.auth_refused += 1
            reply = {"err": 12, "msg": "pair first: this Bluetooth link is not bonded"}
            return codec.build_frame(seq, cmd | 0x80, reply)
        self.link = link
        payload = codec.cbor_decode(wire[6:-2]) if len(wire) > 8 else {}
        self.requests.append((cmd, payload))
        try:
            reply = self.handle(cmd, payload)
        except Refuse as r:
            reply = r.reply
            if cmd in (fw.SHOW_BEGIN, fw.SHOW_CHUNK, fw.SHOW_END):
                self.forget("failed" if reply["err"] == 10 else "refused")
        self.answers.append((cmd, reply))
        return codec.build_frame(seq, cmd | 0x80, reply)

    def modes(self):
        one = 1 in self.partial_depths
        return (["full"] + ["partial"] * bool(self.partial_depths) + ["partial_all"] * (self.partial_all and one)
                + ["inverse"] * one + ["passes"] * (self.partial_all and one))

    def hello(self):
        base = self.hello_override or {
            "proto": 1, "project": "display", "fw": self.fw, "board": self.board, "w": self.w, "h": self.h,
            "depths": self.depths, "max_payload": 6144, "max_png": self.max_png, "chunk": 4096,
            "partial_depths": self.partial_depths, "modes": self.modes(), "full_every": self.full_every,
            "fingerprint_hash": "fnv1a64", **({"fingerprint": self.fingerprint} if self.fingerprint else {})}
        return {**base, "bt": self.bt} if self.bt and not self.hello_override else base

    def forget(self, why):
        self.kept, self.no_prev = False, why
        self.fingerprint = None

    def handle(self, cmd, p):
        if cmd == fw.HELLO:
            return self.hello()
        if cmd == fw.HAVE:
            value = p.get("fingerprint")
            if not isinstance(value, str) or len(value) != 16 or any(c not in "0123456789abcdef" for c in value):
                raise Refuse(2, "fingerprint is 16 lowercase hex digits")
            return {"same": True} if value == self.fingerprint and not self.png else {"send": True}
        if cmd == fw.SHOW_BEGIN:
            return self.begin(p)
        if cmd == fw.SHOW_CHUNK:
            return self.chunk(p)
        if cmd == fw.SHOW_END:
            return self.end()
        if cmd == fw.CLEAR:
            self.png = None
            self.glass, self.glass_depth = b"\xff" * ((self.w + 7) // 8 * self.h), 1
            self.kept, self.since_full = 1 in self.partial_depths, 0
            self.no_prev = "depth"
            self.fingerprint = fingerprint_rows(self.w, self.h, 1, self.glass)
            self.refreshes += 1
            return self.answer(1, "full", "asked", (0, 0, self.w, self.h))
        if cmd == fw.SET:
            if "full_every" in p:
                if type(p["full_every"]) is not int or not 0 <= p["full_every"] <= 250:
                    raise Refuse(2, "full_every is 0 (never) to 250")
                self.full_every = p["full_every"]
            return {"full_every": self.full_every}
        if cmd == fw.BT and self.bt:
            return self.set_bt(p)
        raise Refuse(1, "this is the display firmware: hello, show, clear, set")

    def set_bt(self, p):
        """bt: over the cable only; the keys apply as forget, on, pair."""
        if self.link == "ble":
            raise Refuse(38, "bt works only over the USB cable")
        if p.get("forget"):
            self.bonded, self.pairing = False, 0
        if "on" in p:
            self.bt = "on" if p["on"] else "off"
            self.pairing = self.pairing if p["on"] else 0
        if p.get("pair"):
            if self.bt != "on":
                raise Refuse(36, "Bluetooth is off: bt { on: true } first")
            self.pairing = 120
        elif "pair" in p:
            self.pairing = 0
        return {"on": self.bt == "on", "pairing": self.pairing, "name": self.name}

    # ---------------------------------------------------------------- show
    def check_mode(self, mode, area, depth):
        if not area and mode in (None, "full"):
            return
        if not self.partial_depths:
            raise Refuse(36, "this board has no partial refresh: send the whole picture, without a mode or with full")
        if mode in ("partial_all", "passes") and not self.partial_all:
            raise Refuse(36, f"this board cannot drive every pixel of an area: no {mode}")
        if mode in ("partial_all", "passes", "inverse") and (1 not in self.partial_depths or depth > 1):
            raise Refuse(36, f"{mode} needs a 1 bit picture")
        if depth and depth not in self.partial_depths:
            raise Refuse(36, f"{'partial' if mode == 'partial' else 'an area'} at {depth} bit: not on this board")

    def begin(self, p):
        if self.png:
            self.png = None
            self.forget("cut")
        size = p.get("size")
        if type(size) is not int:
            raise Refuse(2, "show_begin needs size")
        if ("x" in p) != ("y" in p):
            raise Refuse(37, "x and y come together")
        area = "x" in p
        if area and (type(p["x"]) is not int or type(p["y"]) is not int):
            raise Refuse(2, "x and y are numbers")
        if area and (p["x"] % 8 or p["x"] >= self.w or p["y"] >= self.h):
            raise Refuse(37, "x is a multiple of 8, both inside the panel")
        mode = p.get("mode")
        if mode is not None and mode not in fw.MODES:
            raise Refuse(36, "mode is one of full, partial, partial_all, inverse, passes")
        if ("passes" in p) != (mode == "passes") or (mode == "passes" and p["passes"] not in (1, 2, 3, 4)):
            raise Refuse(2, "passes (1..4) comes with the mode passes, and only with it")
        self.check_mode(mode, area, 0)
        if size > self.max_png:
            raise Refuse(13, "too large")
        if size < 57:
            raise Refuse(32, "too short for a PNG")
        self.png = [size, b"", {"xy": (p["x"], p["y"]) if area else None, "mode": mode, "passes": p.get("passes", 0)}]
        return {"chunk": 4096, "next": 0}

    def head(self, data, t):
        if data[:8] != SIG:
            raise Refuse(32, "not a PNG")
        w, h, bits, ctype, _, _, lace = struct.unpack(">IIBBBBB", data[16:29])
        if lace:
            raise Refuse(34, "interlaced")
        if not t["xy"] and (w, h) != (self.w, self.h):
            raise Refuse(33, "not the panel's size")
        if ctype != 0 or bits not in self.depths:
            raise Refuse(34, "depth")
        if t["xy"] and (not w or not h or w % 8 or t["xy"][0] + w > self.w or t["xy"][1] + h > self.h):
            raise Refuse(37, "the width is a multiple of 8, all of it inside the panel")
        self.check_mode(t["mode"], bool(t["xy"]), bits)
        return w, h, bits

    def chunk(self, p):
        if not self.png:
            raise Refuse(11, "no transfer: send show_begin", next=0)
        size, got, t = self.png
        data = p.get("data")
        if type(p.get("off")) is not int or not isinstance(data, (bytes, bytearray)) or not data:
            raise Refuse(2, "show_chunk needs off and data")
        if p["off"] != len(got):
            raise Refuse(11, "not the next offset", next=len(got))
        if len(data) > size - len(got):
            self.png = None
            raise Refuse(13, "more bytes than show_begin announced")
        if not got:
            try:
                if len(data) < 29:
                    raise Refuse(2, "the first chunk must hold the PNG header (29 bytes)")
                self.head(bytes(data), t)
            except Refuse:
                self.png = None
                raise
        self.png[1] = got + bytes(data)
        return {"next": len(self.png[1])}

    def end(self):
        if not self.png:
            raise Refuse(11, "no transfer: send show_begin", next=0)
        size, data, t = self.png
        if len(data) != size:
            raise Refuse(11, "the PNG is not complete", next=len(data))
        self.png = None
        w, h, bits = self.head(data, t)
        try:
            _, _, _, rows = fw.png_rows(data)
        except (fw.DisplayError, zlib.error, struct.error) as e:
            raise Refuse(35, "the PNG data is damaged or cut short") from e
        mode, area, whole = t["mode"], bool(t["xy"]), (0, 0, self.w, self.h)
        keep = bits in self.partial_depths
        have = keep and self.kept and self.glass_depth == bits
        if area and self.kept and not have:
            raise Refuse(34, f"the picture shown has {self.glass_depth} bit: an area must have the same depth")
        stride = (self.w * bits + 7) // 8
        rect = (t["xy"][0], t["xy"][1], w, h) if area else whole
        if area:
            new = bytearray(self.glass if have else b"\xff" * (stride * self.h))
            a, n = rect[0] * bits // 8, w * bits // 8
            for j in range(h):
                new[(rect[1] + j) * stride + a:(rect[1] + j) * stride + a + n] = rows[j * n:(j + 1) * n]
            new = bytes(new)
        else:
            new = rows
        value = fingerprint_rows(self.w, self.h, bits, new)
        if value == self.fingerprint and mode in (None, "partial"):
            return self.answer(bits, "none", "unchanged", (0, 0, 0, 0))
        run, why = mode, "asked"
        if mode == "full":
            pass
        elif not keep:
            run, why = "full", "depth" if self.partial_depths else "board"
        elif not have:
            run, why = "full", "depth" if self.kept else self.no_prev
        elif mode is None:
            cleanup = self.full_every and self.since_full >= self.full_every
            run, why = ("full", "cleanup") if cleanup else ("partial", "changed")
        drv = whole if run == "full" else rect
        if run == "partial":
            drv = fw.changed_rect(self.glass, new, self.w, self.h, bits)
            if drv is None:
                return self.answer(bits, "none", "unchanged", (0, 0, 0, 0))
        self.refreshes += {"inverse": 2, "passes": t["passes"]}.get(run, 1)
        self.since_full = 0 if run == "full" else self.since_full + 1
        self.fingerprint = value
        self.glass, self.glass_depth = new, bits
        self.kept = keep
        if not keep:
            self.no_prev = "depth"
        return self.answer(bits, run, why, drv, t["passes"] if run == "passes" else 0)

    def answer(self, depth, mode, why, rect, passes=0):
        out = {"depth": depth, "ms": 0, "mode": mode, "why": why, **dict(zip("xywh", rect))}
        if mode == "none":
            out["same"] = True
        if passes:
            out["passes"] = passes
        if self.ckv and mode != "none":
            out["ckv"] = self.ckv
        return out

    def pixels(self):
        """The glass, one value a pixel, rows top first."""
        d, stride = self.glass_depth, (self.w * self.glass_depth + 7) // 8
        return bytes(self.glass[y * stride + x * d // 8] >> (8 - d - x * d % 8) & ((1 << d) - 1)
                     for y in range(self.h) for x in range(self.w))
