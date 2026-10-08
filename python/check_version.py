#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Refuse a release whose tag disagrees with the package version (Python 3.11+)."""
import sys
import tomllib
from pathlib import Path


def main(tag):
    with Path(__file__).with_name("pyproject.toml").open("rb") as source:
        version = tomllib.load(source)["project"]["version"]
    if tag != f"v{version}":
        print(f"version mismatch: tag {tag!r}, package {version!r}", file=sys.stderr)
        return 1
    print(f"Release version: {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
