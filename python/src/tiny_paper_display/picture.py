# SPDX-License-Identifier: MIT
# The Picture page's preparation (site/src/picture/prepare.ts, resample.ts) in Python: the same pixels.
"""Any picture as the levels a panel shows: fit/fill/stretch, a border, rotate auto, dither, depth, threshold.

Mirrors site/src/picture so that Python and the web page give the same pixels; docs/spec/vectors/picture-prepare.json
holds the cases, generated from the JavaScript and read by the tests on both sides. The JavaScript keeps its planes in
Float32Array: array('f') here rounds at the same stores, everything between is double precision as there.
"""
import math
from array import array

from .usb_display import DisplayError

DEFAULTS = {"fit": "fit", "border": "white", "rotate": "auto", "depth": 1, "color": False, "dither": "floyd",
            "threshold": 128}
FITS, BORDERS, DITHERS = ("fit", "fill", "stretch"), ("white", "black"), ("floyd", "ordered", "none")
MAX_SOURCE_PIXELS = 48_000_000
PALETTE = ((0, 0, 0), (255, 0, 0), (255, 255, 0), (255, 255, 255))  # black, red, yellow, white = the panel's codes
BAYER4 = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))


def _round(v):  # Math.round
    return math.floor(v + 0.5)


def _clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def check_settings(s):
    """The settings with the defaults filled in; DisplayError for a value the page does not have either."""
    out = {**DEFAULTS, **{k: v for k, v in s.items() if v is not None}}
    unknown = set(out) - set(DEFAULTS)
    if unknown:
        raise DisplayError(f"unknown picture setting: {', '.join(sorted(unknown))}")
    for key, allowed in (("fit", FITS), ("border", BORDERS), ("dither", DITHERS)):
        if out[key] not in allowed:
            raise DisplayError(f"{key} is one of {', '.join(allowed)}")
    if out["rotate"] not in ("auto", 0, 90, 180, 270):
        raise DisplayError("rotate is auto, 0, 90, 180 or 270")
    if out["depth"] not in (1, 2, 4):
        raise DisplayError("depth is 1, 2 or 4")
    return out


class Source:
    """An RGBA picture: w, h and w*h*4 bytes (straight alpha)."""

    def __init__(self, w, h, rgba):
        if w < 1 or h < 1 or w * h > MAX_SOURCE_PIXELS or len(rgba) != w * h * 4:
            raise DisplayError("The picture is too large or empty")
        self.w, self.h, self.rgba = w, h, rgba


def rotate_source(s, deg):
    if deg == 0:
        return s
    w, h = (s.w, s.h) if deg == 180 else (s.h, s.w)
    out = bytearray(w * h * 4)
    for y in range(s.h):
        for x in range(s.w):
            nx = s.h - 1 - y if deg == 90 else s.w - 1 - x if deg == 180 else y
            ny = x if deg == 90 else s.h - 1 - y if deg == 180 else s.w - 1 - x
            a, b = (y * s.w + x) * 4, (ny * w + nx) * 4
            out[b:b + 4] = s.rgba[a:a + 4]
    return Source(w, h, bytes(out))


def pick_rotation(src, pw, ph, rotate):
    if rotate != "auto":
        return rotate
    s0, s90 = min(pw / src.w, ph / src.h), min(pw / src.h, ph / src.w)
    return 90 if s90 > s0 else 0


def _axis_taps(src_n, frm, to, dst_n):
    scale = (to - frm) / dst_n
    out = []
    for i in range(dst_n):
        if scale > 1:
            a = frm + i * scale
            b = a + scale
            k0, k1 = max(0, math.floor(a)), min(src_n - 1, math.ceil(b) - 1)
            w = [max(0, min(b, k + 1) - max(a, k)) for k in range(k0, k1 + 1)]
            total = 0
            for v in w:
                total += v
            total = total or 1
            out.append((k0, [v / total for v in w]))
        else:
            p = frm + (i + 0.5) * scale - 0.5
            k = math.floor(p)
            f = p - k
            a, b = min(src_n - 1, max(0, k)), min(src_n - 1, max(0, k + 1))
            out.append((a, [1]) if a == b else (a, [1 - f, f]))
    return out


def resample_plane(src, sw, sh, rect, dw, dh):
    """Box average when shrinking, bilinear when enlarging; src and the result are float32 arrays."""
    tx, ty = _axis_taps(sw, rect[0], rect[2], dw), _axis_taps(sh, rect[1], rect[3], dh)
    r0 = min(t[0] for t in ty)
    r1 = max(t[0] + len(t[1]) - 1 for t in ty)
    mid = array("f", bytes(4 * dw * (r1 - r0 + 1)))
    for y in range(r0, r1 + 1):
        row, o = y * sw, (y - r0) * dw
        for x in range(dw):
            k0, ws = tx[x]
            s = 0
            for j, wj in enumerate(ws):
                s += src[row + k0 + j] * wj
            mid[o + x] = s
    out = array("f", bytes(4 * dw * dh))
    for y in range(dh):
        k0, ws = ty[y]
        for x in range(dw):
            s = 0
            for j, wj in enumerate(ws):
                s += mid[(k0 + j - r0) * dw + x] * wj
            out[y * dw + x] = s
    return out


def compose(source, pw, ph, s):
    """The planes (1 gray or 3 colour) of the picture placed on the panel, and the rotation used."""
    rotation = pick_rotation(source, pw, ph, s["rotate"])
    src = rotate_source(source, rotation)
    edge = 255 if s["border"] == "white" else 0
    n = 3 if s["color"] else 1
    raw = [array("f", bytes(4 * src.w * src.h)) for _ in range(n)]
    px = src.rgba
    for i in range(src.w * src.h):
        a = px[i * 4 + 3] / 255
        ch = [px[i * 4 + c] * a + edge * (1 - a) for c in range(3)]
        if n == 1:
            raw[0][i] = 0.299 * ch[0] + 0.587 * ch[1] + 0.114 * ch[2]
        else:
            for c in range(3):
                raw[c][i] = ch[c]
    rect, box = (0, 0, src.w, src.h), (0, 0, pw, ph)
    if s["fit"] == "fit":
        k = min(pw / src.w, ph / src.h)
        w, h = min(pw, max(1, _round(src.w * k))), min(ph, max(1, _round(src.h * k)))
        box = ((pw - w) // 2, (ph - h) // 2, w, h)
    elif s["fit"] == "fill":
        k = max(pw / src.w, ph / src.h)
        cw, ch = pw / k, ph / k
        rect = ((src.w - cw) / 2, (src.h - ch) / 2, (src.w + cw) / 2, (src.h + ch) / 2)
    planes = []
    for p in raw:
        small = resample_plane(p, src.w, src.h, rect, box[2], box[3])
        if box[2] == pw and box[3] == ph:
            planes.append(small)
            continue
        full = array("f", [edge]) * (pw * ph)
        for y in range(box[3]):
            at = (box[1] + y) * pw + box[0]
            full[at:at + box[2]] = small[y * box[2]:(y + 1) * box[2]]
        planes.append(full)
    return planes, rotation


def gray_levels(plane, w, h, depth, dither, threshold):
    n1 = (1 << depth) - 1
    out = bytearray(w * h)
    if dither == "none":
        for i in range(w * h):
            out[i] = (1 if plane[i] >= threshold else 0) if depth == 1 else _clamp(_round(plane[i] * n1 / 255), 0, n1)
    elif dither == "ordered":
        for y in range(h):
            for x in range(w):
                t = (BAYER4[y % 4][x % 4] + 0.5) / 16
                out[y * w + x] = _clamp(math.floor(plane[y * w + x] * n1 / 255 + t), 0, n1)
    else:
        buf = array("f", plane)
        for y in range(h):
            for x in range(w):
                i = y * w + x
                old = buf[i]
                q = _clamp(_round(old * n1 / 255), 0, n1)
                out[i] = q
                err = old - q * 255 / n1
                if x + 1 < w:
                    buf[i + 1] += err * 7 / 16
                if y + 1 < h:
                    if x > 0:
                        buf[i + w - 1] += err * 3 / 16
                    buf[i + w] += err * 5 / 16
                    if x + 1 < w:
                        buf[i + w + 1] += err / 16
    return out


def nearest_color(r, g, b):
    best, bd = 0, math.inf
    for i, p in enumerate(PALETTE):
        d = (r - p[0]) ** 2 + (g - p[1]) ** 2 + (b - p[2]) ** 2
        if d < bd:
            bd, best = d, i
    return best


def color_levels(planes, w, h, dither):
    out = bytearray(w * h)
    if dither == "floyd":
        buf = [array("f", p) for p in planes]
        for y in range(h):
            for x in range(w):
                i = y * w + x
                q = nearest_color(buf[0][i], buf[1][i], buf[2][i])
                out[i] = q
                for c in range(3):
                    p = buf[c]
                    err = p[i] - PALETTE[q][c]
                    if x + 1 < w:
                        p[i + 1] += err * 7 / 16
                    if y + 1 < h:
                        if x > 0:
                            p[i + w - 1] += err * 3 / 16
                        p[i + w] += err * 5 / 16
                        if x + 1 < w:
                            p[i + w + 1] += err / 16
    else:
        for y in range(h):
            for x in range(w):
                i = y * w + x
                bias = ((BAYER4[y % 4][x % 4] + 0.5) / 16 - 0.5) * 128 if dither == "ordered" else 0
                out[i] = nearest_color(planes[0][i] + bias, planes[1][i] + bias, planes[2][i] + bias)
    return out


def prepare(source, pw, ph, **settings):
    """(levels, rotation): one level per panel pixel (bytes, rows top first). Gray: 0..2^depth-1, 0 black; colour
    (color=True, depth 2): 0 black, 1 red, 2 yellow, 3 white."""
    s = check_settings(settings)
    planes, rotation = compose(source, pw, ph, s)
    if s["color"]:
        return bytes(color_levels(planes, pw, ph, s["dither"])), rotation
    return bytes(gray_levels(planes[0], pw, ph, s["depth"], s["dither"], s["threshold"])), rotation


def pack_rows(levels, w, h, depth):
    """Levels as packed rows, left pixel in the high bits (what display_fw.write_png takes)."""
    stride, per = (w * depth + 7) // 8, 8 // depth
    out = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            out[y * stride + x // per] |= levels[y * w + x] << (8 - depth * (x % per + 1))
    return bytes(out)
