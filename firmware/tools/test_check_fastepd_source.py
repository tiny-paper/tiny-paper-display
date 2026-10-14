"""Guard of the guard: the build fails when the panel library compiled is not the one meant."""
import json
import subprocess
import sys
from pathlib import Path

TOOL = Path(__file__).with_name("check_fastepd_source.py")
FW = Path(__file__).resolve().parents[1]
REAL_PIN = FW / "components" / "epd" / "idf_component.yml"
REAL_GLUE = FW / "components" / "epd" / "esp" / "epd_fastepd.cpp"
REAL_DOC = FW.parent / "PLAN.md"
MIRROR_FIX = "native_width - 1 - *iEndCol"  # the fork's cb95742 in bbepFixRect
SHA_MIRROR = "52824f76db3ca8064013e5bdd0eb05913ecb6063"  # fork branch all-fixes-with-mirror-head
RULE = "the pin and MIRROR_X move in one commit"
MARK = "iSkipped < 2"
HEAD = "1872, 1404, 26666666"
SHA_A = "eb10f7edd5f097688eb96b9f2d3c6a36a21ca5bd"
SHA_B = "7b9840459c08551beb0eb26dab6c8b91b5e63681"


def lib(root, fixed, head=True, mirror_fix=False, erase=""):
    src = root / "bitbank2__fastepd" / "src"
    src.mkdir(parents=True)
    (src / "FastEPD.inl").write_text(
        f"{{{HEAD if head else '1872, 1404, 20000000'}, 0}},\nif ({MARK if fixed else 'iSkipped == 0'}) {{\n"
        + (f"*iStartCol = pState->{MIRROR_FIX};\n" if mirror_fix else ""))
    (src / "FastEPD.cpp").write_text("")
    if erase:  # the withdrawn erase passes, in the file named
        with (src / erase).open("a") as f:
            f.write("void bbepSetErasePasses(FASTEPDSTATE *pState, uint8_t u8Extra);\n" if erase.endswith(".inl")
                    else "    void setErasePasses(uint8_t u8Extra);\n")
    return src / "FastEPD.cpp"


def run(tmp_path, files, mode="", extra=()):
    cc = tmp_path / "compile_commands.json"
    cc.write_text(json.dumps([{"file": str(f)} for f in files]))
    args = [sys.executable, str(TOOL), str(cc), MARK, HEAD, mode, *map(str, extra)]
    r = subprocess.run(args, capture_output=True, text=True)
    return r.returncode, r.stderr


def pin_and_lock(tmp_path, pin_sha, lock_sha):
    pin = tmp_path / "idf_component.yml"
    pin.write_text(f"dependencies:\n  bitbank2/fastepd:\n    git: https://x/y.git\n    version: {pin_sha}\n")
    lock = tmp_path / "dependencies.lock.esp32s3"
    lock.write_text("dependencies:\n  bitbank2/fastepd:\n    component_hash: 00\n    dependencies: []\n    source:\n"
                    f"      git: https://x/y.git\n      type: git\n    version: {lock_sha}\n  idf:\n    source:\n"
                    "      type: idf\n    version: 5.5.1\n")
    return pin, lock


def test_fork_from_managed_components_passes(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True)
    assert run(tmp_path, [f, tmp_path / "main.c"]) == (0, "")


def test_pin_back_on_upstream_fails(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", False)
    rc, err = run(tmp_path, [f])
    assert rc == 1 and "lacks the row range fix" in err


def test_older_library_under_the_pin_fails(tmp_path):
    # seen 2026-10-08: the pin moved to upstream head, the lock kept the old commit, the old library was built
    f = lib(tmp_path / "app" / "managed_components", True, head=False)
    rc, err = run(tmp_path, [f])
    assert rc == 1 and "not upstream head" in err


def test_library_with_the_erase_passes_fails(tmp_path):
    # issue #268: withdrawn 2026-10-10; seen failing on a real build against the pin before (8e5ac4c)
    for i, (name, word) in enumerate((("FastEPD.inl", "bbepSetErasePasses"), ("FastEPD.h", "setErasePasses"),
                                      ("FastEPD.cpp", "setErasePasses"))):
        f = lib(tmp_path / str(i) / "managed_components", True, erase=name)
        rc, err = run(tmp_path, [f])
        assert rc == 1 and f"{name} holds {word}" in err and "#268" in err
    f = lib(tmp_path / "old" / "managed_components", True, head=False, erase="FastEPD.h")
    assert run(tmp_path, [f], "old")[0] == 1  # the other builds too: no build links them


def test_library_from_elsewhere_fails_without_the_flag(tmp_path):
    f = lib(tmp_path / "elsewhere", True)
    rc, err = run(tmp_path, [f])
    assert rc == 1 and "not from managed_components" in err


def test_board_without_the_library_passes(tmp_path):
    assert run(tmp_path, [tmp_path / "main.c"])[0] == 0
    pin, lock = pin_and_lock(tmp_path, SHA_A, SHA_B)  # no library compiled: the lock is not this build's concern
    assert run(tmp_path, [tmp_path / "main.c"], "", (pin, tmp_path / "none"))[0] == 0


def test_upstream_build(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", False, head=False)
    assert run(tmp_path, [f], "upstream")[0] == 0
    assert run(tmp_path, [tmp_path / "main.c"], "upstream")[0] == 1  # no library compiled at all
    assert run(tmp_path, [f], "something")[0] == 2


def test_upstream_build_that_still_has_the_fix_fails(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True, head=False)  # the pin was not swapped
    rc, err = run(tmp_path, [f], "upstream")
    assert rc == 1 and "has the row range fix" in err


def test_old_pin_build(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True, head=False)
    assert run(tmp_path, [f], "old") == (0, "")
    assert run(tmp_path, [tmp_path / "main.c"], "old")[0] == 1


def test_old_pin_build_that_got_head_fails(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True, head=True)  # the pin or the lock was not swapped
    rc, err = run(tmp_path, [f], "old")
    assert rc == 1 and "is upstream head" in err


def test_old_pin_build_without_the_fix_fails(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", False, head=False)
    rc, err = run(tmp_path, [f], "old")
    assert rc == 1 and "lacks the row range fix" in err


def test_lock_must_name_the_pinned_commit(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True)
    pin, lock = pin_and_lock(tmp_path, SHA_A, SHA_A)
    assert run(tmp_path, [f], "", (pin, lock)) == (0, "")
    pin, lock = pin_and_lock(tmp_path, SHA_A, SHA_B)
    rc, err = run(tmp_path, [f], "", (pin, lock))
    assert rc == 1 and "the lock must move with the pin" in err and SHA_B in err
    rc, err = run(tmp_path, [f], "", (pin, tmp_path / "none"))
    assert rc == 1 and "cannot read" in err


def test_two_copies_fail(tmp_path):
    a = lib(tmp_path / "app" / "managed_components", True)
    b = lib(tmp_path / "elsewhere", True)
    assert run(tmp_path, [a, b])[0] == 1


def test_missing_compile_commands_fails(tmp_path):
    r = subprocess.run([sys.executable, str(TOOL), str(tmp_path / "none.json"), MARK, HEAD, ""],
                       capture_output=True, text=True)
    assert r.returncode == 1


# ---- the mirrored rectangle: the pin and the glue's MIRROR_X move together

def glue(tmp_path, mirror, rule=True):
    g = tmp_path / "epd_fastepd.cpp"
    g.write_text(f"/* {'Rule: ' + RULE if rule else 'no rule'}. */\n"
                 f"static const bool MIRROR_X = {'true' if mirror else 'false'};\n")
    return g


def doc(tmp_path, rule=True):
    d = tmp_path / "x-panel-plain.md"
    d.write_text(f"| Mirrored rectangle | {RULE if rule else 'nothing'} |\n")
    return d


def run_pin(*args):
    r = subprocess.run([sys.executable, str(TOOL), "--pin", *map(str, args)], capture_output=True, text=True)
    return r.returncode, r.stderr


def build_with_glue(tmp_path, mirror_fix, mirror):
    f = lib(tmp_path / "app" / "managed_components", True, mirror_fix=mirror_fix)
    pin, lock = pin_and_lock(tmp_path, SHA_A, SHA_A)
    return run(tmp_path, [f], "", (pin, lock, glue(tmp_path, mirror)))


def test_library_with_the_mirror_fix_needs_the_glue_mirror_off(tmp_path):
    rc, err = build_with_glue(tmp_path, mirror_fix=True, mirror=True)
    assert rc == 1 and "mirrored rectangle fix" in err and "MIRROR_X false" in err


def test_library_with_the_mirror_fix_and_the_mirror_off_passes(tmp_path):
    assert build_with_glue(tmp_path, mirror_fix=True, mirror=False) == (0, "")


def test_library_without_the_mirror_fix_needs_the_glue_mirror(tmp_path):
    assert build_with_glue(tmp_path, mirror_fix=False, mirror=True) == (0, "")
    (tmp_path / "b").mkdir()
    rc, err = build_with_glue(tmp_path / "b", mirror_fix=False, mirror=False)
    assert rc == 1 and "lacks the mirrored rectangle fix" in err


def test_glue_without_the_constant_fails_the_build_check(tmp_path):
    f = lib(tmp_path / "app" / "managed_components", True)
    pin, lock = pin_and_lock(tmp_path, SHA_A, SHA_A)
    g = tmp_path / "epd_fastepd.cpp"
    g.write_text("static const bool FLIP = true;\n")
    rc, err = run(tmp_path, [f], "", (pin, lock, g))
    assert rc == 1 and "MIRROR_X" in err
    rc, err = run(tmp_path, [f], "", (pin, lock, tmp_path / "none.cpp"))
    assert rc == 1 and "cannot read the glue" in err


def test_pin_on_a_commit_with_the_mirror_fix_is_refused_while_the_glue_mirrors(tmp_path):
    pin, _ = pin_and_lock(tmp_path, SHA_MIRROR, SHA_MIRROR)
    rc, err = run_pin(pin, glue(tmp_path, True), doc(tmp_path))
    assert rc == 1 and "cb95742" in err and "all-fixes-with-mirror-head" in err
    assert run_pin(pin, glue(tmp_path, False), doc(tmp_path)) == (0, "")
    pin, _ = pin_and_lock(tmp_path, "cb957424ff439fc3ef6d9faae7e6599de197c3ed", SHA_A)
    assert run_pin(pin, glue(tmp_path, True), doc(tmp_path))[0] == 1


def test_pin_must_be_a_full_commit(tmp_path):
    for bad in ("all-fixes-edge-head", "9dd1c54", "main"):
        pin, _ = pin_and_lock(tmp_path, bad, SHA_A)
        rc, err = run_pin(pin, glue(tmp_path, True), doc(tmp_path))
        assert rc == 1 and "full commit" in err


def test_the_rule_must_stay_written_in_the_glue_and_the_doc(tmp_path):
    pin, _ = pin_and_lock(tmp_path, SHA_A, SHA_A)
    assert run_pin(pin, glue(tmp_path, True), doc(tmp_path)) == (0, "")
    rc, err = run_pin(pin, glue(tmp_path, True, rule=False), doc(tmp_path))
    assert rc == 1 and "epd_fastepd.cpp no longer says" in err
    rc, err = run_pin(pin, glue(tmp_path, True), doc(tmp_path, rule=False))
    assert rc == 1 and "x-panel-plain.md no longer says" in err
    assert run_pin(pin, tmp_path / "none.cpp", doc(tmp_path))[0] == 1


def test_the_repository_pin_glue_and_doc_pass():
    # the real files: the pin is a full commit without the mirror fix, the glue mirrors, both name the rule
    assert run_pin(REAL_PIN, REAL_GLUE, REAL_DOC) == (0, "")
    assert SHA_A in REAL_PIN.read_text()  # the fork's all-fixes-edge-head; a pin move updates SHA_A here on purpose


def test_every_lock_file_moves_with_the_pin_file(tmp_path):
    # seen 2026-10-09: the esp32s3 locks moved with the pin, the esp32c3 locks kept the old manifest hash
    epd = tmp_path / "fw" / "components" / "epd"
    epd.mkdir(parents=True)
    pin, _ = pin_and_lock(epd, SHA_A, SHA_A)
    for proj, chip, h in (("app", "esp32s3", "aa"), ("app", "esp32c3", "aa"), ("display", "esp32s3", "aa"),
                          ("display", "esp32c3", "bb")):
        (tmp_path / "fw" / proj).mkdir(exist_ok=True)
        (tmp_path / "fw" / proj / f"dependencies.lock.{chip}").write_text(f"dependencies: {{}}\nmanifest_hash: {h}\n")
    rc, err = run_pin(pin, glue(tmp_path, True), doc(tmp_path))
    assert rc == 1 and "different manifest hashes" in err and "display/dependencies.lock.esp32c3" in err
    (tmp_path / "fw" / "display" / "dependencies.lock.esp32c3").write_text("dependencies: {}\nmanifest_hash: aa\n")
    assert run_pin(pin, glue(tmp_path, True), doc(tmp_path)) == (0, "")


EDGE_DEFS = {"off": ((), ()), "lead": (("FASTEPD_X_LEAD_WORD",), ("EPD_X_EDGE_LEAD",)),
             "dc": (("FASTEPD_X_SPH_FROM_DC",), ("EPD_X_EDGE_DC",)),
             "both": (("FASTEPD_X_LEAD_WORD", "FASTEPD_X_SPH_FROM_DC"), ("EPD_X_EDGE_LEAD", "EPD_X_EDGE_DC"))}


def run_edge(tmp_path, edge, lib_defs, glue_defs, other_defs=()):
    """A build whose library, glue and one other file were compiled with these defines, checked as X_EDGE=<edge>."""
    root = tmp_path / edge / "-".join(lib_defs + glue_defs + tuple(other_defs))
    f = lib(root / "app" / "managed_components", True)
    g = glue(root, True)
    pin, lock = pin_and_lock(root, SHA_A, SHA_A)

    def cmd(defs):
        return "g++ -DESP_PLATFORM " + " ".join(f"-D{d}=1" for d in defs) + " -c x"
    cc = root / "compile_commands.json"
    cc.write_text(json.dumps([{"file": str(f), "command": cmd(lib_defs)}, {"file": str(g), "command": cmd(glue_defs)},
                              {"file": str(root / "main.c"), "command": cmd(other_defs)}]))
    r = subprocess.run([sys.executable, str(TOOL), str(cc), MARK, HEAD, "", str(pin), str(lock), str(g), edge],
                       capture_output=True, text=True)
    return r.returncode, r.stderr


def test_each_edge_variant_passes_with_its_own_defines(tmp_path):
    for edge, (lib_defs, glue_defs) in EDGE_DEFS.items():
        assert run_edge(tmp_path, edge, lib_defs, glue_defs) == (0, ""), edge


def test_a_build_with_another_variants_defines_fails(tmp_path):
    # the way it breaks: a build directory configured for one variant, built again as another (a release of "off"
    # that still drives the lead word, or a "lead" build that is the released code)
    for edge in EDGE_DEFS:
        for other, (lib_defs, glue_defs) in EDGE_DEFS.items():
            if other != edge:
                rc, err = run_edge(tmp_path, edge, lib_defs, glue_defs)
                assert rc == 1 and f"X_EDGE={edge} but" in err, (edge, other)


def test_the_log_tag_and_the_library_must_agree(tmp_path):
    rc, err = run_edge(tmp_path, "lead", ("FASTEPD_X_LEAD_WORD",), ())  # the library tries, the log says edge=off
    assert rc == 1 and "epd_fastepd.cpp" in err
    rc, err = run_edge(tmp_path, "off", (), ("EPD_X_EDGE_DC",))
    assert rc == 1 and "epd_fastepd.cpp" in err


def test_edge_defines_on_other_files_fail(tmp_path):
    rc, err = run_edge(tmp_path, "lead", ("FASTEPD_X_LEAD_WORD",), ("EPD_X_EDGE_LEAD",), ("FASTEPD_X_LEAD_WORD",))
    assert rc == 1 and "panel library only" in err


def test_an_unknown_edge_is_a_usage_error(tmp_path):
    assert run_edge(tmp_path, "fast", (), ())[0] == 2
