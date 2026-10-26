#!/usr/bin/env python3
"""Judges the desktop host's --run of each lab XBE against the lines the lab program must print.

    check-lab-run.py <lab dir> <run dir> [--known-failure <name>]... [--expected <file name>]
    check-lab-run.py --self-test

<lab dir> holds <name>/expected.txt per lab (the source tree's lab/, or lab/build.sh's output). <run dir> holds
<name>.txt per lab: everything `xbrt_desktop --run` wrote to stdout and stderr. A lab passes when the lines of its
output that carry "lab <name> " (DbgPrint output may arrive behind a prefix) are exactly its expected lines, in
order. On a failure the judge prints the difference, the boot stage that failed if one did, and the last 200 lines of
the output.
A --known-failure lab is judged and reported the same way but does not fail the run; one that passes does, so the
flag is removed the moment the defect it names is fixed. --expected judges the labs that have that file instead of
expected.txt (expected-second.txt: a second launch over the first one's saves).
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
    report += ["  output: " + l for l in output.splitlines()[-200:]]
    return False, report


def run(lab_dir, run_dir, known_failures=(), expected_name="expected.txt"):
    names = sorted(p.parent.name for p in pathlib.Path(lab_dir).glob(f"*/{expected_name}"))
    if not names:
        print(f"no <name>/{expected_name} under {lab_dir}")
        return 1
    unknown = sorted(set(known_failures) - set(names))
    if unknown:
        print(f"--known-failure names no lab: {', '.join(unknown)}")
        return 1
    failed = 0
    passed = 0
    for name in names:
        expected = (pathlib.Path(lab_dir) / name / expected_name).read_text().splitlines()
        out_path = pathlib.Path(run_dir) / f"{name}.txt"
        output = out_path.read_text(encoding="utf-8", errors="replace") if out_path.exists() else None
        ok, report = judge(name, expected, output)
        if name in known_failures:
            report[0] += " (known failure)" if not ok else " but it is listed as a known failure: remove --known-failure " + name
            failed += ok
        else:
            failed += not ok
        passed += ok
        print("\n".join(report))
    print(f"lab run: {passed}/{len(names)} pass, {len(known_failures)} known failure{'' if len(known_failures) == 1 else 's'}")
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
        if run(f"{d}/lab", f"{d}/run", ["demo"]) == 0:
            print("self-test FAIL: a known failure that passes is accepted")
            bad += 1
        pathlib.Path(f"{d}/run/demo.txt").write_text(passing.replace("lab demo ok\n", ""))
        if run(f"{d}/lab", f"{d}/run", ["demo"]) != 0:
            print("self-test FAIL: a known failure that fails is rejected")
            bad += 1
        if run(f"{d}/lab", f"{d}/run") == 0:
            print("self-test FAIL: the directory walk accepts a failing run")
            bad += 1
        if run(f"{d}/lab", f"{d}/run", ["nosuchlab"]) == 0:
            print("self-test FAIL: a known failure naming no lab is accepted")
            bad += 1
        pathlib.Path(f"{d}/lab/demo/expected-second.txt").write_text("\n".join(expected) + "\n")
        pathlib.Path(f"{d}/run/demo.txt").write_text(passing)
        if run(f"{d}/lab", f"{d}/run", expected_name="expected-second.txt") != 0:
            print("self-test FAIL: --expected rejects a passing run")
            bad += 1
        pathlib.Path(f"{d}/lab/demo/expected-second.txt").write_text("lab demo second\n")
        if run(f"{d}/lab", f"{d}/run", expected_name="expected-second.txt") == 0:
            print("self-test FAIL: --expected judges against expected.txt")
            bad += 1
    print(f"self-test: {len(cases) + 8 - bad}/{len(cases) + 8} pass")
    return 1 if bad else 0


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    args = sys.argv[1:]
    known = []
    expected_name = "expected.txt"
    while len(args) >= 4 and args[-2] in ("--known-failure", "--expected"):
        if args[-2] == "--expected":
            expected_name = args[-1]
        else:
            known.append(args[-1])
        args = args[:-2]
    if len(args) != 2:
        sys.exit(__doc__)
    sys.exit(run(args[0], args[1], known, expected_name))
