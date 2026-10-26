#!/usr/bin/env python3
"""Counts the instructions in a PE file that an AMD Jaguar (Xbox One, One S; GCC btver2) cannot run.

Jaguar has SSE4.2, AVX, BMI1, F16C and MOVBE. It lacks BMI2, FMA3, FMA4, AVX2 and AVX-512, and the
first such instruction it executes stops the app with STATUS_ILLEGAL_INSTRUCTION. This script
disassembles the file, classifies every instruction by mnemonic and operands, and prints the counts per
class. With dumpbin and the build's PDB beside the file, each offender is attributed to the function it
sits in, so a count can be checked against the code that decides at run time whether to call it.

    isa-gate.py --tool dumpbin <file> [--allow REGEX ...]      on the build machine (symbols)
    isa-gate.py --tool objdump <file> --report-only            anywhere (counts only)

--allow names functions, by a regex on their (decorated) name, whose offenders are known to run only
behind a CPU-feature check. Everything else counts as a failure and the exit code is 1.
"""
import argparse
import collections
import re
import subprocess
import sys

BMI2 = {"shlx", "shrx", "sarx", "rorx", "pdep", "pext", "mulx", "bzhi"}
FMA3 = re.compile(r"^vfn?m(add|sub)(add|sub)?(132|213|231)(ps|pd|ss|sd|ph|sh)$")
FMA4 = re.compile(r"^vfn?m(add|sub)(add|sub)?(ps|pd|ss|sd)$")
AVX2_ONLY = {
    "vpbroadcastb", "vpbroadcastw", "vpbroadcastd", "vpbroadcastq", "vbroadcasti128",
    "vpermd", "vpermq", "vpermps", "vpermpd", "vperm2i128", "vinserti128", "vextracti128", "vpblendd",
    "vpgatherdd", "vpgatherdq", "vpgatherqd", "vpgatherqq", "vgatherdps", "vgatherdpd", "vgatherqps", "vgatherqpd",
    "vpmaskmovd", "vpmaskmovq", "vpsllvd", "vpsllvq", "vpsrlvd", "vpsrlvq", "vpsravd",
}
# AVX1 has vbroadcastss/sd only from memory; the register-source form is AVX2.
AVX2_REG_BROADCAST = {"vbroadcastss", "vbroadcastsd"}
AVX512_HINT = re.compile(r"\bzmm\d|\{k[0-7]\}|\{z\}|\{1to\d+\}|\b(xmm|ymm)(1[6-9]|2\d|3[01])\b")

# dumpbin /DISASM:NOBYTES:  "  0000000140001000: sub         rsp,28h"   labels: "?name@@YAXXZ:" at column 0
DUMPBIN_INSN = re.compile(r"^\s+([0-9A-Fa-f]{8,16}):\s+(\S+)\s*(.*)$")
DUMPBIN_LABEL = re.compile(r"^(\S.*):$")
# llvm-objdump -d --no-show-raw-insn --x86-asm-syntax=intel:  "140001000:     	sub	rsp, 0x28"   labels: "0000000140001000 <name>:"
OBJDUMP_INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
OBJDUMP_LABEL = re.compile(r"^[0-9a-f]+ <(.+)>:$")


def classify(mnemonic, operands):
    m = mnemonic.lower()
    ops = operands.lower()
    if m in BMI2:
        return "BMI2"
    if FMA3.match(m):
        return "FMA3"
    if FMA4.match(m):
        return "FMA4"
    if AVX512_HINT.search(ops) or m.startswith("vpternlog") or m.startswith("kmov") or m.startswith("vpcompress") or m.startswith("vpexpand"):
        return "AVX512"
    if m in AVX2_ONLY:
        return "AVX2"
    if m in AVX2_REG_BROADCAST and re.match(r"^[xy]mm\d+\s*,\s*xmm\d+", ops):
        return "AVX2"
    if m.startswith("vp") and "ymm" in ops:
        return "AVX2"
    return None


def disassemble(tool, path):
    if tool == "dumpbin":
        cmd = ["dumpbin", "/NOLOGO", "/DISASM:NOBYTES", path]
        insn, label = DUMPBIN_INSN, DUMPBIN_LABEL
    else:
        cmd = [tool, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel", path]
        insn, label = OBJDUMP_INSN, OBJDUMP_LABEL
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    function = "?"
    for line in proc.stdout:
        line = line.rstrip("\n")
        hit = insn.match(line)
        if hit:
            yield function, hit.group(1), hit.group(2), hit.group(3)
            continue
        hit = label.match(line)
        if hit:
            function = hit.group(1)
    err = proc.stderr.read()
    if proc.wait() != 0:
        sys.exit(f"{cmd[0]} failed ({proc.returncode}): {err.strip()}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--tool", default="dumpbin", help="dumpbin, or the path of an LLVM or GNU objdump")
    ap.add_argument("--allow", action="append", default=[], metavar="REGEX",
                    help="function names whose offenders are reached only behind a CPU-feature check")
    ap.add_argument("--report-only", action="store_true", help="print the counts and exit 0 whatever they are")
    ap.add_argument("--list", type=int, default=30, help="how many offending functions to list")
    ap.add_argument("--verbose", action="store_true", help="print every offender with its address")
    args = ap.parse_args()
    allow = [re.compile(a) for a in args.allow]

    total = 0
    counts = collections.Counter()
    per_function = collections.defaultdict(collections.Counter)
    examples = {}
    for function, address, mnemonic, operands in disassemble(args.tool, args.file):
        total += 1
        kind = classify(mnemonic, operands)
        if kind:
            if args.verbose:
                print(f"  {kind:6} {address} {mnemonic} {operands}   in {function}")
            counts[kind] += 1
            per_function[function][kind] += 1
            examples.setdefault((function, kind), f"{mnemonic} {operands}".strip())

    allowed = 0
    failing = 0
    print(f"file {args.file}")
    print(f"instructions {total}")
    for kind in ("BMI2", "FMA3", "FMA4", "AVX2", "AVX512"):
        print(f"{kind} {counts[kind]}")
    rows = sorted(per_function.items(), key=lambda kv: -sum(kv[1].values()))
    for function, kinds in rows:
        n = sum(kinds.values())
        ok = any(a.search(function) for a in allow)
        if ok:
            allowed += n
        else:
            failing += n
    print(f"allowed {allowed}")
    print(f"failing {failing}")
    if rows:
        print(f"offending functions ({len(rows)}), most first:")
        for function, kinds in rows[: args.list]:
            ok = any(a.search(function) for a in allow)
            detail = " ".join(f"{k}={v}" for k, v in sorted(kinds.items()))
            sample = next(v for (f, k), v in examples.items() if f == function)
            print(f"  {'allowed' if ok else 'FAIL':7} {detail:28} {function}   e.g. {sample}")
    if args.report_only:
        return 0
    return 1 if failing else 0


if __name__ == "__main__":
    sys.exit(main())
