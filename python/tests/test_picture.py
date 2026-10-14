# SPDX-License-Identifier: MIT
"""The picture preparation gives the web page's pixels: every case of docs/spec/vectors/picture-prepare.json (made
from site/picture.js by gen_picture.mjs, read by site/test/picture-vectors.test.mjs too)."""
import json

import pytest

from tiny_paper_display import picture
from tiny_paper_display.usb_display import DisplayError, ROOT

DATA = json.loads((ROOT / "docs/spec/vectors/picture-prepare.json").read_text())
SOURCES = {s["name"]: picture.Source(s["w"], s["h"], bytes.fromhex(s["rgba"])) for s in DATA["sources"]}


def test_vectors_cover_every_setting():
    seen = {k: {c["settings"][k] for c in DATA["cases"]} for k in ("fit", "border", "rotate", "depth", "dither", "threshold", "color")}
    assert seen["fit"] == {"fit", "fill", "stretch"} and seen["border"] == {"white", "black"}
    assert {"auto", 0, 90, 180, 270} <= seen["rotate"] and seen["depth"] == {1, 2, 4}
    assert seen["dither"] == {"floyd", "ordered", "none"} and len(seen["threshold"]) >= 3 and seen["color"] == {True, False}


@pytest.mark.parametrize("case", DATA["cases"], ids=lambda c: c["id"])
def test_same_pixels_as_the_web_page(case):
    levels, rotation = picture.prepare(SOURCES[case["src"]], case["panel"]["w"], case["panel"]["h"], **case["settings"])
    assert levels.hex() == case["levels"], case["settings"]
    assert rotation == case["rotation"]


def test_a_wrong_setting_is_refused():
    with pytest.raises(DisplayError):
        picture.prepare(SOURCES["wide"], 8, 6, dither="atkinson")
    with pytest.raises(DisplayError):
        picture.prepare(SOURCES["wide"], 8, 6, depth=3)
