#!/usr/bin/env python3
"""Vectors of the USB display protocol (docs/spec/usb-display.md): usb-display.json, read by
firmware/components/usbdisp/test/test_usbdisp.c and examples/usb/python/tests/test_display_fw.py.
Standard library only. Run: python3 docs/spec/vectors/gen_usb_display.py [--check]"""
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "usb-display.json")
HELLO, SHOW_BEGIN, SHOW_CHUNK, SHOW_END, CLEAR, SET, BT, HAVE, BT_AUTH = 0x01, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x10
BT_NAME = "tp-display-VEC0"
AUTH = {"err": 12, "msg": "authenticate first: bt_auth with the key made over the USB cable"}


def head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < 1 << (8 * struct.calcsize(fmt)):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise ValueError(n)


def cbor(v):
    if v is None:
        return b"\xf6"
    if isinstance(v, bool):
        return b"\xf5" if v else b"\xf4"
    if isinstance(v, int):
        return head(0, v)
    if isinstance(v, bytes):
        return head(2, len(v)) + v
    if isinstance(v, str):
        b = v.encode()
        return head(3, len(b)) + b
    if isinstance(v, list):
        return head(4, len(v)) + b"".join(cbor(x) for x in v)
    if isinstance(v, dict):
        return head(5, len(v)) + b"".join(cbor(k) + cbor(x) for k, x in v.items())
    raise TypeError(type(v))


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = (c << 1 ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


def frame(seq, cmd, payload):
    body = struct.pack("<HBB", len(payload), seq, cmd) + payload
    return b"\xa5\x5a" + body + struct.pack("<H", crc16(body))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png(w, h, bits, rows, color_type=0, palette=None, interlace=0, damage=False):
    """rows: h lists of w samples (gray levels or palette indices) of `bits` bits."""
    raw = bytearray()
    for r in rows:
        raw.append(0)
        acc = n = 0
        for v in r:
            acc = acc << bits | v
            n += bits
            if n == 8:
                raw.append(acc)
                acc = n = 0
        if n:
            raw.append(acc << (8 - n))
    z = bytearray(zlib.compress(bytes(raw), 9))
    if damage:
        z[len(z) // 2] ^= 0x55
        z = z[:-3]
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bits, color_type, 0, 0, interlace))
    if palette:
        out += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    return out + chunk(b"IDAT", bytes(z)) + chunk(b"IEND", b"")


def bound(w, h, depth):
    return ((w * depth + 7) // 8 + 1) * h + 4096


W, H = 16, 8
FW = "0.0.0+vec"
BWRY = [(0, 0, 0), (255, 0, 0), (255, 255, 0), (255, 255, 255)]  # palette index = panel code


def pattern(levels, seed):
    return [[(x * 7 + y * 3 + seed + (x * y) // 3) % levels for x in range(W)] for y in range(H)]


PICS = {
    "gray1": {"bits": 1, "rows": pattern(2, 1)},
    "gray2": {"bits": 2, "rows": pattern(4, 2)},
    "gray4": {"bits": 4, "rows": pattern(16, 3)},
    "bwry": {"bits": 2, "rows": pattern(4, 4), "color_type": 3, "palette": BWRY},
}
for p in PICS.values():
    p["png"] = png(W, H, p["bits"], p["rows"], p.get("color_type", 0), p.get("palette"))


STEP_KEYS = ("recv", "prep", "inflate", "convert", "diff", "fingerprint", "panel", "total")  # usb-display.md "The answer"


class Session:
    def __init__(self, name, board, depths, colors, partial_depths=(), partial_all=False, full_every=0):
        self.name, self.seq, self.steps = name, 0, []
        deep = 8 if colors else max(depths)
        self.panel = {"board": board, "fw": FW, "w": W, "h": H, "depths": depths, "colors": colors,
                      "max_png": bound(W, H, deep), "partial_depths": list(partial_depths),
                      "partial_all": partial_all, "full_every": full_every}
        self.full_every = full_every
        self.default_every = full_every
        self.full_every_set = False
        self.fingerprint = None

    def pair(self, cmd, req, resp):
        self.seq = self.seq % 255 + 1
        return {"cmd": cmd, "seq": self.seq, "tx": frame(self.seq, cmd, cbor(req)).hex(),
                "rx": frame(self.seq, cmd | 0x80, cbor(resp)).hex(), "req": show(req), "resp": show(resp)}

    def step(self, name, cmd, req, resp, **extra):
        s = {"name": name, **self.pair(cmd, req, resp), **extra}
        if "err" in resp and cmd in (SHOW_BEGIN, SHOW_CHUNK, SHOW_END) and resp["err"] != 3:
            self.fingerprint = None
        if "err" in resp:
            s["err"] = resp["err"]
        self.steps.append(s)
        return s

    def hello_resp(self):
        p = self.panel
        r = {"proto": 1, "project": "display", "fw": p["fw"], "board": p["board"], "w": W, "h": H, "depths": p["depths"]}
        if p["colors"]:
            r["colors"] = p["colors"]
        r.update({"max_payload": 6144, "max_png": p["max_png"], "chunk": 4096})
        modes = ["full"] + (["partial"] if p["partial_depths"] else []) + (["partial_all"] if p["partial_all"] else [])
        modes += (["inverse"] if 1 in p["partial_depths"] else []) + (["passes"] if p["partial_all"] else [])
        pd = p["partial_depths"]
        depth_modes = {}
        for d in p["depths"]:  # a mode at a depth: the firmware's usbdisp_mode_check, written out once more here
            ok = ["full"] + (["partial"] if d in pd else [])
            if 1 in pd and d == 1:
                ok += (["partial_all"] if p["partial_all"] else []) + ["inverse"] + (["passes"] if p["partial_all"] else [])
            depth_modes[str(d)] = ok
        r.update({"partial_depths": pd, "modes": modes, "depth_modes": depth_modes, "full_every": self.full_every,
                  "full_every_set": self.full_every_set, "fingerprint_hash": "fnv1a64"})
        if self.fingerprint:
            r["fingerprint"] = self.fingerprint
        return r

    def send(self, name, data, first=None, announce=None, **target):
        """show_begin and the chunks of `data` (first: bytes in the first chunk; target: x, y, mode, passes)."""
        self.step(name + "_begin", SHOW_BEGIN, {"size": announce or len(data), **target}, {"chunk": 4096, "next": 0})
        off = 0
        for n in ([first, len(data) - first] if first else [len(data)]):
            self.step("%s_chunk_%d" % (name, off), SHOW_CHUNK, {"off": off, "data": data[off:off + n]}, {"next": off + n})
            off += n

    def refused_chunk(self, name, data, err, msg, **target):
        self.step(name + "_begin", SHOW_BEGIN, {"size": len(data), **target}, {"chunk": 4096, "next": 0})
        self.step(name, SHOW_CHUNK, {"off": 0, "data": data}, {"err": err, "msg": msg})

    def shown(self, name, depth, mode, why, rows, rect=(0, 0, W, H), refreshes=1, passes=None, cmd=SHOW_END):
        """The answer of a show_end or clear: which refresh ran, why, the area driven; rows: the glass after it."""
        resp = {"depth": depth, "ms": 0, "mode": mode, "why": why, **dict(zip("xywh", rect))}
        if mode == "none":
            resp["same"] = True
        self.fingerprint = fingerprint(W, H, depth, rows, bool(self.panel["colors"]))
        if passes:
            resp["passes"] = passes
        if cmd == SHOW_END:  # a fixed clock: every step took 0 ms
            resp["steps"] = {k: 0 for k in STEP_KEYS}
        return self.step(name, cmd, {}, resp, pixels=pixels(rows), depth=depth, refreshes=refreshes)

    def show(self, name, data, depth, ran, why, rows, rect=(0, 0, W, H), refreshes=1, **target):
        self.send(name, data, **target)
        return self.shown(name + "_end", depth, ran, why, rows, rect, refreshes, target.get("passes"))


def show(v):
    """A readable copy of a payload for the JSON file (bytes as hex)."""
    if isinstance(v, bytes):
        return {"hex": v.hex()}
    if isinstance(v, dict):
        return {k: show(x) for k, x in v.items()}
    return v


def pixels(rows):
    return bytes(v for r in rows for v in r).hex()


def gray_session():
    s = Session("gray", "vec_gray", [1, 2], None, partial_depths=[1], partial_all=True, full_every=20)
    white = [[1] * W] * H
    mx = s.panel["max_png"]
    depth_msg = "this panel takes a gray PNG of 1, 2 bits a pixel, no color, alpha or 8 bit"
    s.step("hello", HELLO, {"proto": 1, "app": "vectors"}, s.hello_resp())
    g1 = PICS["gray1"]
    s.send("gray1", g1["png"], first=40)
    end = s.shown("gray1_end", 1, "full", "first", g1["rows"])
    s.step("have_same", HAVE, {"fingerprint": s.fingerprint}, {"same": True})
    s.step("have_different", HAVE, {"fingerprint": "0000000000000000"}, {"send": True})
    # requests that arrive while the panel refreshes: hello is answered, anything else is BUSY
    end["during"] = [s.pair(HELLO, {}, {k: v for k, v in s.hello_resp().items() if k != "fingerprint"}),
                     s.pair(CLEAR, {}, {"err": 3, "msg": "refreshing", "detail": {"retry_ms": 2000}})]
    g2 = PICS["gray2"]
    s.send("gray2", g2["png"])
    s.shown("gray2_end", 2, "full", "depth", g2["rows"])
    s.shown("clear", 1, "full", "asked", white, cmd=CLEAR)
    # refusals: none of them touches the panel
    s.step("unknown_cmd", 0x02, {}, {"err": 1, "msg": "this is the display firmware: hello, show, clear, set"})
    s.step("begin_no_size", SHOW_BEGIN, {}, {"err": 2, "msg": "show_begin needs size"})
    s.step("hello_refused", HELLO, {}, s.hello_resp())
    s.step("have_refused", HAVE, {"fingerprint": fingerprint(W, H, 1, white)}, {"send": True})
    s.step("too_large", SHOW_BEGIN, {"size": mx + 1},
           {"err": 13, "msg": "the PNG has %d bytes, at most %d are taken" % (mx + 1, mx)})
    s.step("too_short", SHOW_BEGIN, {"size": 10}, {"err": 32, "msg": "too short for a PNG"})
    s.step("chunk_no_begin", SHOW_CHUNK, {"off": 0, "data": b"x"},
           {"err": 11, "msg": "no transfer: send show_begin", "detail": {"next": 0}})
    s.step("end_no_begin", SHOW_END, {}, {"err": 11, "msg": "no transfer: send show_begin", "detail": {"next": 0}})
    s.refused_chunk("not_png", b"\xff\xd8\xff\xe0" + bytes(60), 32, "not a PNG")
    small = png(8, 8, 1, [[0] * 8] * 8)
    s.refused_chunk("wrong_size", small, 33, "the PNG is 8x8, the panel is 16x8: send exactly the panel's size")
    s.refused_chunk("gray8", png(W, H, 8, pattern(256, 5)), 34, depth_msg)
    s.refused_chunk("gray4", PICS["gray4"]["png"], 34, depth_msg)
    s.refused_chunk("rgb", png(W, H, 8, [[0] * (3 * W)] * H, color_type=2), 34, depth_msg)
    s.refused_chunk("interlaced", png(W, H, 1, g1["rows"], interlace=1), 34,
                    "an interlaced PNG, or one without a header, is not taken")
    s.step("short_head_begin", SHOW_BEGIN, {"size": len(g1["png"])}, {"chunk": 4096, "next": 0})
    s.step("short_head", SHOW_CHUNK, {"off": 0, "data": g1["png"][:20]},
           {"err": 2, "msg": "the first chunk must hold the PNG header (29 bytes)"})
    # an incomplete transfer: show_end and a wrong offset say where to go on, and it can still be finished
    s.step("part_begin", SHOW_BEGIN, {"size": len(g1["png"])}, {"chunk": 4096, "next": 0})
    s.step("part_chunk", SHOW_CHUNK, {"off": 0, "data": g1["png"][:40]}, {"next": 40})
    s.step("part_end", SHOW_END, {}, {"err": 11, "msg": "the PNG is not complete", "detail": {"next": 40}})
    s.step("part_wrong_off", SHOW_CHUNK, {"off": 0, "data": g1["png"][:40]},
           {"err": 11, "msg": "not the next offset", "detail": {"next": 40}})
    s.step("part_overrun", SHOW_CHUNK, {"off": 40, "data": g1["png"][40:] + b"more"},
           {"err": 13, "msg": "more bytes than show_begin announced"})
    bad = png(W, H, 1, g1["rows"], damage=True)
    s.send("damaged", bad)
    s.step("damaged_end", SHOW_END, {}, {"err": 35, "msg": "the PNG data is damaged or cut short"})
    cut = PICS["gray2"]["png"][:-26]
    s.send("cut", cut)
    s.step("cut_end", SHOW_END, {}, {"err": 35, "msg": "the PNG data is damaged or cut short"})
    # after all of that it still shows a picture
    s.send("again", g1["png"])
    s.shown("again_end", 1, "full", "refused", g1["rows"])
    area_session(s, [list(r) for r in g1["rows"]])
    return s


def gray1(rows):
    return png(len(rows[0]), len(rows), 1, rows)


def paste(glass, x, y, rows):
    out = [list(r) for r in glass]
    for j, r in enumerate(rows):
        out[y + j][x:x + len(r)] = r
    return out


def changed(a, b):
    """The rectangle the device drives for a partial from a to b at 1 bit: the changed bytes, in groups of 8 pixels."""
    ys = [y for y in range(H) if a[y] != b[y]]
    xs = [x // 8 for y in ys for x in range(W) if a[y][x] != b[y][x]]
    return (min(xs) * 8, ys[0], (max(xs) - min(xs) + 1) * 8, ys[-1] - ys[0] + 1)


def area_session(s, glass):
    """Area updates and refresh modes on the gray session's panel (partial at 1 bit, every mode), from `glass`."""
    g2 = PICS["gray2"]
    # the default: only what changed, no flash; the device finds the rectangle itself
    s.show("same", gray1(glass), 1, "none", "unchanged", glass, rect=(0, 0, 0, 0), refreshes=0)
    corner = paste(glass, 9, 2, [[1 - v for v in r[9:14]] for r in glass[2:4]])
    s.show("corner", gray1(corner), 1, "partial", "changed", corner, rect=(8, 2, 8, 2))
    # a rectangle: x, y and a PNG of its size
    box = [[(x + y) % 2 for x in range(8)] for y in range(4)]
    glass = paste(corner, 8, 4, box)
    s.show("box", gray1(box), 1, "partial", "changed", glass, rect=changed(corner, glass), x=8, y=4)
    neg = [[1 - v for v in r] for r in box]
    glass = paste(glass, 8, 4, neg)
    s.show("box_partial", gray1(neg), 1, "partial", "asked", glass, rect=(8, 4, 8, 4), x=8, y=4, mode="partial")
    glass = paste(glass, 0, 0, box)
    s.show("box_all", gray1(box), 1, "partial_all", "asked", glass, rect=(0, 0, 8, 4), x=0, y=0, mode="partial_all")
    glass = paste(glass, 0, 4, box)
    s.show("box_inverse", gray1(box), 1, "inverse", "asked", glass, rect=(0, 4, 8, 4), refreshes=2, x=0, y=4,
           mode="inverse")
    glass = paste(glass, 8, 0, neg)
    s.show("box_passes", gray1(neg), 1, "passes", "asked", glass, rect=(8, 0, 8, 4), refreshes=3, x=8, y=0,
           mode="passes", passes=3)
    glass = paste(glass, 8, 4, box)
    s.show("box_full", gray1(box), 1, "full", "asked", glass, x=8, y=4, mode="full")
    s.show("whole_all", gray1(corner), 1, "partial_all", "asked", corner, mode="partial_all")
    glass = corner
    # refusals of a target; each one makes the next update a full refresh
    one = {"size": len(gray1(box))}
    s.step("mode_unknown", SHOW_BEGIN, {**one, "mode": "fast"},
           {"err": 36, "msg": "mode is one of full, partial, partial_all, inverse, passes"})
    s.step("x_alone", SHOW_BEGIN, {**one, "x": 8}, {"err": 37, "msg": "x and y come together"})
    s.step("x_text", SHOW_BEGIN, {**one, "x": "8", "y": 0}, {"err": 2, "msg": "x and y are numbers"})
    s.step("x_unaligned", SHOW_BEGIN, {**one, "x": 4, "y": 0},
           {"err": 37, "msg": "x 4, y 0: x is a multiple of 8, both inside the panel (16x8)"})
    s.step("y_outside", SHOW_BEGIN, {**one, "x": 8, "y": 8},
           {"err": 37, "msg": "x 8, y 8: x is a multiple of 8, both inside the panel (16x8)"})
    passes_msg = "passes (1..4) comes with the mode passes, and only with it"
    s.step("passes_alone", SHOW_BEGIN, {**one, "mode": "partial", "passes": 2}, {"err": 2, "msg": passes_msg})
    s.step("passes_missing", SHOW_BEGIN, {**one, "mode": "passes"}, {"err": 2, "msg": passes_msg})
    s.step("passes_many", SHOW_BEGIN, {**one, "mode": "passes", "passes": 5}, {"err": 2, "msg": passes_msg})
    s.refused_chunk("too_low", gray1(box), 37,
                    "a PNG of 8x4 at 8,6: the width is a multiple of 8, all of it inside the panel (16x8)", x=8, y=6)
    s.refused_chunk("too_wide", gray1([[0] * 16] * 2), 37,
                    "a PNG of 16x2 at 8,0: the width is a multiple of 8, all of it inside the panel (16x8)", x=8, y=0)
    s.refused_chunk("width_12", gray1([[0] * 12] * 2), 37,
                    "a PNG of 12x2 at 0,0: the width is a multiple of 8, all of it inside the panel (16x8)", x=0, y=0)
    s.refused_chunk("gray2_partial", g2["png"], 36, "partial at 2 bit: this board refreshes partially at 1 bit only",
                    mode="partial")
    s.refused_chunk("gray2_area", png(8, 4, 2, [[3] * 8] * 4), 36,
                    "an area at 2 bit: this board refreshes partially at 1 bit only", x=0, y=0)
    s.refused_chunk("gray2_inverse", g2["png"], 36, "inverse needs a 1 bit picture", mode="inverse")
    # an area when the device does not know what the panel shows: the rest white, a full refresh
    glass = paste([[1] * W] * H, 8, 4, box)
    s.show("box_after_refusal", gray1(box), 1, "full", "refused", glass, x=8, y=4)
    # an explicit partial without a frame kept becomes a full one and says so
    s.step("refuse_again", SHOW_BEGIN, {**one, "mode": "fast"},
           {"err": 36, "msg": "mode is one of full, partial, partial_all, inverse, passes"})
    s.show("partial_no_prev", gray1(corner), 1, "full", "refused", corner, mode="partial")
    # a transfer replaced by another was cut
    s.step("cut_begin", SHOW_BEGIN, {"size": len(gray1(corner))}, {"chunk": 4096, "next": 0})
    s.show("after_cut", gray1(glass), 1, "full", "cut", glass)
    # an area at another depth than the picture shown
    s.show("to_gray2", g2["png"], 2, "full", "depth", g2["rows"])
    s.show("back_to_gray1", gray1(corner), 1, "full", "depth", corner)
    # the setting: a full refresh every N updates
    s.step("set_get", SET, {}, {"full_every": 20, "full_every_set": False})
    s.step("set_bad", SET, {"full_every": 251}, {"err": 2, "msg": "full_every is 0 (never) to 250"})
    s.step("set_7", SET, {"full_every": 7}, {"full_every": 7, "full_every_set": True})
    s.step("set_clear", SET, {"full_every": None}, {"full_every": 20, "full_every_set": False})
    s.step("set_2", SET, {"full_every": 2}, {"full_every": 2, "full_every_set": True})
    s.full_every, s.full_every_set = 2, True
    a, b = glass, corner
    s.show("tick_1", gray1(a), 1, "partial", "changed", a, rect=changed(a, b))
    s.show("tick_2", gray1(b), 1, "partial", "changed", b, rect=changed(a, b))
    s.show("tick_3", gray1(a), 1, "full", "cleanup", a)
    s.show("tick_4", gray1(b), 1, "partial", "changed", b, rect=changed(a, b))
    s.step("hello_after_set", HELLO, {}, s.hello_resp())


def bwry_session():
    s = Session("bwry", "vec_bwry", [1], ["black", "white", "red", "yellow"])
    msg = "this panel takes a 1-bit gray PNG or an indexed PNG of black, white, red and yellow"
    s.step("hello", HELLO, {"proto": 1, "app": "vectors"}, s.hello_resp())
    c = PICS["bwry"]
    s.send("bwry", c["png"])
    s.shown("bwry_end", 2, "full", "board", c["rows"])
    g1 = PICS["gray1"]
    s.send("gray1", g1["png"])
    s.shown("gray1_end", 2, "full", "board", [[3 * v for v in r] for r in g1["rows"]])
    s.refused_chunk("gray2", PICS["gray2"]["png"], 34, msg)
    blue = png(W, H, 2, c["rows"], color_type=3, palette=BWRY[:3] + [(0, 0, 255)])
    s.send("blue", blue)
    s.step("blue_end", SHOW_END, {}, {"err": 34, "msg": "a color that is not black, white, red (ff0000) or yellow (ffff00)"})
    s.shown("clear", 2, "full", "asked", [[3] * W] * H, cmd=CLEAR)
    # a board without a partial refresh refuses the modes and an area, with the reason
    no = "this board has no partial refresh: send the whole picture, without a mode or with full"
    for mode in ("partial", "partial_all", "inverse"):
        s.step("no_" + mode, SHOW_BEGIN, {"size": len(g1["png"]), "mode": mode}, {"err": 36, "msg": no})
    s.step("no_area", SHOW_BEGIN, {"size": len(g1["png"]), "x": 0, "y": 0}, {"err": 36, "msg": no})
    s.send("full", g1["png"], mode="full")
    s.shown("full_end", 2, "full", "asked", [[3 * v for v in r] for r in g1["rows"]])
    return s


def fingerprint(w, h, bits, rows, color=False):
    data = bytearray(struct.pack(">HHBB", w, h, bits, color))
    for row in rows:
        acc = n = 0
        for v in row:
            acc, n = (acc << bits) | v, n + bits
            if n == 8:
                data.append(acc)
                acc = n = 0
        if n:
            data.append(acc << (8 - n))
    value = 0xcbf29ce484222325
    for b in data:
        value = ((value ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return f"{value:016x}"


def fingerprint_vectors():
    out = []
    for color, bits in ((False, 1), (False, 2), (False, 4), (True, 2)):
        for changed in (False, True):
            rows = pattern(1 << bits, 1)
            if changed:
                rows[0][0] ^= 1
            out.append({"bits": bits, "color": color, "png": png(W, H, bits, rows, 3 if color else 0,
                        BWRY if color else None).hex(), "fingerprint": fingerprint(W, H, bits, rows, color)})
    return out
def bt_session():
    """Bluetooth ("Bluetooth" in the spec) before any key: link says where the request arrives; after every refusal over
    ble the device drops the link (the next ble step is a new connection). Sealed frames: bt-key.json."""
    s = Session("bt", "vec_gray", [1, 2], None, partial_depths=[1], partial_all=True, full_every=20)

    def st(name, link, cmd, req, resp):
        s.step(name, cmd, req, resp, link=link)

    def state(on):
        return {"on": on, "name": BT_NAME}

    st("hello_off", "usb", HELLO, {}, {**s.hello_resp(), "bt": "off"})
    st("state_off", "usb", BT, {}, state(False))
    st("auth_over_usb", "usb", BT_AUTH, {}, {"err": 2, "msg": "bt_auth is for Bluetooth only"})
    st("on", "usb", BT, {"on": True}, state(True))
    st("hello_on", "usb", HELLO, {}, {**s.hello_resp(), "bt": "on"})
    st("unauthenticated_hello", "ble", HELLO, {}, AUTH)
    st("unauthenticated_clear", "ble", CLEAR, {}, AUTH)
    st("auth_without_key", "ble", BT_AUTH, {"kid": bytes(8), "nonce": bytes(16)}, AUTH)
    st("forget", "usb", BT, {"forget": True}, state(True))
    st("off", "usb", BT, {"on": False}, state(False))
    return s


def build():
    bt = bt_session()
    return {
        "about": "USB display protocol vectors (docs/spec/usb-display.md); generated by gen_usb_display.py, do not edit. "
                 "A session is replayed in order against a device with that panel: send tx, expect exactly rx. "
                 "pixels: what the panel shows after the step, one byte a pixel (gray level at `depth`, or the palette "
                 "code 0 black, 1 red, 2 yellow, 3 white), rows top first; refreshes: the panel refreshes of the step (a "
                 "partial one changes only the pixels the device asks it to drive). A step with err leaves the panel as "
                 "it was. panel.partial_depths, partial_all, full_every: what the board can do and its setting. "
                 "during: requests sent while that step's refresh runs, answered before it. ms is 0 here (a fixed clock).",
        "fingerprints": fingerprint_vectors(),
        # Fault replies for host codecs: IO and NOMEM are also injected by the firmware's failure-path tests.
        "refusals": [Session("codec", "vec_gray", [1], None).pair(CLEAR, {}, {"err": code, "msg": msg})
                     for code, msg in ((10, "panel failed"), (14, "no memory"))],
        "pictures": {k: {"png": p["png"].hex(), "bits": p["bits"], "pixels": pixels(p["rows"])} for k, p in PICS.items()},
        "sessions": [{"name": s.name, "panel": s.panel, "steps": s.steps} for s in (gray_session(), bwry_session())],
        "bluetooth": {"about": "A device with Bluetooth and no key (panel as session gray, name as given). link: usb "
                               "or ble; each ble step is a new connection, dropped after its refusal. Replayed in order.",
                      "name": BT_NAME, "panel": bt.panel, "steps": bt.steps},
    }


def main():
    text = json.dumps(build(), indent=1) + "\n"
    if "--check" in sys.argv:
        if open(OUT).read() != text:
            sys.exit("usb-display.json is stale: run gen_usb_display.py")
        return
    open(OUT, "w").write(text)


if __name__ == "__main__":
    main()
