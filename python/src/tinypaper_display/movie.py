#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Play a video on a tiny-paper display, one frame every few seconds.
"""Show frames of a video on the display firmware, one every --every seconds for --duration seconds.

The frames are taken evenly across the film (or --step seconds of film apart), fitted to the panel on white and
shown as they are. Needs ffmpeg and ffprobe on the PATH. With --dry-run DIR no device is used: the PNGs that would be
sent are written there.

  movie.py /dev/ttyACM0 film.mp4 --every 5 --duration 60
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


def play(video, every, duration, show, w, h, step=None, start=0.0, fill=False, sleep=time.sleep, now=time.monotonic):
    """Show the frames through `show(index, at, png)`, one each `every` seconds. Returns how many were shown."""
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
    p.add_argument("port", help="/dev/ttyACM0, COM5, socket://127.0.0.1:7701 (sim), or - with --dry-run")
    p.add_argument("video", type=Path)
    p.add_argument("--every", type=float, default=5, help="seconds between frames on the display (default 5)")
    p.add_argument("--duration", type=float, default=60, help="how long to play, in seconds (default 60)")
    p.add_argument("--step", type=float, help="seconds of film between frames (default: spread over the whole film)")
    p.add_argument("--start", type=float, default=0, help="where in the film to begin, in seconds")
    p.add_argument("--fill", action="store_true", help="fill the panel and crop, instead of fitting on white")
    p.add_argument("--dry-run", type=Path, metavar="DIR", help="write the PNGs to DIR instead of a device")
    p.add_argument("--size", default="1872x1404", help="panel size for --dry-run (default the TRMNL X)")
    a = p.parse_args()
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

            n = play(a.video, a.every, a.duration, save, w, h, a.step, a.start, a.fill, sleep=lambda _s: None)
            print(f"{n} frames in {a.dry_run}")
            return 0
        from . import display_fw
        from .usb_display import Display, DisplayError, codec
        try:
            with codec.open_serial(a.port) as port:
                port.timeout, port.write_timeout = 0.05, 2
                display = Display(port)
                hello = display.hello()
                if not display_fw.is_display_firmware(hello):
                    raise MovieError("this device does not run the display firmware (hello has no project: display)")
                print(f"{hello['board']} {hello['w']}x{hello['h']}, one frame every {a.every:g} s for {a.duration:g} s")

                def show(i, at, png):
                    reply = display_fw.show_png(display, display_fw.panel_png(png, hello), hello)
                    print(f"frame {i}: film {at:7.1f} s, panel {reply.get('ms')} ms")

                n = play(a.video, a.every, a.duration, show, hello["w"], hello["h"], a.step, a.start, a.fill)
                print(f"{n} frames shown")
                return 0
        except DisplayError as e:
            raise MovieError(str(e)) from e
    except (MovieError, ValueError, OSError, subprocess.TimeoutExpired) as e:
        print(f"movie: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
