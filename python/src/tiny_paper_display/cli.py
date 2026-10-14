# SPDX-License-Identifier: MIT
"""Show pictures or video frames on a tiny-paper display over USB or Bluetooth; set Bluetooth up over the cable."""
import argparse
import sys

from . import cli_show, movie, setup_bt


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    parser = argparse.ArgumentParser(prog="tiny-paper-display", description=__doc__)
    parser.add_argument("command", choices=("show", "clear", "movie", "setup"), help="operation to perform")
    args = parser.parse_args(argv[:1])
    saved = sys.argv
    try:
        sys.argv = [f"tiny-paper-display {args.command}", *argv[1:]]
        if args.command == "clear":
            sys.argv.append("--clear")
        return {"movie": movie.main, "setup": setup_bt.main}.get(args.command, cli_show.main)()
    finally:
        sys.argv = saved
