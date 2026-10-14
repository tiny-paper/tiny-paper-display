# SPDX-License-Identifier: MIT
# The simple API: find devices, open, configure, draw, close by itself. A thin layer over display_fw, usb_display, ble_port.
"""
    import tiny_paper_display as tpd
    for dev in tpd.devices():          # USB ports whose hello says: display firmware
        print(dev.port, dev.board, dev.w, dev.h)
    with tpd.open() as d:              # the first device; tpd.open("COM5"), tpd.open(bluetooth="NAME")
        d.configure(cleanup={"screen": 30})     # stored on the device
        d.show("photo.png", fit="fit", dither="floyd")
        d.area(canvas, 8, 16)          # a rectangle, x a multiple of 8
        d.clear()
    tpd.show("photo.png")              # open the first device, show, close

Device settings (cleanup) live on the device. Picture preferences (fit, border, dither, depth, threshold, rotate) are
per call, with the defaults of the connection (`d.defaults`, or keyword arguments of open()); they are never sent.
"""
import logging
import sys
import time
from dataclasses import dataclass

from . import display_fw, images, picture, usb_display
from .canvas import Canvas
from .display_fw import Busy, PanelFault, Refused
from .usb_display import Disconnected, Display, DisplayError

logger = logging.getLogger(__name__)
ROOT_LOG = "tiny_paper_display"
_handler = None


def set_log(level="info"):
    """Log to stderr: "debug" (each request and answer, no picture bytes), "info" (each show: host prepare ms, transfer
    ms, the device's mode, why and ms), "warning" (retries and refusals); None removes it. By default the package is
    silent (a NullHandler). Your own logging configuration works as well: the logger is "tiny_paper_display"."""
    global _handler
    root = logging.getLogger(ROOT_LOG)
    if _handler is not None:
        root.removeHandler(_handler)
        _handler = None
    if level is None:
        return
    number = level if isinstance(level, int) else logging.getLevelName(str(level).upper())
    if not isinstance(number, int):
        raise DisplayError("log is debug, info, warning or None")
    _handler = logging.StreamHandler(sys.stderr)
    _handler.setFormatter(logging.Formatter("%(asctime)s %(name)s %(levelname)s %(message)s"))
    root.addHandler(_handler)
    root.setLevel(number)

ESPRESSIF = 0x303A
PROBE_TIMEOUT = 1  # seconds a port gets to answer hello while looking
KEEP = object()
PICTURE_KEYS = ("fit", "border", "dither", "depth", "threshold", "rotate")
DEFAULTS = {"fit": "fit", "border": "white", "dither": "floyd", "depth": None, "threshold": 128, "rotate": "auto"}


@dataclass(frozen=True)
class Found:
    """A device that answered hello with the display firmware. port: the serial port, or None for Bluetooth, where
    address is the name or address to open it with."""
    port: str
    address: str
    board: str
    w: int
    h: int

    @property
    def target(self):
        return self.port or self.address


def list_ports():
    """[(name, vid)] of the serial ports; the one place the system is asked (tests replace it)."""
    from serial.tools import list_ports as lp
    return [(p.device, p.vid) for p in lp.comports()]


def _probe(port):
    """(Display, hello) when this open port runs the display firmware, else None."""
    display = Display(port, timeout=PROBE_TIMEOUT)
    try:
        hello = display.hello()
    except (DisplayError, OSError, ValueError, usb_display.codec.PortError):
        return None
    return (display, hello) if display_fw.is_display_firmware(hello) else None


def _try_serial(name):
    try:
        port = usb_display.open_port(name)
    except (DisplayError, OSError, ValueError, usb_display.codec.PortError):
        return None
    found = None
    try:
        found = _probe(port)
    finally:
        if found is None:
            port.close()
    return (port, *found) if found else None


def _candidates():
    return [name for name, vid in list_ports() if vid in (None, ESPRESSIF)]


def devices(bluetooth=False, scan_s=5):
    """The devices running the display firmware: every serial port whose hello is answered by it (a port that answers
    otherwise, or not at all, is not listed). bluetooth=True also scans for devices in range (needs bleak) and opens
    each one to ask hello."""
    out = []
    for name in _candidates():
        opened = _try_serial(name)
        if opened:
            port, _, hello = opened
            port.close()
            out.append(Found(name, "", hello.get("board", ""), hello["w"], hello["h"]))
    if bluetooth:
        from . import ble_port
        for ble_name, address in ble_port.find(scan_s):
            try:
                with Device._connect(None, ble_name or address, defaults={}) as d:
                    out.append(Found(None, ble_name or address, d.board, d.w, d.h))
            except (DisplayError, OSError):
                continue
    return out


def open(target=None, bluetooth=None, log=None, **defaults):  # noqa: A001 (the API's own name)
    """A Device. target: a port ("COM5", "/dev/ttyACM0"); bluetooth: a device name or address; neither: the first
    device found. Keyword arguments are the connection's picture defaults (fit, border, dither, depth, threshold,
    rotate). log: "debug", "info", "warning" shortcut for tpd.log(level). Use it in a with block: it is closed at the end,
    also after an exception."""
    if log is not None:
        set_log(log)
    if target and bluetooth:
        raise DisplayError("a port or bluetooth, not both")
    if target or bluetooth:
        return Device._connect(target, bluetooth, defaults)
    for name in _candidates():
        opened = _try_serial(name)
        if opened:
            return Device(*opened, name=name, defaults=defaults)
    raise DisplayError("no device running the display firmware found: is it plugged in with a data cable, and the port free?")


def open_all(bluetooth=False, log=None, **defaults):
    """A Group of every device found, each open; closed together."""
    if log is not None:
        set_log(log)
    opened = []
    try:
        for name in _candidates():
            found = _try_serial(name)
            if found:
                opened.append(Device(*found, name=name, defaults=defaults))
        if bluetooth:
            from . import ble_port
            for ble_name, address in ble_port.find():
                opened.append(Device._connect(None, ble_name or address, defaults))
        if not opened:
            raise DisplayError("no device running the display firmware found")
    except BaseException:
        Group(opened).close()
        raise
    return Group(opened)


def show(picture_, target=None, *, bluetooth=None, **options):
    """Open the first device (or target / bluetooth), show the picture (Device.show options), close; returns the answer."""
    with open(target, bluetooth) as d:
        return d.show(picture_, **options)


class Device:
    """An open display. d.caps (display_fw.Capabilities), d.hello (the raw answer), d.board, d.w, d.h, d.defaults."""

    def __init__(self, port, display, hello, name="", defaults=None, bluetooth=False):
        self.port, self.display, self.name, self.bluetooth = port, display, name, bluetooth
        self._stale = False  # the last call failed: the next one reconnects first and sends the whole picture
        display.timeout = 3
        self.defaults = {**DEFAULTS, **{k: v for k, v in (defaults or {}).items()}}
        bad = set(self.defaults) - set(DEFAULTS)
        if bad:
            port.close()
            raise DisplayError(f"unknown picture default: {', '.join(sorted(bad))}")
        self._set_hello(hello)

    @classmethod
    def _connect(cls, target, bluetooth, defaults):
        port = usb_display.open_port(target, bluetooth)
        try:
            found = _probe(port)
            if found is None:
                raise DisplayError(f"{target or bluetooth} does not run the display firmware (hello has no project: display)")
            return cls(port, *found, name=target or bluetooth, defaults=defaults, bluetooth=bool(bluetooth))
        except BaseException:
            port.close()
            raise

    def _set_hello(self, hello):
        self.hello, self.caps = hello, display_fw.parse_capabilities(hello)
        self.board, self.w, self.h = hello.get("board", ""), hello["w"], hello["h"]

    def close(self):
        port, self.port = self.port, None
        if port is not None:
            port.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _need_open(self):
        if self.port is None:
            raise DisplayError("the device is closed")

    def _reconnect(self):
        """A new link to the same device (a cut transfer, a replugged cable); the caches of the old one are gone."""
        old, self.port = self.port, None
        try:
            old.close()
        except Exception:  # the old port is gone anyway
            pass
        try:
            port = usb_display.open_port(None if self.bluetooth else self.name, self.name if self.bluetooth else None)
        except (DisplayError, OSError, ValueError, usb_display.codec.PortError) as exc:
            raise Disconnected(f"cannot reconnect to {self.name}: {exc}") from exc
        found = None
        try:
            found = _probe(port)
        finally:
            if found is None:
                port.close()
        if found is None:
            raise Disconnected(f"{self.name} does not answer as the display firmware after reconnecting")
        self.port, (self.display, hello) = port, found
        self.display.timeout = 3
        self._set_hello(hello)

    def _run(self, what, work):
        """work() with the device's failures made plain: a cut transfer (Disconnected) is retried once on a new link;
        after any failure the object stays usable and the next show sends the whole picture."""
        self._need_open()
        try:
            if self._stale:
                self._reconnect()
                self._stale = False
            try:
                return work()
            except Disconnected as exc:
                logger.warning("%s: transfer cut (%s): reconnecting and sending again, once", what, exc)
                self._reconnect()
                try:
                    return work()
                except Disconnected as again:
                    raise Disconnected(f"{what}: the transfer was cut twice: {again}") from again
        except DisplayError as exc:
            self._stale = isinstance(exc, Disconnected)
            self._next_force = True
            for key in ("_display_last", "_display_area_last", "_display_known"):
                setattr(self.display, key, None)  # forget what the device is thought to show
            if isinstance(exc, Refused):
                logger.warning("%s: refused by the device: %s", what, exc)
            raise

    def _send(self, what, png, started, send, force):
        """A prepared PNG on its way: run (retry, forget), check the panel's gate clock, log the host times (info)."""
        prepared = time.perf_counter()
        reply = self._run(what, lambda: send(png, force or self._next_force))
        sent = time.perf_counter()
        reply = {**reply, "prepare_ms": round((prepared - started) * 1000), "transfer_ms": round((sent - prepared) * 1000)}
        if display_fw.gate_clock_wrong(reply):
            self._next_force = True
            for key in ("_display_last", "_display_area_last", "_display_known"):
                setattr(self.display, key, None)
            logger.warning("%s: the device counted a wrong gate clock", what)
            raise PanelFault(reply)
        self._next_force = False
        steps = display_fw.steps_text(reply)
        clean = display_fw.clean_text(reply)
        logger.info("%s: host prepare %s ms, transfer %s ms; device %s (%s) %s ms%s%s", what, reply["prepare_ms"],
                    reply["transfer_ms"], reply.get("mode"), reply.get("why"), reply.get("ms"), f", {clean}" if clean else "",
                    f" ({steps})" if steps else "")
        return reply

    _next_force = False

    # ------------------------------------------------------------------ settings on the device
    def configure(self, cleanup=KEEP):
        """Settings stored on the device. cleanup: how ghosting is removed, {"areas": N, "screen": M}. areas: clean the
        changed parts every N partial updates (default 15, 0 never); screen: refresh the whole screen every M updates
        (default 0, never). A key left out keeps its value; None clears both (the defaults). Returns the new
        capabilities (caps.cleanup is the same dict)."""
        def work():
            if cleanup is not KEEP:
                display_fw.set_cleanup(self.display, cleanup)
            self._set_hello(display_fw.request(self.display, display_fw.HELLO))
            return self.caps
        return self._run("configure", work)

    # ------------------------------------------------------------------ drawing
    def _settings(self, given):
        unknown = set(given) - set(PICTURE_KEYS)
        if unknown:
            raise DisplayError(f"unknown picture option: {', '.join(sorted(unknown))}")
        return {**self.defaults, **{k: v for k, v in given.items() if v is not None}}

    def _depth(self, wanted, partial=False):
        depths = self.caps.partial_depths if partial else self.caps.depths
        if not depths:
            raise DisplayError(f"{self.board or 'this board'} has no partial refresh: show the whole picture")
        if wanted is None:
            return max(depths)
        if wanted not in depths:
            raise DisplayError(f"{self.board or 'this board'} does not do {wanted} bit{' partial' if partial else ''} "
                               f"(it does {', '.join(map(str, sorted(depths)))})")
        return wanted

    def _levels_png(self, source, w, h, s, partial):
        color = bool(self.caps.colors)
        depth = 2 if color else self._depth(s["depth"], partial)
        levels, _ = picture.prepare(source, w, h, fit=s["fit"], border=s["border"], dither=s["dither"], depth=depth,
                                    threshold=s["threshold"], rotate=s["rotate"], color=color)
        return display_fw.write_png(w, h, depth, picture.pack_rows(levels, w, h, depth), indexed=color)

    def show(self, pic, *, mode=None, passes=None, force=False, **options):
        """Show any picture (path, bytes, Canvas) fitted to the whole panel. options: fit ("fit", "fill", "stretch"),
        border ("white", "black"), dither ("floyd", "ordered", "none"), depth (1, 2, 4; None: the deepest the board
        has), threshold (1 bit, with dither none), rotate ("auto", 0, 90, 180, 270). A gray PNG the panel takes as it is
        goes unchanged when no depth is asked. Returns the device's answer (display_fw.describe)."""
        self._need_open()
        started = time.perf_counter()
        s = self._settings(options)
        png = None
        if isinstance(pic, Canvas) and (pic.w, pic.h) == (self.w, self.h) and not self.caps.colors:
            png = pic.png(self._depth(s["depth"]))
        elif isinstance(pic, (bytes, bytearray)) and s["depth"] is None and display_fw.png_fits(bytes(pic), self.hello):
            png = bytes(pic)
        if png is None:
            png = self._levels_png(images.load(pic), self.w, self.h, s, False)
        return self._send("show", png, started, lambda p, f: display_fw.show_png(
            self.display, p, self.hello, mode=mode, passes=passes, force=f), force)

    def area(self, pic, x, y, mode=None, *, passes=None, force=False, depth=None):
        """A rectangle of the panel with its top left pixel at x, y: a Canvas, or a picture (path, bytes) shown at its own
        size. x and the width must be multiples of 8 and all of it inside the panel (checked before anything is sent);
        the board must have a partial refresh at the depth (default: its deepest partial depth)."""
        self._need_open()
        started = time.perf_counter()
        if isinstance(pic, Canvas):
            w, h = pic.w, pic.h
        else:
            data = pic if isinstance(pic, (bytes, bytearray)) else None
            size = images.png_size(bytes(data)) if data else None
            if size is None:
                pic = images.load(pic)
                w, h = pic.w, pic.h
            else:
                w, h = size
        if type(x) is not int or type(y) is not int:
            raise DisplayError("x and y are whole numbers")
        if x % 8 or w % 8:
            raise DisplayError(f"x and the width are multiples of 8 (x={x}, width={w})")
        if x < 0 or y < 0 or x + w > self.w or y + h > self.h:
            raise DisplayError(f"the rectangle {x},{y} {w}x{h} is not inside the {self.w}x{self.h} panel")
        d = self._depth(depth, partial=True)
        if isinstance(pic, Canvas):
            png = pic.png(d)
        elif isinstance(pic, (bytes, bytearray)) and depth is None and display_fw.png_fits(bytes(pic), {**self.hello, "w": w, "h": h, "depths": self.caps.partial_depths}):
            png = bytes(pic)
        else:
            s = {**self._settings({}), "fit": "stretch", "rotate": 0, "depth": d}
            png = self._levels_png(images.load(pic), w, h, s, True)
        return self._send("area", png, started, lambda p, f: display_fw.show_area(
            self.display, p, self.hello, x, y, mode, passes, force=f), force)

    def clear(self):
        """A white panel (one full refresh)."""
        return self._run("clear", lambda: display_fw.clear(self.display))


class Group:
    """Several open devices; the same calls, each returning the list of the devices' results (an exception object
    in the place of a device that failed)."""

    def __init__(self, devices):
        self.devices = list(devices)

    def __len__(self):
        return len(self.devices)

    def __iter__(self):
        return iter(self.devices)

    def _each(self, name, *args, **kw):
        """One entry per device, in order: its result, or the exception it raised (the others go on)."""
        out = []
        for d in self.devices:
            try:
                out.append(getattr(d, name)(*args, **kw))
            except Exception as exc:  # one device failing must not stop the others
                logger.warning("%s on %s failed: %s", name, d.name, exc)
                out.append(exc)
        return out

    def show(self, pic, **kw):
        return self._each("show", pic, **kw)

    def area(self, pic, x, y, mode=None, **kw):
        return self._each("area", pic, x, y, mode, **kw)

    def clear(self):
        return self._each("clear")

    def configure(self, **kw):
        return self._each("configure", **kw)

    def close(self):
        error = None
        for d in self.devices:
            try:
                d.close()
            except Exception as exc:  # close the others anyway
                error = error or exc
        if error:
            raise error

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
