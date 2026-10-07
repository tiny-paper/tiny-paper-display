#!/usr/bin/env python3
"""After a build failed with a managed component hash mismatch, say which component and both hashes (issue #109).

Usage: explain_hash_mismatch.py <idf log>. Reads the folder and the expected hash from the cmake error, computes the
hash of what was fetched with the component manager's own hash_dir (only inside the ESP-IDF python env). Prints
nothing and returns 0 when the log has no such error.
"""
import re
import sys
from pathlib import Path

ERR = re.compile(r'Hash of the component in\s+"([^"]+)"\s+does not match expected hash\s+"([0-9a-f]+)"')


def actual_hash(folder):
    try:
        from idf_component_tools.hash_tools.calculate import hash_dir
    except ImportError:
        return "unknown (idf_component_tools not importable here)"
    return hash_dir(folder, use_gitignore=False)


def explain(text, hasher=actual_hash):
    m = ERR.search(text)
    if not m:
        return ""
    folder, expected = m[1], m[2]
    name = Path(folder).name.replace("__", "/", 1)
    return (f"component {name} fetched into {folder} does not match its dependencies.lock hash\n"
            f"  locked  {expected}\n  fetched {hasher(folder)}\n"
            "  the source changed upstream or its fetch is not reproducible (submodules, .git files); pin a full commit in\n"
            "  idf_component.yml, delete the lock entry, build once to regenerate it deliberately, and review the diff (issue #109)")


def main(argv):
    msg = explain(Path(argv[1]).read_text(errors="replace"))
    if msg:
        print("explain_hash_mismatch: " + msg, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
