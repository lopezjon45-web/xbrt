#!/usr/bin/env python3
"""Judges kernel-tests.txt from `xbrt_desktop.exe --kernel-tests`: every named test must report ok, none FAIL.

    check-kernel-tests.py <kernel-tests.txt>
"""
import sys

REQUIRED = ["DbgPrint", "KeQuerySystemTime", "NtCreateFile", "NtReadFile", "stub", "unbridged"]


def main(path):
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError as e:
        sys.exit(f"no kernel-tests.txt: {e}")
    for line in lines:
        print("  " + line)
    results = {}
    for line in lines:
        parts = line.split(" ", 3)
        if len(parts) >= 3 and parts[0] == "kernel":
            results[parts[1]] = parts[2]
    bad = [name for name in REQUIRED if results.get(name) != "ok"]
    bad += [name for name, verdict in results.items() if verdict != "ok" and name not in bad]
    if "END" not in lines:
        bad.append("END")
    for name in bad:
        print(f"NOT OK {name}: {results.get(name, 'missing')}")
    if bad:
        return 1
    print("kernel tests: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
