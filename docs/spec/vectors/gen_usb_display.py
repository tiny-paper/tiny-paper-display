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
HELLO, SHOW_BEGIN, SHOW_CHUNK, SHOW_END, CLEAR = 0x01, 0x08, 0x09, 0x0A, 0x0B


def head(major, n):
    if n < 24:
        return bytes([major << 5 | n])
    for ai, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
        if n < 1 << (8 * struct.calcsize(fmt)):
            return bytes([major << 5 | ai]) + struct.pack(fmt, n)
    raise ValueError(n)


def cbor(v):
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


class Session:
    def __init__(self, name, board, depths, colors):
        self.name, self.seq, self.steps = name, 0, []
        deep = 8 if colors else max(depths)
        self.panel = {"board": board, "fw": FW, "w": W, "h": H, "depths": depths, "colors": colors,
                      "max_png": bound(W, H, deep)}

    def pair(self, cmd, req, resp):
        self.seq = self.seq % 255 + 1
        return {"cmd": cmd, "seq": self.seq, "tx": frame(self.seq, cmd, cbor(req)).hex(),
                "rx": frame(self.seq, cmd | 0x80, cbor(resp)).hex(), "req": show(req), "resp": show(resp)}

    def step(self, name, cmd, req, resp, **extra):
        s = {"name": name, **self.pair(cmd, req, resp), **extra}
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
        return r

    def send(self, name, data, first=None, announce=None):
        """show_begin and the chunks of `data` (first: bytes in the first chunk)."""
        self.step(name + "_begin", SHOW_BEGIN, {"size": announce or len(data)}, {"chunk": 4096, "next": 0})
        off = 0
        for n in ([first, len(data) - first] if first else [len(data)]):
            self.step("%s_chunk_%d" % (name, off), SHOW_CHUNK, {"off": off, "data": data[off:off + n]}, {"next": off + n})
            off += n

    def refused_chunk(self, name, data, err, msg):
        self.step(name + "_begin", SHOW_BEGIN, {"size": len(data)}, {"chunk": 4096, "next": 0})
        self.step(name, SHOW_CHUNK, {"off": 0, "data": data}, {"err": err, "msg": msg})


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
    s = Session("gray", "vec_gray", [1, 2], None)
    mx = s.panel["max_png"]
    depth_msg = "this panel takes a gray PNG of 1, 2 bits a pixel, no color, alpha or 8 bit"
    s.step("hello", HELLO, {"proto": 1, "app": "vectors"}, s.hello_resp())
    g1 = PICS["gray1"]
    s.send("gray1", g1["png"], first=40)
    end = s.step("gray1_end", SHOW_END, {}, {"depth": 1, "ms": 0}, pixels=pixels(g1["rows"]), depth=1)
    # requests that arrive while the panel refreshes: hello is answered, anything else is BUSY
    end["during"] = [s.pair(HELLO, {}, s.hello_resp()),
                     s.pair(CLEAR, {}, {"err": 3, "msg": "refreshing", "detail": {"retry_ms": 2000}})]
    g2 = PICS["gray2"]
    s.send("gray2", g2["png"])
    s.step("gray2_end", SHOW_END, {}, {"depth": 2, "ms": 0}, pixels=pixels(g2["rows"]), depth=2)
    s.step("clear", CLEAR, {}, {"depth": 1, "ms": 0}, pixels=pixels([[1] * W] * H), depth=1)
    # refusals: none of them touches the panel
    s.step("unknown_cmd", 0x02, {}, {"err": 1, "msg": "this is the display firmware: hello, show, clear"})
    s.step("begin_no_size", SHOW_BEGIN, {}, {"err": 2, "msg": "show_begin needs size"})
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
    s.step("again_end", SHOW_END, {}, {"depth": 1, "ms": 0}, pixels=pixels(g1["rows"]), depth=1)
    return s


def bwry_session():
    s = Session("bwry", "vec_bwry", [1], ["black", "white", "red", "yellow"])
    msg = "this panel takes a 1-bit gray PNG or an indexed PNG of black, white, red and yellow"
    s.step("hello", HELLO, {"proto": 1, "app": "vectors"}, s.hello_resp())
    c = PICS["bwry"]
    s.send("bwry", c["png"])
    s.step("bwry_end", SHOW_END, {}, {"depth": 2, "ms": 0}, pixels=pixels(c["rows"]), depth=2)
    g1 = PICS["gray1"]
    s.send("gray1", g1["png"])
    s.step("gray1_end", SHOW_END, {}, {"depth": 2, "ms": 0}, pixels=pixels([[3 * v for v in r] for r in g1["rows"]]),
           depth=2)
    s.refused_chunk("gray2", PICS["gray2"]["png"], 34, msg)
    blue = png(W, H, 2, c["rows"], color_type=3, palette=BWRY[:3] + [(0, 0, 255)])
    s.send("blue", blue)
    s.step("blue_end", SHOW_END, {}, {"err": 34, "msg": "a color that is not black, white, red (ff0000) or yellow (ffff00)"})
    s.step("clear", CLEAR, {}, {"depth": 2, "ms": 0}, pixels=pixels([[3] * W] * H), depth=2)
    return s


def build():
    return {
        "about": "USB display protocol vectors (docs/spec/usb-display.md); generated by gen_usb_display.py, do not edit. "
                 "A session is replayed in order against a device with that panel: send tx, expect exactly rx. "
                 "pixels: what the panel shows after the step, one byte a pixel (gray level at `depth`, or the palette "
                 "code 0 black, 1 red, 2 yellow, 3 white), rows top first. A step with err leaves the panel as it was. "
                 "during: requests sent while that step's refresh runs, answered before it. ms is 0 here (a fixed clock).",
        "pictures": {k: {"png": p["png"].hex(), "bits": p["bits"], "pixels": pixels(p["rows"])} for k, p in PICS.items()},
        "sessions": [{"name": s.name, "panel": s.panel, "steps": s.steps} for s in (gray_session(), bwry_session())],
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
