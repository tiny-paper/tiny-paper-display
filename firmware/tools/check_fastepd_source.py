#!/usr/bin/env python3
"""Guard: an ESP build compiles the panel library that was meant (firmware/components/epd/idf_component.yml).

    check_fastepd_source.py <compile_commands.json> <fix marker> <head marker> <mode> [<pin yml> <lock file> [<glue>
                            [<edge>]]]
    check_fastepd_source.py --pin <pin yml> <glue> <design doc>     (no build needed: make -C firmware test)

The pin is upstream FastEPD head plus the fork's fixes, the first of them one line in bbepPartialUpdate (a row range
no longer streaks). A build that compiles FastEPD must take it from managed_components, and its src/FastEPD.inl must
hold, by mode:

    ""         (the pin)               the fix marker and the head marker
    old        (FASTEPD_PIN=old)       the fix marker, not the head marker (the pin before upstream head)
    upstream   (FASTEPD_UPSTREAM=1)    neither (the build that shows the flaw)

With a pin and a lock file: the lock names the commit the pin names. The component manager keeps the lock's commit
when only the pin moves (same repository), so a pin moved alone would build the old library.
A build without FastEPD (the other boards) passes.

The mirrored rectangle: the X sends its rows last pixel first and the library applies a rectangle's column mask in
sent order, so the glue hands it a mirrored x (MIRROR_X in epd_fastepd.cpp). A library with the fix for that (fork
commit cb95742 and every branch that carries it, or upstream after such a fix) wants the true x: with the glue's
mirror still on, every gray box would be driven at the mirrored place. So the pin and the constant move in one
commit: --pin refuses a pin that is one of the commits known to carry the fix while MIRROR_X is true, and the
build check refuses a compiled library whose bbepFixRect mirrors the columns while MIRROR_X is true (and one that
does not while MIRROR_X is false). The glue and the design doc must both name the rule (MIRROR_RULE).
The right edge tries (X_EDGE=off|lead|dc|both, PLAN.md "Upstream"): with <edge> the
library's compile lines hold exactly that variant's defines (off: none, the released behaviour), the glue's lines the
matching EPD_X_EDGE_* (the log's edge= then tells the truth), and no other file is compiled with either.
--pin also wants one manifest hash in every dependencies.lock.* of the projects next to the pin (display).
The erase passes (issue #268, withdrawn 2026-10-10): no library compiled may hold setErasePasses or
bbepSetErasePasses in its src/, in any mode. The fork branches with them go, so a pin back on one must not build.
"""
import json
import re
import sys
from pathlib import Path

COMPONENT = "bitbank2__fastepd"
MODES = {"": (True, True), "old": (True, False), "upstream": (False, False)}  # mode: (fix, head) expected
NAMES = {"": "the pin", "old": "FASTEPD_PIN=old", "upstream": "FASTEPD_UPSTREAM=1"}

# The fork's mirrored rectangle fix (cb95742) in bbepFixRect, as the compiled source shows it.
MIRROR_FIX = "native_width - 1 - *iEndCol"
# Every fork commit whose history holds that fix (git log 3d2a63a..<branch>, read 2026-10-09). A new branch on top of
# one of these is caught by the build check (MIRROR_FIX in the source), not by this list.
MIRROR_FIX_COMMITS = {
    "cb957424ff439fc3ef6d9faae7e6599de197c3ed": "rect-mirror-fix-head",
    "f1de583a4798fbbfde568e13eefe52d6ffc9fb32": "all-fixes-with-mirror-head~4 (cb95742 picked)",
    "112a3a3106255fcf3d9863e8b2b57eed4e017bfc": "all-fixes-with-mirror-head~3",
    "93e76d63c87aa9a94dbe03ada130e39819454885": "all-fixes-with-mirror-head~2",
    "5cda1787d578b8515cfd1f6c92ff270ac9f73cf8": "all-fixes-with-mirror-head~1",
    "52824f76db3ca8064013e5bdd0eb05913ecb6063": "all-fixes-with-mirror-head",
}
# The withdrawn erase passes (fork commit aaedab3): their public call and the function behind it.
ERASE_WORDS = ("bbepSetErasePasses", "setErasePasses")
# The words the glue (next to MIRROR_X) and PLAN.md must both hold.
MIRROR_RULE = "the pin and MIRROR_X move in one commit"
MIRROR_HINT = ("the library drives a rectangle at the x it is given, so the glue must stop mirroring it: set MIRROR_X "
               "false in components/epd/esp/epd_fastepd.cpp in the commit that moves the pin "
               "(PLAN.md \"Upstream\")")


# X_EDGE: the library's defines per variant; the glue's are EDGE_GLUE in the same order (lead word, start pulse).
EDGE_LIB = ("FASTEPD_X_LEAD_WORD", "FASTEPD_X_SPH_FROM_DC")
EDGE_GLUE = ("EPD_X_EDGE_LEAD", "EPD_X_EDGE_DC")
EDGES = {"off": (False, False), "lead": (True, False), "dc": (False, True), "both": (True, True)}
GLUE_FILE = "epd_fastepd.cpp"


def entry_defines(entry):
    """The -D names of one compile_commands entry (command string or arguments list)."""
    text = entry.get("command") or " ".join(entry.get("arguments", []))
    return set(re.findall(r"-D\s*([A-Za-z_][A-Za-z0-9_]*)", text))


def check_edge(commands, edge):
    """Every library file has the defines of this X_EDGE and no other, the glue the matching ones, nothing else any."""
    want = EDGES[edge]
    for entry in commands:
        path = Path(entry.get("file", ""))
        defs = entry_defines(entry)
        if COMPONENT in path.parts:
            names = EDGE_LIB
        elif path.name == GLUE_FILE:
            names = EDGE_GLUE
        else:
            stray = sorted(defs & set(EDGE_LIB + EDGE_GLUE))
            if stray:
                return f"{path} is compiled with {stray}: the right edge defines belong to the panel library only"
            continue
        have = tuple(n in defs for n in names)
        if have != want:
            on = [n for n, h in zip(names, have) if h] or ["none"]
            return (f"X_EDGE={edge} but {path} is compiled with {', '.join(on)} of {', '.join(names)}: a build "
                    "directory configured for another variant; build through firmware/tools/idf/build.sh")
    return None


def check_no_erase(src):
    """No source file of the compiled library names the withdrawn erase passes."""
    for path in sorted(src.iterdir()):
        if path.suffix not in (".inl", ".h", ".cpp", ".c"):
            continue
        try:
            text = path.read_text(errors="replace")
        except OSError as e:
            return f"cannot read {path}: {e}"
        for word in ERASE_WORDS:
            if word in text:
                return (f"{path} holds {word}: a library with the withdrawn erase passes (issue #268). The pin must "
                        "be a commit of the fork's all-fixes-edge-head, which never had them "
                        "(PLAN.md \"Upstream\")")
    return None


def glue_mirror(glue):
    """MIRROR_X of the glue: True, False, or None when the line is not found."""
    m = re.search(r"^static const bool MIRROR_X = (true|false);", glue, re.M)
    return None if not m else m.group(1) == "true"


def check_mirror_source(lib_text, glue_path):
    """The compiled library and the glue agree on who mirrors a rectangle's x."""
    try:
        mirror = glue_mirror(Path(glue_path).read_text())
    except OSError as e:
        return f"cannot read the glue: {e}"
    if mirror is None:
        return f"{glue_path} has no 'static const bool MIRROR_X = true|false;' line"
    fixed = MIRROR_FIX in lib_text
    if fixed and mirror:
        return (f"the compiled FastEPD has the mirrored rectangle fix ({MIRROR_FIX!r}) and MIRROR_X is true: "
                f"{MIRROR_HINT}")
    if not fixed and not mirror:
        return ("MIRROR_X is false but the compiled FastEPD lacks the mirrored rectangle fix: every gray box would "
                "land mirrored; set MIRROR_X true or pin a library with the fix")
    return None


def check_manifest_hashes(fw):
    """Every dependencies.lock.* of the display project names one manifest hash: the hash covers the pin file, so a
    build of any chip rewrites its lock after a pin move (seen 2026-10-09: the esp32s3 locks moved with the pin,
    the esp32c3 ones did not, and the next esp32c3 build left the tree dirty)."""
    seen = {}
    for lock in sorted(Path(fw).glob("*/dependencies.lock.*")):
        m = re.search(r"^manifest_hash: *(\S+)", lock.read_text(), re.M)
        seen.setdefault(m.group(1) if m else None, []).append(f"{lock.parent.name}/{lock.name}")
    if len(seen) > 1:
        return ("the lock files name different manifest hashes, they all move with the pin file (build each chip "
                "once and commit the locks): " + "; ".join(f"{h}: {', '.join(n)}" for h, n in seen.items()))
    return None


def check_pin(pin, glue_path, doc):
    """Without a build: the pin is a full commit, not one known to carry the mirror fix while the glue mirrors, and
    the rule is written down in the glue and in the design doc."""
    try:
        sha = pinned_commit(Path(pin).read_text())
        glue = Path(glue_path).read_text()
        doc_text = Path(doc).read_text()
    except OSError as e:
        return f"cannot read the pin, the glue or the doc: {e}"
    if not sha or not re.fullmatch(r"[0-9a-f]{40}", sha):
        return f"{pin}: the FastEPD version must be a full commit, not {sha!r} (a branch name moves)"
    mirror = glue_mirror(glue)
    if mirror is None:
        return f"{glue_path} has no 'static const bool MIRROR_X = true|false;' line"
    if sha in MIRROR_FIX_COMMITS and mirror:
        return (f"the pin {sha[:8]} ({MIRROR_FIX_COMMITS[sha]}) has the mirrored rectangle fix cb95742 and MIRROR_X "
                f"is true: {MIRROR_HINT}")
    for path, text in ((glue_path, glue), (doc, doc_text)):
        if MIRROR_RULE not in " ".join(text.replace("*", " ").split()):
            return f"{path} no longer says {MIRROR_RULE!r}: the rule on the mirrored rectangle must stay written there"
    return check_manifest_hashes(Path(pin).resolve().parents[2])  # firmware/, from components/epd/<pin>


def pinned_commit(text):
    """The version of bitbank2/fastepd in a manifest or a lock file (both list it as a 4-space key of that entry)."""
    m = re.search(r"^  bitbank2/fastepd:\n((?:    .*\n|\n)*)", text, re.M)
    v = re.search(r"^    version: *(\S+)", m.group(1), re.M) if m else None
    return v.group(1) if v else None


def check_lock(pin, lock):
    try:
        want = pinned_commit(Path(pin).read_text())
        text = Path(lock).read_text()
    except OSError as e:
        return f"cannot read the pin or the lock: {e}"
    have = pinned_commit(text)
    if not want:
        return f"{pin} names no FastEPD commit"
    if have != want:
        return (f"{lock} names FastEPD {have}, the pin {want}: the lock must move with the pin (delete its "
                "bitbank2/fastepd entry or the file, build again, commit both)")
    return None


def check(commands, fix, head, mode, glue=None):
    name = NAMES[mode]
    roots = set()
    for entry in commands:
        parts = Path(entry.get("file", "")).parts
        if COMPONENT in parts:
            roots.add(Path(*parts[: parts.index(COMPONENT) + 1]))
    if not roots:
        return f"{name} but the build compiles no FastEPD" if mode else None
    if len(roots) != 1:
        return f"FastEPD compiled from more than one place: {sorted(map(str, roots))}"
    root = roots.pop()
    inl = root / "src" / "FastEPD.inl"
    try:
        text = inl.read_text(errors="replace")
    except OSError as e:
        return f"cannot read {inl}: {e}"
    if "managed_components" not in root.parts:
        return f"FastEPD is compiled from {root}, not from managed_components (the pin)"
    err = check_no_erase(root / "src")
    if err:
        return err
    want_fix, want_head = MODES[mode]
    hint = "components/epd/idf_component.yml and the lock file (PLAN.md)"
    if (fix in text) != want_fix:
        if want_fix:
            return (f"{inl} lacks the row range fix ({fix!r}): the pin must be the fork commit, or upstream once it "
                    f"has the fix; see {hint}")
        return f"{name} but {inl} has the row range fix ({fix!r})"
    if (head in text) != want_head:
        if want_head:
            return f"{inl} is not upstream head ({head!r} missing): an older library was built; see {hint}"
        return f"{name} but {inl} is upstream head ({head!r}): the pin was not swapped"
    if glue:
        return check_mirror_source(text, glue)
    return None


def main(argv):
    if len(argv) == 5 and argv[1] == "--pin":
        err = check_pin(argv[2], argv[3], argv[4])
        if err:
            print(f"check_fastepd_source: {err}", file=sys.stderr)
        return 1 if err else 0
    if len(argv) not in (5, 7, 8, 9) or argv[4] not in MODES or (len(argv) == 9 and argv[8] not in EDGES):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        commands = json.loads(Path(argv[1]).read_text())
    except (OSError, ValueError) as e:
        print(f"check_fastepd_source: cannot read {argv[1]}: {e}", file=sys.stderr)
        return 1
    err = check(commands, argv[2], argv[3], argv[4], argv[7] if len(argv) >= 8 else None)
    if not err and len(argv) == 9:
        err = check_edge(commands, argv[8])
    uses = any(COMPONENT in Path(e.get("file", "")).parts for e in commands)
    if not err and uses and len(argv) >= 7:
        err = check_lock(argv[5], argv[6])
    if err:
        print(f"check_fastepd_source: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
