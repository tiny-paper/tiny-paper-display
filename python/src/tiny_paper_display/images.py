# SPDX-License-Identifier: MIT
# Any picture the API takes, as a picture.Source (RGBA).
"""load(picture): a path, bytes, a Canvas or a picture.Source as a Source. PNG needs nothing else (png_image); JPEG and
the other formats, and PNG kinds png_image refuses (16 bit, interlaced), go through Pillow when it is installed."""
import io
import struct
from pathlib import Path

from . import png_image
from .canvas import Canvas
from .picture import Source
from .usb_display import DisplayError

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_BYTES = png_image.MAX_BYTES
NEEDS_PILLOW = "this picture is not a PNG this package reads: pip install Pillow (JPEG, WebP, GIF and more), or give a PNG"


def read_file(path):
    path = Path(path)
    try:
        with path.open("rb") as f:
            data = f.read(MAX_BYTES + 1)
    except OSError as exc:
        raise DisplayError(f"cannot read {path}: {exc.strerror or exc}") from exc
    if len(data) > MAX_BYTES:
        raise DisplayError("the picture exceeds 16 MiB")
    return data


def png_size(data):
    """(w, h) of a PNG from its header, None when the bytes are not a PNG."""
    if len(data) < 24 or data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def _pillow(data):
    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise DisplayError(NEEDS_PILLOW) from exc
    try:
        Image.MAX_IMAGE_PIXELS = 48_000_000
        im = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGBA")
    except Exception as exc:  # Pillow raises its own and the system's errors
        raise DisplayError(f"cannot read the picture: {exc}") from exc
    return Source(im.width, im.height, im.tobytes())


def from_bytes(data):
    if len(data) > MAX_BYTES:
        raise DisplayError("the picture exceeds 16 MiB")
    if data[:8] == PNG_SIGNATURE:
        try:
            w, h, pixels = png_image.decode_png(data, keep_alpha=True)
        except ValueError as exc:
            try:
                return _pillow(data)
            except DisplayError as pil:
                raise DisplayError(f"{exc} ({pil})") from exc
        return Source(w, h, bytes(v for p in pixels for v in p))
    return _pillow(data)


def load(picture):
    if isinstance(picture, Source):
        return picture
    if isinstance(picture, Canvas):
        return Source(picture.w, picture.h, picture.rgba())
    if isinstance(picture, (bytes, bytearray, memoryview)):
        return from_bytes(bytes(picture))
    if isinstance(picture, (str, Path)):
        return from_bytes(read_file(picture))
    raise DisplayError("a picture is a path, PNG or JPEG bytes, or a Canvas")
