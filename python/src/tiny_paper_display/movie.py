#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Play a video on a tiny-paper display, one frame every few seconds.
"""Show frames of a video on the display firmware, one every --every seconds for --duration seconds.

The frames are taken evenly across the film (or --step seconds of film apart), fitted to the panel on white and
shown as they are. Needs ffmpeg and ffprobe on the PATH. With --dry-run DIR no device is used: the PNGs that would be
sent are written there.

--mode partial (the default): black and white frames, each sent as the rectangle that changed against the last frame
shown (the whole panel when most of it changed), without a flash, and a full refresh every --full-every frames as
clean-up. --mode full: every frame whole at the panel's deepest gray, with a full refresh.

  movie.py /dev/ttyACM0 film.mp4 --every 5 --duration 60
  movie.py --bluetooth tp-display-1A2B film.mp4 --every 5 --duration 60
  movie.py - film.mp4 --every 5 --dry-run frames/
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

FFMPEG_S = 60  # one frame must come out within this time


class MovieError(Exception):
    pass


def film_seconds(video):
    """Length of the film in seconds, from ffprobe."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "json", str(video)],
        capture_output=True, timeout=FFMPEG_S, check=False)
    if out.returncode != 0:
        raise MovieError(f"ffprobe cannot read {video}: {out.stderr.decode(errors='replace').strip()[:200]}")
    try:
        seconds = float(json.loads(out.stdout)["format"]["duration"])
    except (KeyError, ValueError) as e:
        raise MovieError(f"{video} has no length ffprobe can tell") from e
    if seconds <= 0:
        raise MovieError(f"{video} is empty")
    return seconds


def frame_times(length, count, step=None, start=0.0):
    """The film positions to show: `count` of them, `step` seconds apart, or spread evenly over what is left."""
    if count < 1:
        raise MovieError("nothing to show: --duration is shorter than --every")
    left = max(length - start, 0.0)
    if step is None:
        step = left / count
    return [min(start + i * step, max(length - 0.05, 0.0)) for i in range(count)]


def frame_png(video, at, w, h, fill=False):
    """One frame at `at` seconds as a gray PNG of exactly w by h: fitted on white, or filling the panel when `fill`."""
    if fill:
        fit = f"scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h}"
    else:
        fit = f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:white"
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", f"{at:.3f}", "-i", str(video), "-frames:v", "1",
         "-vf", fit + ",format=gray", "-f", "image2pipe", "-vcodec", "png", "-"],
        capture_output=True, timeout=FFMPEG_S, check=False)
    if out.returncode != 0 or not out.stdout.startswith(b"\x89PNG"):
        raise MovieError(f"ffmpeg gave no frame at {at:.1f} s: {out.stderr.decode(errors='replace').strip()[:200]}")
    return out.stdout


def frame_rows(video, at, w, h, fill=False):
    """One frame at `at` seconds as packed 1 bit rows (ceil(w / 8) bytes a row, left pixel in the high bit, 0 black),
    dithered by ffmpeg: what a partial refresh takes."""
    if fill:
        fit = f"scale={w}:{h}:force_original_aspect_ratio=increase,crop={w}:{h}"
    else:
        fit = f"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:white"
    out = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", f"{at:.3f}", "-i", str(video), "-frames:v", "1",
         "-vf", fit, "-f", "rawvideo", "-pix_fmt", "monob", "-"],
        capture_output=True, timeout=FFMPEG_S, check=False)
    if out.returncode != 0 or len(out.stdout) != (w + 7) // 8 * h:
        raise MovieError(f"ffmpeg gave no frame at {at:.1f} s: {out.stderr.decode(errors='replace').strip()[:200]}")
    return out.stdout


class Partial:
    """What to send for each frame of packed 1 bit rows: the rectangle that changed against the last frame sent."""

    def __init__(self, w, h, full_every=20):
        self.w, self.h, self.full_every, self.last, self.since_full = w, h, full_every, None, 0

    def next(self, rows):
        """("full" | "partial", (x, y, w, h), png) for this frame, or None when it is the frame shown. full: the whole
        frame with a full refresh (the first one, and after full_every partial ones); partial: the changed rectangle,
        the whole panel when it covers more than half of it."""
        from . import display_fw
        whole = (0, 0, self.w, self.h)
        if self.last is None or (self.full_every and self.since_full >= self.full_every):
            self.last, self.since_full = rows, 0
            return "full", whole, display_fw.write_png(self.w, self.h, 1, rows)
        rect = display_fw.changed_rect(self.last, rows, self.w, self.h, 1)
        if rect is None:
            return None
        if rect[2] * rect[3] * 2 > self.w * self.h:
            rect = whole
        self.last, self.since_full = rows, self.since_full + 1
        return "partial", rect, display_fw.crop_png(rows, self.w, 1, rect)

    def forget(self):
        """The device did not keep the frame (it answered with a full refresh nobody asked for): send a whole one."""
        self.last = None


def dry_partial(folder, w, h, full_every, out=print):
    """The --dry-run of --mode partial: `save(index, at, rows)` writes what would be sent and says where it goes."""
    plan = Partial(w, h, full_every)

    def save(i, at, rows):
        step = plan.next(rows)
        if step is None:
            out(f"frame {i}: film {at:7.1f} s, unchanged, nothing sent")
            return
        kind, (x, y, rw, rh), png = step
        (folder / f"frame-{i:03d}-{kind}-{x}-{y}-{rw}x{rh}.png").write_bytes(png)
        out(f"frame {i}: film {at:7.1f} s, {kind} {x},{y} {rw}x{rh}, {len(png)} bytes")

    return save


def play(video, every, duration, show, w, h, step=None, start=0.0, fill=False, sleep=time.sleep, now=time.monotonic,
         frame_png=frame_png):
    """Show the frames through `show(index, at, frame)`, one each `every` seconds; frame_png(video, at, w, h, fill)
    makes each (a PNG, or frame_rows' packed rows). Returns how many were shown."""
    if every <= 0:
        raise MovieError("--every must be more than 0")
    times = frame_times(film_seconds(video), int(duration // every), step, start)
    began = now()
    for i, at in enumerate(times):
        png = frame_png(video, at, w, h, fill)
        wait = began + i * every - now()  # the frame is ready early: hold it until its turn
        if wait > 0:
            sleep(wait)
        show(i, at, png)
    return len(times)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("port", nargs="?", help="/dev/ttyACM0, COM5, socket://127.0.0.1:7701 (sim), or - with --dry-run; "
                                           "not with --bluetooth")
    p.add_argument("video", type=Path, nargs="?")
    p.add_argument("--bluetooth", metavar="NAME_OR_ADDRESS",
                   help="over Bluetooth to the device with this name (tp-display-XXXX) or address instead of a port "
                        "(pip install tiny-paper-display[bluetooth], paired with tiny-paper-display setup)")
    p.add_argument("--every", type=float, default=5, help="seconds between frames on the display (default 5)")
    p.add_argument("--duration", type=float, default=60, help="how long to play, in seconds (default 60)")
    p.add_argument("--step", type=float, help="seconds of film between frames (default: spread over the whole film)")
    p.add_argument("--start", type=float, default=0, help="where in the film to begin, in seconds")
    p.add_argument("--fill", action="store_true", help="fill the panel and crop, instead of fitting on white")
    p.add_argument("--dry-run", type=Path, metavar="DIR", help="write the PNGs to DIR instead of a device")
    p.add_argument("--size", default="1872x1404", help="panel size for --dry-run (default the TRMNL X)")
    p.add_argument("--force", action="store_true", help="upload unchanged frames too")
    p.add_argument("--mode", choices=("partial", "full"), default="partial",
                   help="partial (default): black and white, only the changed rectangle, no flash; full: gray, a full "
                        "refresh every frame")
    p.add_argument("--full-every", type=int, default=20, metavar="N",
                   help="with partial: a full refresh every N frames as clean-up (default 20, 0 never)")
    a = p.parse_args()
    if a.bluetooth and a.video is None and a.port is not None:
        a.port, a.video = None, Path(a.port)  # the one positional is the film
    if a.video is None or (a.port is None) == (a.bluetooth is None):
        p.error("a port and a video, or --bluetooth NAME_OR_ADDRESS and a video")
    try:
        for tool in ("ffmpeg", "ffprobe"):
            if not shutil.which(tool):
                raise MovieError(f"{tool} is not installed")
        if not a.video.is_file():
            raise MovieError(f"no such file: {a.video}")
        if a.dry_run:
            w, h = (int(v) for v in a.size.lower().split("x"))
            a.dry_run.mkdir(parents=True, exist_ok=True)

            def save(i, at, png):
                (a.dry_run / f"frame-{i:03d}.png").write_bytes(png)
                print(f"frame {i}: film {at:7.1f} s, {len(png)} bytes")

            if a.mode == "partial":
                if w % 8:
                    raise MovieError("--size: the width is a multiple of 8")
                save, source = dry_partial(a.dry_run, w, h, a.full_every), frame_rows
            else:
                source = frame_png
            n = play(a.video, a.every, a.duration, save, w, h, a.step, a.start, a.fill, sleep=lambda _s: None,
                     frame_png=source)
            print(f"{n} frames in {a.dry_run}")
            return 0
        from . import display_fw
        from .usb_display import Display, DisplayError, open_port
        try:
            with open_port(a.port, a.bluetooth) as port:
                display = Display(port)
                hello = display.hello()
                if not display_fw.is_display_firmware(hello):
                    raise MovieError("this device does not run the display firmware (hello has no project: display)")
                print(f"{hello['board']} {hello['w']}x{hello['h']}, one frame every {a.every:g} s for {a.duration:g} s")

                partial = a.mode == "partial"
                if partial and 1 not in hello.get("partial_depths", []):
                    print(f"{hello['board']} has no 1 bit partial refresh: every frame with a full refresh (--mode full)")
                    partial = False
                plan = Partial(hello["w"], hello["h"], a.full_every)

                def show(i, at, png):
                    reply = display_fw.show_png(display, display_fw.panel_png(png, hello), hello,
                                                mode="full" if a.mode == "full" else None, force=a.force)
                    print(f"frame {i}: film {at:7.1f} s, {display_fw.describe(reply)}")

                def show_partial(i, at, rows):
                    # Validate every duplicate with the device: the host plan alone cannot see a cut transfer.
                    whole_png = display_fw.write_png(hello["w"], hello["h"], 1, rows)
                    value = display_fw.fingerprint(whole_png, hello)
                    if not a.force and value in (hello.get("fingerprint"), getattr(display, "_display_known", None)):
                        reply = display_fw.show_png(display, whole_png, hello)
                        print(f"frame {i}: film {at:7.1f} s, {display_fw.describe(reply)}")
                        plan.last = rows
                        if reply.get("mode") == "full":
                            plan.since_full = 0
                        return
                    step = plan.next(rows)
                    if step is None:
                        reply = display_fw.show_png(display, whole_png, hello, force=a.force)
                        print(f"frame {i}: film {at:7.1f} s, {display_fw.describe(reply)}")
                        plan.last = rows
                        if reply.get("mode") == "full":
                            plan.since_full = 0
                        return
                    kind, (x, y, rw, rh), png = step
                    if (rw, rh) == (hello["w"], hello["h"]):
                        reply = display_fw.show_png(display, png, hello, mode=kind, force=a.force)
                    else:
                        reply = display_fw.show_area(display, png, hello, x, y, mode="partial", force=a.force)
                    if reply.get("mode") == "full" and kind != "full":
                        plan.forget()  # the device had lost its frame: the rest of the panel is white now
                    print(f"frame {i}: film {at:7.1f} s, {display_fw.describe(reply)}")

                n = play(a.video, a.every, a.duration, show_partial if partial else show, hello["w"], hello["h"], a.step,
                         a.start, a.fill, frame_png=frame_rows if partial else frame_png)
                print(f"{n} frames shown")
                return 0
        except DisplayError as e:
            raise MovieError(str(e)) from e
    except (MovieError, ValueError, OSError, subprocess.TimeoutExpired) as e:
        print(f"movie: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
