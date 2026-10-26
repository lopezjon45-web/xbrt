#!/usr/bin/env python3
"""Judges the desktop host's cpu-tests.txt from the windows-2022 runner: the CPU core's tests run the same compat-mode
code the console runs, so every one must pass there.

    check-cpu-tests.py <cpu-tests.txt>

Required: reserve ok, identity ok, every `cpu <name> ok` line below, the counters line, END; and no `cpu ... FAIL`.
"""
import re
import sys

TESTS = [
    "emit", "gate",
    "fs-load", "fs-load-8b", "fs-store", "fs-store-a3", "fs-store-esp", "fs-store-imm", "fs-push", "fs-pop",
    "fs-movzx-byte", "fs-cmp", "fs-cmp-rev", "fs-other",
    "mmio", "port", "privileged", "rdtsc", "nested", "trap-state", "spawn-exit", "fault-gate", "memory",
]
REQUIRED = [r"^reserve ok\b", r"^identity ok\b"] + [rf"^cpu {re.escape(t)} ok\b" for t in TESTS] + [r"^cpu counters\b", r"^END$"]


def main(path):
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as e:
        sys.exit(f"no cpu-tests.txt: {e}")
    for line in lines:
        print("  " + line)
    missing = [r for r in REQUIRED if not any(re.search(r, l) for l in lines)]
    failed = [l for l in lines if re.match(r"^cpu \S+ FAIL\b", l)]
    for r in missing:
        print(f"MISSING {r}")
    for l in failed:
        print(f"FAILED {l}")
    if missing or failed:
        last = next((l for l in reversed(lines) if l.startswith("cpu ")), "(no cpu line)")
        print(f"last cpu line: {last}")
        return 1
    print("cpu tests: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
