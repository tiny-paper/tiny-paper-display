#!/usr/bin/env python3
"""Fail when a board's app binary leaves less than its headroom in the app slot.
check_slot.py <board> <app.bin> [--boards-dir DIR]; headroom: slot_headroom_bytes in boards/<board>/caps.json (gen_caps.py),
slot: app0 in partitions.csv."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def slot_size(csv_path):
    for line in open(csv_path):
        f = [x.strip() for x in line.split(",")]
        if len(f) >= 5 and f[0] == "app0":
            return int(f[4], 0)
    raise ValueError("no app0 row in " + csv_path)


def headroom(path, board):
    try:
        return int(json.load(open(path))["slot_headroom_bytes"])
    except KeyError:
        raise ValueError("no slot_headroom_bytes for board %s in %s" % (board, path))


def check(board, size, boards_dir):
    slot = slot_size(os.path.join(boards_dir, board, "partitions.csv"))
    need = headroom(os.path.join(boards_dir, board, "caps.json"), board)
    free = slot - size
    return free >= need, "%s app %d B, slot %d B, free %d B, headroom %d B" % (board, size, slot, free, need)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("app")
    ap.add_argument("--boards-dir", default=os.path.join(HERE, "..", "boards"))
    a = ap.parse_args(argv)
    try:
        ok, msg = check(a.board, os.path.getsize(a.app), a.boards_dir)
    except (OSError, ValueError) as e:
        print("check_slot: " + str(e), file=sys.stderr)
        return 2
    print(("ok: " if ok else "FAIL: app slot headroom: ") + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
