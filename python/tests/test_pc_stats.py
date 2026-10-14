# SPDX-License-Identifier: MIT
"""examples/pc_stats.py without a device and without psutil: the page layout from a fixed snapshot (no box overlaps
another, everything inside its box and the canvas), the /proc and /sys parsers on sample file contents, and the rates."""
import importlib.util
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[1] / "examples" / "pc_stats.py"
spec = importlib.util.spec_from_file_location("pc_stats", SRC)
ps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps)

SIZES = [(1872, 1404), (800, 480)]


def inside(a, b):
    return a[0] >= b[0] and a[1] >= b[1] and a[0] + a[2] <= b[0] + b[2] and a[1] + a[3] <= b[1] + b[3]


def apart(a, b):
    return a[0] + a[2] <= b[0] or b[0] + b[2] <= a[0] or a[1] + a[3] <= b[1] or b[1] + b[3] <= a[1]


def check_layout(pg):
    items = pg.items
    assert any(k == "text" for k, _, _ in items)
    boxes = [(i, r) for i, (k, r, p) in enumerate(items) if p is None]
    for n, (i, a) in enumerate(boxes):
        assert inside(a, (0, 0, pg.w, pg.h)), f"box {i} {a} leaves the canvas"
        for j, b in boxes[n + 1:]:
            assert apart(a, b), f"boxes {i} {a} and {j} {b} overlap"
    for i, (kind, r, parent) in enumerate(items):
        if parent is None:
            continue
        assert inside(r, items[parent][1]), f"{kind} {i} {r} leaves its box {items[parent][1]}"
        assert inside(r, (0, 0, pg.w, pg.h))
        for j, (k2, r2, p2) in enumerate(items):
            if j > i and p2 == parent:
                assert apart(r, r2), f"{kind} {i} {r} overlaps {k2} {j} {r2} in box {parent}"


@pytest.mark.parametrize("w,h", SIZES)
@pytest.mark.parametrize("depth", [1, 4])
def test_demo_page_layout(w, h, depth):
    pg = ps.render(ps.demo_snapshot(), w, h, depth)
    assert (pg.c.w, pg.c.h) == (w, h)
    check_layout(pg)
    assert len(pg.c.png(depth)) > 100
    assert 0 in pg.c.px and 15 in pg.c.px


@pytest.mark.parametrize("w,h", SIZES)
def test_layout_survives_bad_data(w, h):
    snap = ps.demo_snapshot()
    snap.update(temps=[], battery=None, load=None, procs=[], disks=[], cores=[50.0] * 128,
                host="a-very-long-host-name-" * 6, os="x" * 200, cpu_hist=[], down_hist=[], up_hist=[])
    check_layout(ps.render(snap, w, h))
    snap["disks"] = [{"mount": "/mnt/disk%d" % i, "used": 1, "total": 2, "rd_s": 1e9, "wr_s": 1e12} for i in range(30)]
    snap["temps"] = [{"name": "CPU", "now": 99.0, "hist": [99.0]}]
    snap["procs"] = [("p" * 80, 1234.0)] * 5
    check_layout(ps.render(snap, w, h))


def test_layout_check_sees_an_overlap():  # the guard itself must be able to fail
    pg = ps.render(ps.demo_snapshot(), 800, 480)
    boxes = [i for i, (k, r, p) in enumerate(pg.items) if p is None]
    k, r, p = pg.items[boxes[1]]
    pg.items[boxes[1]] = (k, (r[0] - 20, r[1], r[2], r[3]), p)
    with pytest.raises(AssertionError):
        check_layout(pg)


STAT = """cpu  1000 100 500 8000 400 10 20 5 0 0
cpu0 600 50 250 3900 200 5 10 2 0 0
cpu1 400 50 250 4100 200 5 10 3 0 0
cpu10 1 0 1 8 0 0 0 0 0 0
intr 12345
"""


def test_parse_cpu_stat():
    total, cores = ps.parse_cpu_stat(STAT)
    assert total == (1000 + 100 + 500 + 10 + 20 + 5, 10035)  # busy = all but idle and iowait
    assert [c[1] for c in cores] == [5017, 5018, 10]  # cpu0, cpu1, cpu10 in numeric order


def test_parse_meminfo():
    m = ps.parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 400 kB\nSwapTotal: 200 kB\nSwapFree: 150 kB\n")
    assert m == {"mem_used": 600 * 1024, "mem_total": 1000 * 1024, "swap_used": 50 * 1024, "swap_total": 200 * 1024}
    assert ps.parse_meminfo("")["mem_total"] == 0  # a missing file: zeros, no crash


def test_parse_net_diskstats_mounts_load_uptime():
    net = ("Inter-|   Receive | Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets\n"
           "    lo: 999 1 0 0 0 0 0 0 999 1 0 0 0 0 0 0\n  eth0: 5000 10 0 0 0 0 0 0 7000 9 0 0 0 0 0 0\n"
           " wlan0: 100 1 0 0 0 0 0 0 50 1 0 0 0 0 0 0\n")
    assert ps.parse_net_dev(net) == (5100, 7050)
    assert ps.parse_diskstats("   8  0 sda 10 0 2000 5 20 0 4000 9 0 0 0\n 259 1 nvme0n1p1 1 0 8 0 1 0 16 0\n") == {
        "sda": (2000 * 512, 4000 * 512), "nvme0n1p1": (8 * 512, 16 * 512)}
    mounts = "proc /proc proc rw 0 0\n/dev/sda2 / ext4 rw 0 0\n/dev/sda2 /var/x ext4 rw 0 0\n/dev/loop3 /snap/a squashfs ro 0 0\n/dev/sdb1 /mnt/my\\040disk ext4 rw 0 0\n"
    assert ps.parse_mounts(mounts) == [("/dev/sda2", "/"), ("/dev/sdb1", "/mnt/my disk")]
    assert ps.parse_loadavg("0.52 0.41 0.30 1/345 6789\n") == (0.52, 0.41, 0.30)
    assert ps.parse_uptime("12345.67 99999.00\n") == 12345.67


def test_parse_pid_stat_with_odd_command_names():
    tail = "S 1 2 3 4 5 6 7 8 9 10 250 50 0 0 20"
    assert ps.parse_pid_stat(f"42 (my (odd) cmd) {tail}", 100) == ("my (odd) cmd", 3.0)
    with pytest.raises(ValueError):
        ps.parse_pid_stat("garbage", 100)


def test_classify_temp():
    assert [ps.classify_temp(n) for n in ("coretemp", "k10temp", "amdgpu", "nvme", "iwlwifi_1")] == ["CPU", "CPU", "GPU", "NVME", None]


def put(root, rel, text):
    f = Path(root) / rel
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(text)


def test_proc_backend_on_a_copy_of_the_files(tmp_path):
    put(tmp_path, "sys/class/hwmon/hwmon0/name", "coretemp\n")
    put(tmp_path, "sys/class/hwmon/hwmon0/temp1_input", "58500\n")
    put(tmp_path, "sys/class/hwmon/hwmon1/name", "amdgpu\n")
    put(tmp_path, "sys/class/hwmon/hwmon1/temp1_input", "47000\n")
    put(tmp_path, "sys/class/power_supply/BAT0/capacity", "76\n")
    put(tmp_path, "sys/class/power_supply/BAT0/status", "Charging\n")
    put(tmp_path, "etc/os-release", 'NAME="Debian"\nPRETTY_NAME="Debian GNU/Linux 12 (bookworm)"\n')
    put(tmp_path, "proc/loadavg", "0.1 0.2 0.3 1/2 3\n")
    put(tmp_path, "proc/1/stat", "1 (init) S 0 1 1 0 -1 4194560 1 2 3 4 100 100 0 0 20\n")
    b = ps.ProcBackend(tmp_path)
    assert b.temps() == {"CPU": 58.5, "GPU": 47.0}
    assert b.battery() == (76, True)
    assert b.os_name() == "Debian GNU/Linux 12 (bookworm)"
    assert b.load() == (0.1, 0.2, 0.3)
    assert b.procs() == {1: ("init", 200 / b.ticks)}


def test_proc_backend_with_nothing_there_gives_empty_values(tmp_path):  # no sensors, no battery, a minimal container
    b = ps.ProcBackend(tmp_path)
    assert b.temps() == {} and b.battery() is None and b.load() is None and b.disks() == [] and b.procs() == {}
    assert b.cpu() == ((0, 0), []) and b.net() == (0, 0) and b.uptime() == 0.0


class Fake:
    """A backend whose counters grow by one fixed step per call."""

    def __init__(self):
        self.n = 0

    def cpu(self):
        self.n += 1
        return (50 * self.n, 100 * self.n), [(10 * self.n, 100 * self.n), (90 * self.n, 100 * self.n)]

    def net(self):
        return 1000 * self.n, 100 * self.n

    def memory(self):
        return {"mem_used": 1, "mem_total": 2, "swap_used": 0, "swap_total": 0}

    def temps(self):
        return {"CPU": 50.0 + self.n}

    def disks(self):
        return [{"mount": "/", "used": 1, "total": 2, "rd": 100 * self.n, "wr": 0}]

    def procs(self):
        return {1: ("a", 1.0 * self.n), 2: ("b", 0.0)}

    load = battery = lambda self: None
    uptime = lambda self: 5.0  # noqa: E731
    os_name = lambda self: "Fake"  # noqa: E731


def test_sampler_rates_history_and_slow_cycles():
    sm = ps.Sampler(Fake(), 5)
    first = sm.sample()
    second = sm.sample()
    assert second["cpu"] == pytest.approx(50.0) and second["cores"] == pytest.approx([10.0, 90.0])
    assert second["down"] > 0 and len(second["cpu_hist"]) == 2
    sm.sample()
    sm.sample()  # cycle 3: disks and processes are read again, between them the old values stay
    assert second["disks"] is first["disks"] or second["disks"] == first["disks"]
    assert [p[0] for p in sm.procs] == ["a", "b"] and sm.procs[0][1] > 0
    assert sm.sample()["procs"] == sm.procs
    assert len(sm.hist["cpu"]) <= sm.n == 60


# ---- real drives only, idle cores visible, the full-refresh note
MOUNTS = """sysfs /sys sysfs rw 0 0
/dev/sda2 / ext4 rw 0 0
/dev/sda1 /boot/efi vfat rw 0 0
/dev/sda3 /boot ext4 rw 0 0
/dev/loop3 /snap/core/1 squashfs ro 0 0
/dev/sdc1 /snap/odd ext4 rw 0 0
/dev/sdd1 /usr/src ext4 rw 0 0
/dev/sde1 /media/x/squash squashfs ro 0 0
overlay /var/lib/docker/overlay2/x overlay rw 0 0
tmpfs /run tmpfs rw 0 0
devtmpfs /dev devtmpfs rw 0 0
/dev/sdb1 /mnt/my\\040disk ext4 rw 0 0
/dev/nvme0n1p1 /home/data xfs rw 0 0
"""


def test_parse_mounts_keeps_only_real_drives():
    assert ps.parse_mounts(MOUNTS) == [("/dev/sda2", "/"), ("/dev/sdb1", "/mnt/my disk"), ("/dev/nvme0n1p1", "/home/data")]


@pytest.mark.parametrize("fstype,device,mount,opts,real", [
    ("ext4", "/dev/sda2", "/", "rw", True),
    ("squashfs", "/dev/loop1", "/snap/x", "ro", False),
    ("overlay", "overlay", "/", "rw", False),
    ("tmpfs", "tmpfs", "/run", "rw", False),
    ("devtmpfs", "devtmpfs", "/dev", "rw", False),
    ("ext4", "/dev/sdc1", "/snap/x", "rw", False),
    ("ext4", "/dev/sdc1", "/usr/src", "rw", False),
    ("vfat", "/dev/sdc1", "/boot/efi", "rw", False),
    ("NTFS", "C:\\", "C:\\", "rw,fixed", True),
    ("", "D:\\", "D:\\", "cdrom", False),
    ("FAT32", "E:\\", "E:\\", "rw,removable", False),
])
def test_real_drive_filter(fstype, device, mount, opts, real):
    assert ps.is_real_drive(fstype, device, mount, opts) is real


def test_idle_cores_still_show_a_baseline():
    for depth in (1, 4):
        pg = ps.Page(200, 100, depth)
        pg.items.append(("box", (0, 0, 200, 100), None))
        pg.cores(0, 10, 10, 100, 40, [0.0] * 4)
        row = pg.c.px[(10 + 40 - pg.t - 1) * 200 + 10:(10 + 40 - pg.t - 1) * 200 + 110]
        assert row.count(pg.c.px[10 * 200 + 10]) < 100  # not all the frame colour
        assert any(v != 15 for v in row[pg.t:-pg.t]), "idle cores draw nothing"


def test_full_refresh_note():
    assert "full" in ps.refresh_note({"mode": "full", "why": "every 30"})
    assert "every 30" in ps.refresh_note({"mode": "full", "why": "every 30"})
    assert ps.refresh_note({"mode": "partial", "why": "-"}) is None
