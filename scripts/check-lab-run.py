#!/usr/bin/env python3
"""Judges the desktop host's --run of each lab XBE against the lines the lab program must print.

    check-lab-run.py <lab dir> <run dir>
    check-lab-run.py --self-test

<lab dir> holds <name>/expected.txt per lab (the source tree's lab/, or lab/build.sh's output). <run dir> holds
<name>.txt per lab: everything `xbrt_desktop --run` wrote to stdout and stderr. A lab passes when the lines of its
output that carry "lab <name> " (DbgPrint output may arrive behind a prefix) are exactly its expected lines, in
order. On a failure the judge prints the difference, the boot stage that failed if one did, and the output's tail.
"""
import difflib
import os
import pathlib
import sys
import tempfile


def lab_lines(name, output):
    tag = f"lab {name} "
    return [line[line.index(tag):].rstrip() for line in output.splitlines() if tag in line]


def judge(name, expected, output):
    if output is None:
        return False, [f"lab {name}: FAIL no run output"]
    got = lab_lines(name, output)
    if got == expected:
        return True, [f"lab {name}: pass"]
    report = [f"lab {name}: FAIL"]
    report += ["  " + d for d in difflib.unified_diff(expected, got, "expected", "run", lineterm="", n=1)]
    report += ["  boot: " + l for l in output.splitlines() if " FAIL " in l and not l.lstrip().startswith("lab ")]
    report += ["  tail: " + l for l in output.splitlines()[-8:]]
    return False, report


def run(lab_dir, run_dir):
    names = sorted(p.parent.name for p in pathlib.Path(lab_dir).glob("*/expected.txt"))
    if not names:
        print(f"no <name>/expected.txt under {lab_dir}")
        return 1
    failed = 0
    for name in names:
        expected = (pathlib.Path(lab_dir) / name / "expected.txt").read_text().splitlines()
        out_path = pathlib.Path(run_dir) / f"{name}.txt"
        output = out_path.read_text(encoding="utf-8", errors="replace") if out_path.exists() else None
        ok, report = judge(name, expected, output)
        print("\n".join(report))
        failed += not ok
    print(f"lab run: {len(names) - failed}/{len(names)} pass")
    return 1 if failed else 0


def self_test():
    expected = ["lab demo start", "lab demo value 0x00000001 0x00000001", "lab demo ok"]
    passing = "reserve ok\nxbe xbrt lab: demo FFFF0002 XDK 0, 3 imports\n  [KERNEL] lab demo start\nlab demo value 0x00000001 0x00000001\nlab demo ok\n"
    cases = [
        ("accepts the expected lines among other output", passing, True),
        ("rejects a missing result line", passing.replace("lab demo ok\n", ""), False),
        ("rejects a wrong value", passing.replace("0x00000001 0x00000001", "0x00000001 0x00000002"), False),
        ("rejects a FAIL result", passing.replace("lab demo ok", "lab demo FAIL NtCreateFile 0xC0000034"), False),
        ("rejects lines out of order", "lab demo ok\nlab demo start\nlab demo value 0x00000001 0x00000001\n", False),
        ("rejects an extra lab line", passing + "lab demo ok\n", False),
        ("rejects a run that stopped at boot", "reserve ok\nstart FAIL no guest execution\n", False),
    ]
    bad = 0
    for what, output, should_pass in cases:
        ok, report = judge("demo", expected, output)
        if ok != should_pass:
            print(f"self-test FAIL: {what}: {report[0]}")
            bad += 1
    if judge("demo", expected, None)[0]:
        print("self-test FAIL: accepts a missing run file")
        bad += 1
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(f"{d}/lab/demo")
        os.makedirs(f"{d}/run")
        pathlib.Path(f"{d}/lab/demo/expected.txt").write_text("\n".join(expected) + "\n")
        pathlib.Path(f"{d}/run/demo.txt").write_text(passing)
        if run(f"{d}/lab", f"{d}/run") != 0:
            print("self-test FAIL: the directory walk rejects a passing run")
            bad += 1
    print(f"self-test: {len(cases) + 2 - bad}/{len(cases) + 2} pass")
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(run(sys.argv[1], sys.argv[2]))
