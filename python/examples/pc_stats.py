#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Live PC stats on the display: CPU, memory, temperatures, disks, network, top processes, load, battery.

    python pc_stats.py                      (the first device found, every 5 s, until Ctrl-C)
    python pc_stats.py --interval 10 --depth 4 --verbose
    python pc_stats.py --once               (draw one page and exit)
    python pc_stats.py --png page.png --size 800x480 [--demo]     (no device; --demo: fixed fake numbers)

One timer. Each cycle the whole page is drawn in memory and sent with one Device.show: the device refreshes only the
rectangle that changed. Disks and processes are read every 3rd cycle, temperatures every 2nd. Needs psutil if it is
installed, else reads /proc and /sys (Linux only). No other dependency; the pictures are drawn with tpd.Canvas."""
import argparse
import os
import platform
import socket
import sys
import time
from collections import deque
from pathlib import Path

import tiny_paper_display as tpd

HISTORY_S = 300  # the graphs show 5 minutes
SLOW_DISKS_PROCS, SLOW_TEMPS = 3, 2


# ---------------------------------------------------------------- /proc and /sys parsers (pure: text in, numbers out)
def parse_cpu_stat(text):
    """/proc/stat -> (total, [per core]) as (busy, total) jiffies."""
    out = {}
    for line in text.splitlines():
        f = line.split()
        if f and f[0].startswith("cpu") and len(f) > 5:
            v = [int(x) for x in f[1:9]]
            out[f[0]] = (sum(v) - v[3] - v[4], sum(v))
    cores = [out[k] for k in sorted((k for k in out if k != "cpu"), key=lambda k: int(k[3:]))]
    return out.get("cpu", (0, 0)), cores


def parse_meminfo(text):
    """/proc/meminfo -> bytes: mem_used (total - available), mem_total, swap_used, swap_total."""
    kb = {}
    for line in text.splitlines():
        k, _, v = line.partition(":")
        if v.split():
            kb[k] = int(v.split()[0]) * 1024
    mem_total, swap_total = kb.get("MemTotal", 0), kb.get("SwapTotal", 0)
    return {"mem_used": mem_total - kb.get("MemAvailable", kb.get("MemFree", 0)), "mem_total": mem_total,
            "swap_used": swap_total - kb.get("SwapFree", 0), "swap_total": swap_total}


def parse_net_dev(text):
    """/proc/net/dev -> (rx_bytes, tx_bytes) summed over all interfaces except lo."""
    rx = tx = 0
    for line in text.splitlines()[2:]:
        name, _, rest = line.partition(":")
        f = rest.split()
        if len(f) >= 9 and name.strip() != "lo":
            rx, tx = rx + int(f[0]), tx + int(f[8])
    return rx, tx


def parse_diskstats(text):
    """/proc/diskstats -> {device: (bytes read, bytes written)}."""
    out = {}
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 10:
            out[f[2]] = (int(f[5]) * 512, int(f[9]) * 512)
    return out


def parse_mounts(text):
    """/proc/mounts -> [(device path, mount point)] of real block devices, each device once (no loop devices)."""
    seen, out = set(), []
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].startswith("/dev/") and not f[0].startswith("/dev/loop") and f[0] not in seen:
            seen.add(f[0])
            out.append((f[0], f[1].replace("\\040", " ")))
    return out


def parse_loadavg(text):
    return tuple(float(x) for x in text.split()[:3])


def parse_uptime(text):
    return float(text.split()[0])


def parse_pid_stat(text, ticks):
    """/proc/PID/stat -> (command, cpu seconds used so far); the command may hold spaces and brackets."""
    a, b = text.index("("), text.rindex(")")
    f = text[b + 2:].split()
    return text[a + 1:b], (int(f[11]) + int(f[12])) / ticks


def classify_temp(chip, label=""):
    """A hwmon chip (and label) -> "CPU", "GPU", "NVME" or None."""
    c = chip.lower()
    if c in ("coretemp", "k10temp", "zenpower", "cpu_thermal", "x86_pkg_temp", "cpu-thermal") or c.startswith("cpu"):
        return "CPU"
    if c in ("amdgpu", "nouveau", "radeon", "nvidia") or c.startswith("gpu"):
        return "GPU"
    return "NVME" if c == "nvme" else None


def read(path, default=""):
    try:
        return Path(path).read_text()
    except (OSError, UnicodeDecodeError):
        return default


class ProcBackend:
    """Raw counters from /proc and /sys (Linux). `root` is "/" or a copy of the files for a test."""

    def __init__(self, root="/"):
        self.root = Path(root)
        self.ticks = os.sysconf("SC_CLK_TCK") if hasattr(os, "sysconf") else 100

    def p(self, rel):
        return self.root / rel

    def cpu(self):
        return parse_cpu_stat(read(self.p("proc/stat")))

    def memory(self):
        return parse_meminfo(read(self.p("proc/meminfo")))

    def net(self):
        return parse_net_dev(read(self.p("proc/net/dev")))

    def load(self):
        t = read(self.p("proc/loadavg"))
        return parse_loadavg(t) if t else None

    def uptime(self):
        t = read(self.p("proc/uptime"))
        return parse_uptime(t) if t else 0.0

    def temps(self):
        out = {}
        for d in sorted(self.p("sys/class/hwmon").glob("hwmon*")):
            kind = classify_temp(read(d / "name").strip())
            files = sorted(d.glob("temp*_input"))
            if kind and kind not in out and files:
                try:
                    out[kind] = int(read(files[0]).strip()) / 1000
                except ValueError:
                    pass
        if "CPU" not in out:
            for d in sorted(self.p("sys/class/thermal").glob("thermal_zone*")):
                if classify_temp(read(d / "type").strip()) == "CPU" or read(d / "type").strip() == "acpitz":
                    try:
                        out["CPU"] = int(read(d / "temp").strip()) / 1000
                        break
                    except ValueError:
                        pass
        return out

    def disks(self):
        io = parse_diskstats(read(self.p("proc/diskstats")))
        out = []
        for dev, mount in parse_mounts(read(self.p("proc/mounts"))):
            try:
                st = os.statvfs(mount)
                name = os.path.basename(os.path.realpath(dev))
            except OSError:
                continue
            total = st.f_blocks * st.f_frsize
            if total:
                rd, wr = io.get(name, (0, 0))
                out.append({"mount": mount, "used": total - st.f_bavail * st.f_frsize, "total": total, "rd": rd, "wr": wr})
        return out

    def procs(self):
        out = {}
        for d in self.p("proc").glob("[0-9]*"):
            try:
                out[int(d.name)] = parse_pid_stat(read(d / "stat"), self.ticks)
            except (ValueError, IndexError):
                pass
        return out

    def battery(self):
        for d in sorted(self.p("sys/class/power_supply").glob("BAT*")):
            try:
                return int(read(d / "capacity").strip()), read(d / "status").strip().lower() == "charging"
            except ValueError:
                pass
        return None

    def os_name(self):
        for line in read(self.p("etc/os-release")).splitlines():
            if line.startswith("PRETTY_NAME="):
                return line.split("=", 1)[1].strip('"')
        return platform.system() + " " + platform.release()


class PsutilBackend:
    """The same raw counters through psutil (any OS it supports)."""

    def __init__(self):
        import psutil
        self.ps = psutil

    @staticmethod
    def pair(t):
        total = sum(t) - getattr(t, "guest", 0) - getattr(t, "guest_nice", 0)
        return total - t.idle - getattr(t, "iowait", 0), total

    def cpu(self):
        return self.pair(self.ps.cpu_times()), [self.pair(t) for t in self.ps.cpu_times(percpu=True)]

    def memory(self):
        m, s = self.ps.virtual_memory(), self.ps.swap_memory()
        return {"mem_used": m.total - m.available, "mem_total": m.total, "swap_used": s.used, "swap_total": s.total}

    def net(self):
        n = self.ps.net_io_counters()
        return n.bytes_recv, n.bytes_sent

    def load(self):
        try:
            return tuple(self.ps.getloadavg())
        except (AttributeError, OSError):
            return None

    def uptime(self):
        return time.time() - self.ps.boot_time()

    def temps(self):
        out = {}
        try:
            sensors = self.ps.sensors_temperatures()
        except AttributeError:
            return out
        for chip, entries in sensors.items():
            kind = classify_temp(chip)
            if kind and kind not in out and entries:
                out[kind] = entries[0].current
        return out

    def disks(self):
        try:
            io = self.ps.disk_io_counters(perdisk=True)
        except Exception:  # noqa: BLE001 (counters are missing on some systems)
            io = {}
        out = []
        for part in self.ps.disk_partitions():
            try:
                u = self.ps.disk_usage(part.mountpoint)
            except OSError:
                continue
            c = io.get(os.path.basename(part.device))
            out.append({"mount": part.mountpoint, "used": u.used, "total": u.total, "rd": c.read_bytes if c else 0,
                        "wr": c.write_bytes if c else 0})
        return out

    def procs(self):
        out = {}
        for p in self.ps.process_iter(["name", "cpu_times"]):
            t = p.info["cpu_times"]
            if t:
                out[p.pid] = (p.info["name"] or "?", t.user + t.system)
        return out

    def battery(self):
        b = self.ps.sensors_battery() if hasattr(self.ps, "sensors_battery") else None
        return (round(b.percent), bool(b.power_plugged)) if b else None

    def os_name(self):
        return platform.system() + " " + platform.release()


def make_backend():
    try:
        return PsutilBackend()
    except ImportError:
        if not sys.platform.startswith("linux"):
            sys.exit("pc_stats needs psutil on this system (pip install psutil); without it only Linux works.")
        return ProcBackend()


# ---------------------------------------------------------------- the numbers: counters to rates, with history
class Sampler:
    """Turns the backend's counters into a snapshot dict (see demo_snapshot) with rates and 5-minute histories."""

    def __init__(self, backend, interval):
        self.b, self.n = backend, max(2, int(HISTORY_S // max(interval, 1)))
        self.cycle = 0
        self.prev = None  # (time, cpu, net)
        self.hist = {k: deque(maxlen=self.n) for k in ("cpu", "down", "up")}
        self.temp_hist = {}
        self.temps, self.disks, self.procs = {}, [], []
        self.prev_io, self.prev_procs = {}, ({}, 0.0)

    def sample(self):
        b, now = self.b, time.monotonic()
        (tb, tt), cores = b.cpu()
        rx, tx = b.net()
        down = up = 0.0
        cpu, per = 0.0, [0.0] * len(cores)
        if self.prev:
            t0, ((pb, pt), pcores), (prx, ptx) = self.prev
            dt = max(now - t0, 1e-3)
            cpu = 100 * (tb - pb) / (tt - pt) if tt > pt else 0.0
            per = [100 * (c[0] - p[0]) / (c[1] - p[1]) if c[1] > p[1] else 0.0 for c, p in zip(cores, pcores)]
            down, up = max(rx - prx, 0) / dt, max(tx - ptx, 0) / dt
        self.prev = (now, ((tb, tt), cores), (rx, tx))
        for k, v in (("cpu", cpu), ("down", down), ("up", up)):
            self.hist[k].append(v)
        if self.cycle % SLOW_TEMPS == 0:
            self.temps = b.temps()
        for k, v in self.temps.items():
            self.temp_hist.setdefault(k, deque(maxlen=self.n)).append(v)
        if self.cycle % SLOW_DISKS_PROCS == 0:
            self.read_slow(now)
        self.cycle += 1
        mem = b.memory()
        bat = b.battery()
        return {"host": socket.gethostname(), "os": b.os_name(), "uptime": b.uptime(), "clock": time.strftime("%H:%M:%S"),
                "cpu": cpu, "cores": per, "cpu_hist": list(self.hist["cpu"]), **mem,
                "temps": [{"name": k, "now": v, "hist": list(self.temp_hist[k])} for k, v in self.temps.items()],
                "disks": self.disks, "down": down, "up": up, "down_hist": list(self.hist["down"]),
                "up_hist": list(self.hist["up"]), "procs": self.procs, "load": b.load(), "battery": bat}

    def read_slow(self, now):
        disks = self.b.disks()
        t0 = self.prev_io.get("t")
        for d in disks:
            old = self.prev_io.get(d["mount"])
            dt = max(now - t0, 1e-3) if t0 else None
            d["rd_s"], d["wr_s"] = ((max(d["rd"] - old[0], 0) / dt, max(d["wr"] - old[1], 0) / dt) if old and dt else (0.0, 0.0))
        self.prev_io = {"t": now, **{d["mount"]: (d["rd"], d["wr"]) for d in disks}}
        self.disks = sorted(disks, key=lambda d: -d["total"])
        procs, pt = self.prev_procs
        cur = self.b.procs()
        if pt:
            dt = max(now - pt, 1e-3)
            rows = [(name, 100 * (sec - procs[pid][1]) / dt) for pid, (name, sec) in cur.items() if pid in procs]
            self.procs = sorted(rows, key=lambda r: -r[1])[:5]
        self.prev_procs = (cur, now)


def demo_snapshot():
    """Fixed fake numbers (docs and tests): 8 cores, a GPU, two disks, a battery."""
    wave = [(i * 37) % 70 + 10 for i in range(60)]
    return {"host": "workstation", "os": "Debian GNU/Linux 12 (bookworm)", "uptime": 3 * 86400 + 4 * 3600 + 25 * 60,
            "clock": "14:32:05", "cpu": 37.0, "cores": [12.0, 80.0, 45.0, 3.0, 100.0, 22.0, 9.0, 61.0], "cpu_hist": wave,
            "mem_used": 12.4 * 2**30, "mem_total": 31.3 * 2**30, "swap_used": 0.4 * 2**30, "swap_total": 4.0 * 2**30,
            "temps": [{"name": "CPU", "now": 58.0, "hist": [45 + i % 17 for i in range(60)]},
                      {"name": "GPU", "now": 47.0, "hist": [40 + i % 9 for i in range(60)]}],
            "disks": [{"mount": "/", "used": 210 * 2**30, "total": 468 * 2**30, "rd_s": 1.2e6, "wr_s": 0.3e6},
                      {"mount": "/home/data", "used": 1.2 * 2**40, "total": 3.6 * 2**40, "rd_s": 0.0, "wr_s": 5.5e6}],
            "down": 2.4e6, "up": 0.3e6, "down_hist": [w * 4e4 for w in wave], "up_hist": [w * 5e3 for w in reversed(wave)],
            "procs": [("firefox", 42.5), ("python3", 18.0), ("Xorg", 6.2), ("code", 4.0), ("pipewire", 1.1)],
            "load": (1.42, 1.1, 0.87), "battery": (76, False)}


# ---------------------------------------------------------------- drawing
def fmt_bytes(n):
    for unit, div in (("T", 2**40), ("G", 2**30), ("M", 2**20), ("K", 2**10)):
        if n >= div:
            return f"{n / div:.0f}{unit}" if n / div >= 100 else f"{n / div:.1f}{unit}"
    return f"{int(n)}B"


def fmt_rate(n):
    return fmt_bytes(n) + "/S"


def fmt_uptime(sec):
    d, rest = divmod(int(sec), 86400)
    h, m = rest // 3600, rest % 3600 // 60
    return f"{d}D {h}H" if d else f"{h}H {m}M"


class Page:
    """A Canvas plus a list of everything drawn: items (kind, (x, y, w, h), parent index or None), for the layout test."""

    def __init__(self, w, h, depth):
        self.c = tpd.Canvas(w, h)
        self.w, self.h, self.items = w, h, []
        self.s = max(1, min(round(w / 360), round(h / 220)))  # text scale: glyphs 7 * s px tall
        self.ink, self.paper = 0, 15
        self.mid = 8 if depth > 1 else 0  # the second series and bar fills: gray when there are grays
        self.t = max(1, self.s // 2)  # frame thickness
        self.gt = self.t + (self.s >= 4)  # graph line thickness

    def add(self, kind, rect, parent=None):
        self.items.append((kind, rect, parent))
        return len(self.items) - 1

    def box(self, x, y, w, h, title):
        self.c.frame(x, y, w, h, self.ink, self.t)
        i = self.add("box", (x, y, w, h))
        self.text(i, x + 2 * self.s, y + 2 * self.s, title)
        return i

    def text(self, parent, x, y, s, scale=None, color=None, limit=None):
        """Text that is cut where the box (or `limit` px from x) ends; returns the width drawn."""
        scale = scale or self.s
        bx, _, bw, _ = self.items[parent][1]
        room = (limit if limit is not None else bx + bw - 2 * self.s - x)
        s = "".join(ch if ch.upper() in tpd.canvas.FONT else " " for ch in s)[:max(0, (room + scale) // (6 * scale))]
        if not s:
            return 0
        self.c.text(x, y, s, scale, self.ink if color is None else color)
        w = 6 * scale * len(s) - scale
        self.add("text", (x, y, w, 7 * scale), parent)
        return w

    def bar(self, parent, x, y, w, h, pct, fill=None):
        self.c.frame(x, y, w, h, self.ink, self.t)
        inner = max(0, (w - 2 * self.t) * max(0.0, min(pct, 100.0)) / 100)
        self.c.rect(x + self.t, y + self.t, round(inner), h - 2 * self.t, self.ink if fill is None else fill)
        self.add("bar", (x, y, w, h), parent)

    def cores(self, parent, x, y, w, h, values):
        """One thin vertical bar per core, side by side."""
        self.c.frame(x, y, w, h, self.ink, self.t)
        n = max(1, len(values))
        inner_w, inner_h = w - 2 * self.t, h - 2 * self.t
        for i, v in enumerate(values):
            x0, x1 = i * inner_w // n, (i + 1) * inner_w // n
            bh = round(inner_h * max(0.0, min(v, 100.0)) / 100)
            self.c.rect(x + self.t + x0, y + self.t + inner_h - bh, max(1, x1 - x0 - (1 if x1 - x0 > 2 else 0)), bh, self.ink)
        self.add("cores", (x, y, w, h), parent)

    def graph(self, parent, x, y, w, h, series, vmin, vmax, slots):
        """Lines over time, newest at the right edge; series is [(values, thickness, color)]; slots = samples shown."""
        self.c.frame(x, y, w, h, self.ink, self.t)
        iw, ih = w - 2 * self.t - 1, h - 2 * self.t - 1
        for values, thick, color in series:
            vals = values[-slots:]
            pts = [(x + self.t + iw - round((len(vals) - 1 - i) * iw / max(slots - 1, 1)),
                    y + self.t + ih - round(ih * (min(max(v, vmin), vmax) - vmin) / max(vmax - vmin, 1e-9)))
                   for i, v in enumerate(vals)]
            self.c.polyline(pts, self.ink if color is None else color, thick)
        self.add("graph", (x, y, w, h), parent)


def render(snap, w, h, depth=1, slots=60):
    """The whole page for a snapshot; returns the Page (page.c is the Canvas)."""
    pg = Page(w, h, depth)
    c, s, lh = pg.c, pg.s, 9 * pg.s
    m = 2 * s
    hh = 11 * s
    c.rect(0, 0, w, hh, pg.ink)
    head = pg.add("box", (0, 0, w, hh))
    left = f"{snap['host']}  {snap['os']}"
    right = f"UP {fmt_uptime(snap['uptime'])}  {snap['clock']}"
    rw = 6 * s * len(right) - s
    pg.text(head, w - m - rw, 2 * s, right, color=pg.paper, limit=rw)
    pg.text(head, m, 2 * s, left, color=pg.paper, limit=max(0, w - 3 * m - rw - m))
    y0, y1 = hh + m, h - m
    cw = (w - 3 * m) // 2

    def column(x, names_fracs):
        gaps = m * (len(names_fracs) - 1)
        tot = sum(f for _, f in names_fracs)
        y, boxes = y0, {}
        for k, (name, f) in enumerate(names_fracs):
            bh = (y1 - y0 - gaps) * f / tot if k < len(names_fracs) - 1 else y1 - y
            boxes[name] = (x, int(y), cw, int(bh) if k < len(names_fracs) - 1 else int(y1 - y))
            y += int(bh) + m
        return boxes

    lcol = [("cpu", 42), ("mem", 20)] + ([("temp", 20)] if snap["temps"] else []) + [("load", 18)]
    L, R = column(m, lcol), column(2 * m + cw, [("net", 30), ("disk", 32), ("proc", 38)])
    inner = lambda b: (b[0] + 2 * s, b[1] + 2 * s + lh, b[2] - 4 * s, b[3] - 4 * s - lh)  # noqa: E731 (x, y, w, h below the title)

    # CPU: big percent, one bar per core, 5-minute history
    b = L["cpu"]
    i = pg.box(*b, f"CPU  {len(snap['cores'])} CORES  5 MIN")
    x, y, iw, ih = inner(b)
    uh = ih * 45 // 100
    bs = 3 * s if 21 * s <= uh and 4 * 18 * s <= iw * 4 // 10 else 2 * s
    bw = pg.text(i, x, y + (uh - 7 * bs) // 2, f"{snap['cpu']:.0f}%", bs)
    if uh > 2 * s and iw - bw - m > 4 * s:
        pg.cores(i, x + bw + 2 * m, y, iw - bw - 2 * m, uh, snap["cores"])
    if ih - uh - m > 4 * s:
        pg.graph(i, x, y + uh + m, iw, ih - uh - m, [(snap["cpu_hist"], pg.gt, None)], 0, 100, slots)

    # memory
    b = L["mem"]
    i = pg.box(*b, "MEMORY")
    x, y, iw, ih = inner(b)
    pct = 100 * snap["mem_used"] / max(snap["mem_total"], 1)
    pg.text(i, x, y, f"{fmt_bytes(snap['mem_used'])}/{fmt_bytes(snap['mem_total'])}  {pct:.0f}%")
    if ih >= lh + 6 * s:
        pg.bar(i, x, y + lh, iw, 5 * s, pct)
    if ih >= 2 * lh + 6 * s:
        sp = 100 * snap["swap_used"] / snap["swap_total"] if snap["swap_total"] else 0
        pg.text(i, x, y + lh + 6 * s + s, f"SWAP {fmt_bytes(snap['swap_used'])}/{fmt_bytes(snap['swap_total'])}", limit=iw * 6 // 10 - m)
        pg.bar(i, x + iw * 6 // 10 + m, y + lh + 6 * s + s + s, iw - iw * 6 // 10 - m, 5 * s, sp)

    # temperatures: numbers left, small history right
    if snap["temps"]:
        b = L["temp"]
        i = pg.box(*b, "TEMPERATURE")
        x, y, iw, ih = inner(b)
        for k, t in enumerate(snap["temps"]):
            if (k + 1) * lh <= ih:
                pg.text(i, x, y + k * lh, f"{t['name']} {t['now']:.0f}C", limit=iw // 2 - m)
        if iw // 2 > 6 * s and ih > 4 * s:
            pg.graph(i, x + iw // 2 + m, y, iw - iw // 2 - m, ih,
                     [(t["hist"], pg.gt, pg.ink if k == 0 else pg.mid) for k, t in enumerate(snap["temps"])], 20, 100, slots)

    # load and battery
    b = L["load"]
    i = pg.box(*b, "LOAD")
    x, y, iw, ih = inner(b)
    if snap["load"] and ih >= lh:
        pg.text(i, x, y, "1/5/15 MIN  " + "  ".join(f"{v:.2f}" for v in snap["load"]))
    if snap["battery"] and ih >= 2 * lh:
        pct, charging = snap["battery"]
        pg.text(i, x, y + lh, f"BATTERY {pct}%{' CHARGING' if charging else ''}", limit=iw // 2 - m)
        pg.bar(i, x + iw // 2 + m, y + lh + s, iw - iw // 2 - m, 5 * s, pct)

    # network: speeds, then two histories
    b = R["net"]
    i = pg.box(*b, "NETWORK  5 MIN")
    x, y, iw, ih = inner(b)
    pg.text(i, x, y, f"DOWN {fmt_rate(snap['down'])}", limit=iw // 2 - m)
    pg.text(i, x + iw // 2 + m, y, f"UP {fmt_rate(snap['up'])}", limit=iw - iw // 2 - m)
    gh = ih - lh - m
    if gh > 4 * s:
        top = max(max(snap["down_hist"] + snap["up_hist"], default=0), 1e5)
        pg.graph(i, x, y + lh + m, iw // 2 - m, gh, [(snap["down_hist"], pg.gt, None)], 0, top, slots)
        pg.graph(i, x + iw // 2 + m, y + lh + m, iw - iw // 2 - m, gh, [(snap["up_hist"], pg.gt, None)], 0, top, slots)

    # disks: name and size, then a bar and the speeds; as many as fit
    b = R["disk"]
    i = pg.box(*b, "DISKS")
    x, y, iw, ih = inner(b)
    per = lh + 6 * s + m
    fit = max(0, (ih + m) // per)
    shown = snap["disks"][:fit] if len(snap["disks"]) <= fit else snap["disks"][:max(fit - 1, 0)]
    for k, d in enumerate(shown):
        yy = y + k * per
        pct = 100 * d["used"] / max(d["total"], 1)
        pg.text(i, x, yy, f"{d['mount']}  {fmt_bytes(d['used'])}/{fmt_bytes(d['total'])}")
        pg.bar(i, x, yy + lh, iw * 4 // 10 - m, 5 * s, pct)
        pg.text(i, x + iw * 4 // 10 + m, yy + lh - s, f"R {fmt_bytes(d.get('rd_s', 0))} W {fmt_bytes(d.get('wr_s', 0))}", s, limit=iw - iw * 4 // 10 - m)
    if len(shown) < len(snap["disks"]) and fit:
        pg.text(i, x, y + len(shown) * per, f"+{len(snap['disks']) - len(shown)} MORE")

    # top five processes
    b = R["proc"]
    i = pg.box(*b, "TOP PROCESSES")
    x, y, iw, ih = inner(b)
    for k, (name, cpu) in enumerate(snap["procs"][:5]):
        if (k + 1) * lh <= ih + 2 * s:
            pct = f"{cpu:.0f}%"
            pg.text(i, x, y + k * lh, name, limit=iw - 6 * s * 5)
            pg.text(i, x + iw - 6 * s * len(pct) + s, y + k * lh, pct, limit=6 * s * len(pct))
    return pg


# ---------------------------------------------------------------- the program
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="/dev/ttyACM0 or COM5 (default: the first device found)")
    ap.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS")
    ap.add_argument("--interval", type=float, default=5, help="seconds between pages (default 5)")
    ap.add_argument("--depth", type=int, default=1, choices=(1, 2, 4), help="gray bits (default 1: fastest partial refresh)")
    ap.add_argument("--cleanup-areas", type=int, metavar="N",
                    help="clean the changed parts every N updates (device default 15, 0 never; stored on the device)")
    ap.add_argument("--cleanup-screen", type=int, metavar="M",
                    help="refresh the whole screen every M updates (device default 0 never; stored on the device)")
    ap.add_argument("--once", action="store_true", help="draw one page and exit")
    ap.add_argument("--verbose", action="store_true", help="one line per cycle: host and device times, mode, why")
    ap.add_argument("--png", metavar="FILE", help="write the page to a PNG and exit (no device)")
    ap.add_argument("--size", default="800x480", help="PNG size WxH (default 800x480)")
    ap.add_argument("--demo", action="store_true", help="with --png: fixed fake numbers instead of this machine")
    args = ap.parse_args(argv)
    tpd.log(None)
    if args.png:
        w, h = (int(v) for v in args.size.lower().split("x"))
        if args.demo:
            snap = demo_snapshot()
        else:
            smp = Sampler(make_backend(), args.interval)
            smp.sample()
            time.sleep(1)
            smp.cycle = 0
            snap = smp.sample()
        Path(args.png).write_bytes(render(snap, w, h, args.depth).c.png(args.depth))
        print(f"wrote {args.png} ({w}x{h}, {args.depth} bit)")
        return 0
    sampler = Sampler(make_backend(), args.interval)
    sampler.sample()  # the first counters; rates start with the next sample
    try:
        with tpd.open(args.port, args.bluetooth) as d:
            given = {k: v for k, v in (("areas", args.cleanup_areas), ("screen", args.cleanup_screen)) if v is not None}
            if given:
                d.configure(cleanup=given)
            slots = sampler.n
            nxt = time.monotonic()
            while True:
                t0 = time.monotonic()
                page = render(sampler.sample(), d.w, d.h, args.depth, slots)
                try:
                    r = d.show(page.c, depth=args.depth)
                    if args.verbose:
                        cleaned = tpd.display_fw.clean_text(r)
                        print(f"prepare {r['prepare_ms']} ms, transfer {r['transfer_ms']} ms, device {r.get('ms')} ms, "
                              f"{r.get('mode')}, {r.get('why')}{', ' + cleaned if cleaned else ''}; drawn in {round((time.monotonic() - t0) * 1000)} ms", flush=True)
                except tpd.DisplayError as e:
                    print(f"show failed, trying again next cycle: {e}", file=sys.stderr)
                if args.once:
                    return 0
                nxt += args.interval
                time.sleep(max(0.0, nxt - time.monotonic()))
    except KeyboardInterrupt:
        print()  # the with block has closed the device
        return 0


if __name__ == "__main__":
    sys.exit(main())
