#!/usr/bin/env python3
"""Judges the desktop host's run.txt from the windows-2022 runner: every stage that runs on a desktop x64 Windows
must pass there, since the console runs the same compat-mode code (route 3's CI printed identical lines).

    check-desktop-run.py <run.txt>

Required: reserve ok, identity ok, each probe line ok, END. The device line is reported, not required: WARP is
not the console's driver.
"""
import re
import sys

REQUIRED = [
    r"^reserve ok\b",
    r"^identity ok\b",
    r"^probe resume ok\b",
    r"^probe selectors ok\b",
    r"^probe r15 ok\b",
    r"^probe cost ok\b",
    r"^probe fs ok\b",
    r"^END$",
]


def main(path):
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as e:
        sys.exit(f"no run.txt: {e}")
    for line in lines:
        print("  " + line)
    missing = [r for r in REQUIRED if not any(re.search(r, l) for l in lines)]
    for r in missing:
        print(f"MISSING {r}")
    if missing:
        last = next((l for l in reversed(lines) if l.startswith("BEGIN")), "(no BEGIN line)")
        print(f"last step started: {last}")
        return 1
    print("desktop run: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
