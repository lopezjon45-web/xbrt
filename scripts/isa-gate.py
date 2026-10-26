#!/usr/bin/env python3
"""Counts the instructions in an x64 PE file that an AMD Jaguar (Xbox One, One S; GCC btver2) cannot run.

Jaguar has SSE4.2, AVX, BMI1, F16C and MOVBE. It lacks BMI2, FMA3, FMA4, AVX2 and AVX-512, and the
first such instruction it executes stops the app with STATUS_ILLEGAL_INSTRUCTION. This script
decodes the file's code sections, classifies every instruction by mnemonic and operands, and prints the counts per
class. With dumpbin and the build's PDB beside the file, each offender is attributed to the function it
sits in, so a count can be checked against the code that decides at run time whether to call it.

    isa-gate.py --tool dumpbin <file> [--allow REGEX ...]      on the build machine (symbols)
    isa-gate.py --tool objdump <file> --report-only            anywhere (export names only)
    isa-gate.py --tool dumpbin <file> --guards REGEX            the evidence for an --allow

Code and data are told apart from the file's own structure. Decoding starts at every .pdata function, the entry
point and the exports and follows direct branches. An MSVC switch (a bound check, then a jump through a table of
RVAs that MSVC places in .text after the function) is resolved, its table bytes are data and its case targets are
followed. Every byte that is neither traced code nor such a table is decoded in a linear sweep and counts like code,
so the gate fails closed: only a table proven from the instructions that read it is left out. The offenders a linear
sweep would have read in those tables are listed, so each exclusion can be checked.

--tool only names the functions (dumpbin's labels come from the PDB; objdump's from the exports); decoding is
Capstone's (scripts/isa-gate-requirements.txt). --allow names functions, by a regex on their (decorated) name,
whose offenders are known to run only behind a CPU-feature check. Everything else counts as a failure and the exit
code is 1. --guards prints, for each offending function the regex names, every call or jump to it with the four
instructions before, so the check that guards it can be read in the log.
"""
import argparse
import bisect
import collections
import re
import struct
import subprocess
import sys

try:
    import capstone
    from capstone import x86
except ImportError:
    sys.exit("isa-gate.py needs Capstone: python -m pip install --require-hashes -r scripts/isa-gate-requirements.txt")

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
DUMPBIN_INSN = re.compile(r"^\s+([0-9A-Fa-f]{8,16}):\s+\S")
DUMPBIN_LABEL = re.compile(r"^(\S.*):$")
# llvm-objdump -d:  labels "0000000140001000 <name>:"
OBJDUMP_LABEL = re.compile(r"^([0-9a-f]+) <(.+)>:$")

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_SCN_MEM_EXECUTE = 0x20000000
# A chained .pdata entry continues a function's unwind state over a later range; MSVC gives one to the bytes after an
# epilog, and that range can start with the function's switch table, so only primary entries are taken as code.
UNW_FLAG_CHAININFO = 0x4
STOPS = {"int3", "ud2", "hlt"}


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


Section = collections.namedtuple("Section", "name va data executable")
Table = collections.namedtuple("Table", "va size jump")


class Image:
    """The parts of a PE32+ file the gate reads: code sections, entry point, exports and .pdata function starts."""

    def __init__(self, path):
        raw = open(path, "rb").read()
        pe = struct.unpack_from("<I", raw, 0x3C)[0]
        if raw[pe:pe + 4] != b"PE\0\0":
            sys.exit(f"{path}: not a PE file")
        machine, nsections = struct.unpack_from("<HH", raw, pe + 4)
        opt_size = struct.unpack_from("<H", raw, pe + 20)[0]
        opt = pe + 24
        if machine != IMAGE_FILE_MACHINE_AMD64 or struct.unpack_from("<H", raw, opt)[0] != 0x20B:
            sys.exit(f"{path}: not an x64 PE file")
        entry = struct.unpack_from("<I", raw, opt + 16)[0]
        self.base = struct.unpack_from("<Q", raw, opt + 24)[0]
        ndirs = struct.unpack_from("<I", raw, opt + 108)[0]
        dirs = [struct.unpack_from("<II", raw, opt + 112 + 8 * i) for i in range(min(ndirs, 16))]
        self.sections = []
        for i in range(nsections):
            at = pe + 24 + opt_size + 40 * i
            vsize, rva, rsize, rptr = struct.unpack_from("<IIII", raw, at + 8)
            flags = struct.unpack_from("<I", raw, at + 36)[0]
            self.sections.append(Section(raw[at:at + 8].rstrip(b"\0").decode(errors="replace"), self.base + rva,
                                         raw[rptr:rptr + min(vsize or rsize, rsize)], bool(flags & IMAGE_SCN_MEM_EXECUTE)))
        self.code_sections = [s for s in self.sections if s.executable]
        self.exports = {}
        self.functions = []
        if len(dirs) > 0 and dirs[0][0]:
            self._read_exports(*dirs[0])
        if len(dirs) > 3 and dirs[3][0]:
            pdata = self.read(self.base + dirs[3][0], dirs[3][1])
            for i in range(0, len(pdata) - 11, 12):
                begin, _, unwind = struct.unpack_from("<III", pdata, i)
                if not (self.read(self.base + unwind, 1) or b"\0")[0] >> 3 & UNW_FLAG_CHAININFO:
                    self.functions.append(self.base + begin)
        self.entry = self.base + entry if entry else None

    def _read_exports(self, rva, size):
        d = self.read(self.base + rva, 40)
        _, _, _, _, _, _, nfuncs, nnames, funcs, names, ords = struct.unpack("<IIHHIIIIIII", d)
        addrs = struct.unpack(f"<{nfuncs}I", self.read(self.base + funcs, 4 * nfuncs))
        name_rvas = struct.unpack(f"<{nnames}I", self.read(self.base + names, 4 * nnames))
        ordinals = struct.unpack(f"<{nnames}H", self.read(self.base + ords, 2 * nnames))
        for name_rva, ordinal in zip(name_rvas, ordinals):
            target = addrs[ordinal]
            if not rva <= target < rva + size:
                name = self.read(self.base + name_rva, 256).split(b"\0")[0].decode(errors="replace")
                self.exports[self.base + target] = name

    def section(self, va):
        for s in self.sections:
            if s.va <= va < s.va + len(s.data):
                return s
        return None

    def read(self, va, n):
        s = self.section(va)
        return s.data[va - s.va: va - s.va + n] if s else b""

    def in_code(self, va, n=1):
        s = self.section(va)
        return s is not None and s.executable and va + n <= s.va + len(s.data)


PARENT = {}
for _r in ("a", "b", "c", "d"):
    PARENT.update({f"r{_r}x": f"r{_r}x", f"e{_r}x": f"r{_r}x", f"{_r}x": f"r{_r}x", f"{_r}l": f"r{_r}x", f"{_r}h": f"r{_r}x"})
for _r in ("si", "di", "bp", "sp"):
    PARENT.update({f"r{_r}": f"r{_r}", f"e{_r}": f"r{_r}", _r: f"r{_r}", f"{_r}l": f"r{_r}"})
for _n in range(8, 16):
    PARENT.update({f"r{_n}{s}": f"r{_n}" for s in ("", "d", "w", "b")})


def reg(insn, r):
    name = insn.reg_name(r)
    return PARENT.get(name, name)


def written(insn):
    return {reg(insn, r) for r in insn.regs_access()[1]}


def resolve_table(image, code, jump):
    """The tables an MSVC switch jumps through and their case targets, or None when the instructions before the jump
    do not prove them.

    MSVC's x64 switch (cl /O2), with B holding __ImageBase and the index bounded by an unsigned compare:
        cmp I, n-1 ; ja default ; [movsxd I, I32 | cdqe] ; [movzx I, byte ptr [B + J + bytes]] ;
        mov R32, dword ptr [B + I*4 + table] ; add R, B ; jmp R
    with unrelated instructions in between. A table entry is the RVA of a case; the optional byte table maps the
    bounded J to the entry index.
    """
    def before(insn):
        for _ in range(24):
            insn = next((p for n in range(1, 16) if (p := code.get(insn.address - n)) is not None and p.size == n), None)
            if insn is None:
                return
            yield insn

    def is_reg(op, name, insn):
        return op.type == x86.X86_OP_REG and reg(insn, op.reg) == name

    target = reg(jump, jump.operands[0].reg)
    add = next((i for i in before(jump) if target in written(i)), None)
    if add is None or add.mnemonic != "add" or not is_reg(add.operands[0], target, add) \
            or add.operands[1].type != x86.X86_OP_REG:
        return None
    base = reg(add, add.operands[1].reg)
    load = next((i for i in before(add) if written(i) & {target, base}), None)
    if load is None or load.mnemonic not in ("mov", "movsxd") or not is_reg(load.operands[0], target, load):
        return None
    mem = load.operands[1]
    if mem.type != x86.X86_OP_MEM or mem.size != 4 or mem.mem.segment or reg(load, mem.mem.base) != base \
            or mem.mem.scale != 4 or not mem.mem.index:
        return None
    table_va = image.base + (mem.mem.disp & 0xFFFFFFFF)
    index = reg(load, mem.mem.index)
    byte_table = None
    base_moved = False

    for insn in before(load):
        ops = insn.operands
        if insn.mnemonic == "cmp" and is_reg(ops[0], index, insn) and ops[1].type == x86.X86_OP_IMM:
            follow = code.get(insn.address + insn.size)
            if follow is None or follow.mnemonic not in ("ja", "jae"):
                return None
            bound = ops[1].imm & ((1 << (8 * ops[0].size)) - 1)
            count = bound + 1 if follow.mnemonic == "ja" else bound
            break
        changed = written(insn)
        base_moved |= base in changed
        if index not in changed:
            continue
        if insn.mnemonic == "cdqe" and index == "rax":
            continue
        if insn.mnemonic in ("movsxd", "mov") and ops[1].type == x86.X86_OP_REG:
            index = reg(insn, ops[1].reg)
            continue
        if insn.mnemonic == "movzx" and byte_table is None and not base_moved and ops[1].type == x86.X86_OP_MEM \
                and ops[1].size == 1 and ops[1].mem.scale == 1 and not ops[1].mem.segment:
            pair = [reg(insn, ops[1].mem.base), reg(insn, ops[1].mem.index)]
            if base in pair and pair.count(base) == 1:
                byte_table = image.base + (ops[1].mem.disp & 0xFFFFFFFF)
                index = pair[1 - pair.index(base)]
                continue
        return None
    else:
        return None

    if not 0 < count <= 0x10000:
        return None
    tables = []
    if byte_table is not None:
        if not image.in_code(byte_table, count):
            return None
        tables.append(Table(byte_table, count, jump.address))
        count = max(image.read(byte_table, count)) + 1
    if not image.in_code(table_va, 4 * count):
        return None
    targets = [image.base + e for e in struct.unpack(f"<{count}I", image.read(table_va, 4 * count))]
    if not all(image.in_code(t) for t in targets):
        return None
    tables.append(Table(table_va, 4 * count, jump.address))
    return tables, targets


def trace(image, md):
    """Decodes from every primary function start, the exports and the entry point along direct branches, and along
    the case targets of each switch table proven once the code before its jump has been traced.

    Returns the instructions by address, the proven tables and the register jumps no table was proven for."""
    code = {}
    tables = []
    jumps = set()
    work = list(image.functions) + list(image.exports) + ([image.entry] if image.entry else [])
    while work:
        while work:
            va = work.pop()
            while va not in code and image.in_code(va):
                insn = next(md.disasm(image.read(va, 15), va, 1), None)
                if insn is None:
                    break
                code[va] = insn
                groups = set(insn.groups)
                if capstone.CS_GRP_RET in groups or insn.mnemonic in STOPS:
                    break
                if capstone.CS_GRP_JUMP in groups or capstone.CS_GRP_CALL in groups:
                    op = insn.operands[0]
                    if op.type == x86.X86_OP_IMM:
                        work.append(op.imm)
                    elif capstone.CS_GRP_JUMP in groups and op.type == x86.X86_OP_REG:
                        jumps.add(insn)
                    if insn.mnemonic == "jmp":
                        break
                va += insn.size
        for jump in sorted(jumps, key=lambda i: i.address):
            found = resolve_table(image, code, jump)
            if found:
                jumps.discard(jump)
                tables += found[0]
                work += found[1]
    return code, tables, sorted(j.address for j in jumps)


def sweep(md, va, data):
    """Linear decode of bytes nothing traced; an undecodable byte is skipped."""
    at = 0
    while at < len(data):
        n = 0
        for insn in md.disasm(data[at:], va + at):
            yield insn
            n = insn.address + insn.size - va
        at = max(n, at + 1)


def analyse(image):
    """Every instruction the gate judges (traced code, then the sweep of untraced bytes); the tables left out, those
    kept in because traced code overlaps them, and the offenders a linear sweep reads in the ones left out; the
    register jumps no table was proven for; the traced code."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    code, tables, unresolved = trace(image, md)
    owner = {s.name: bytearray(len(s.data)) for s in image.code_sections}
    for insn in code.values():
        s = image.section(insn.address)
        start = insn.address - s.va
        owner[s.name][start:start + insn.size] = b"\1" * min(insn.size, len(s.data) - start)
    kept = []
    overlapping = []
    for t in sorted(set(tables)):
        s = image.section(t.va)
        span = owner[s.name][t.va - s.va: t.va - s.va + t.size]
        if any(span):
            overlapping.append(t)
        else:
            owner[s.name][t.va - s.va: t.va - s.va + t.size] = b"\2" * t.size
            kept.append(t)

    lite = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    judged = [(i.address, i.mnemonic, i.op_str) for i in code.values()]
    for s in image.code_sections:
        own = owner[s.name]
        at = 0
        while at < len(own):
            end = at
            while end < len(own) and own[end] == own[at]:
                end += 1
            if own[at] == 0:
                judged += [(i.address, i.mnemonic, i.op_str) for i in sweep(lite, s.va + at, bytes(s.data[at:end]))]
            at = end
    hidden = []
    for t in kept:
        hidden += [(i.address, i.mnemonic, i.op_str, t) for i in sweep(lite, t.va, image.read(t.va, t.size))
                   if classify(i.mnemonic, i.op_str)]
    judged.sort()
    return judged, kept, overlapping, hidden, sorted(set(unresolved)), code


def labels(tool, path, image):
    """Function start -> name: the exports, then the labels the disassembler prints (dumpbin's come from the PDB)."""
    names = dict(image.exports)
    if tool == "none":
        return names
    if tool == "dumpbin":
        cmd = ["dumpbin", "/NOLOGO", "/DISASM:NOBYTES", path]
    else:
        cmd = [tool, "-d", "--no-show-raw-insn", path]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace")
    pending = None
    for line in proc.stdout:
        line = line.rstrip("\n")
        if tool == "dumpbin":
            hit = DUMPBIN_INSN.match(line)
            if hit:
                if pending is not None:
                    names[int(hit.group(1), 16)] = pending
                    pending = None
                continue
            hit = DUMPBIN_LABEL.match(line)
            if hit:
                pending = hit.group(1)
        else:
            hit = OBJDUMP_LABEL.match(line)
            if hit:
                names[int(hit.group(1), 16)] = hit.group(2)
    err = proc.stderr.read()
    if proc.wait() != 0:
        sys.exit(f"{cmd[0]} failed ({proc.returncode}): {err.strip()}")
    return names


class Names:
    """Names an address by the label at or before it. A primary function start with no label of its own is named
    sub_<address>, so a sparse set of labels (only the exports) never stretches over the functions after it."""

    def __init__(self, labels, image):
        named = {va: f"sub_{va:X}" for va in image.functions}
        named.update(labels)
        self.labels = labels
        self.starts = sorted(named)
        self.names = [named[a] for a in self.starts]
        self.first = {}
        for va in reversed(self.starts):
            self.first[named[va]] = va

    def __call__(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        return self.names[i] if i >= 0 else "?"


def branch_target(insn):
    if insn.mnemonic.startswith(("call", "j")) and insn.operands and insn.operands[0].type == x86.X86_OP_IMM:
        return insn.operands[0].imm
    return None


def print_guards(code, names, judged, pattern):
    offending = sorted({names(a) for a, m, o in judged if classify(m, o) and pattern.search(names(a))})
    ordered = sorted(code)
    for target in offending:
        sites = [i for i, a in enumerate(ordered)
                 if branch_target(code[a]) == names.first[target] and names(a) != target]
        print(f"{target}: {len(sites)} branches in")
        for i in sites:
            for a in ordered[max(0, i - 4): i + 1]:
                insn = code[a]
                print(f"    {names(a):24.24} {a:016X} {insn.mnemonic} {names.labels.get(branch_target(insn), insn.op_str)}")
            print()
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--tool", default="dumpbin",
                    help="where function names come from: dumpbin, the path of an LLVM objdump, or none")
    ap.add_argument("--allow", action="append", default=[], metavar="REGEX",
                    help="function names whose offenders are reached only behind a CPU-feature check")
    ap.add_argument("--report-only", action="store_true", help="print the counts and exit 0 whatever they are")
    ap.add_argument("--list", type=int, default=30, help="how many offending functions to list")
    ap.add_argument("--verbose", action="store_true", help="print every offender with its address")
    ap.add_argument("--guards", metavar="REGEX", help="print the branches into offending functions this names")
    args = ap.parse_args()
    allow = [re.compile(a) for a in args.allow]

    image = Image(args.file)
    judged, tables, overlapping, hidden, unresolved, code = analyse(image)
    names = Names(labels(args.tool, args.file, image), image)
    if args.guards:
        return print_guards(code, names, judged, re.compile(args.guards))

    counts = collections.Counter()
    per_function = collections.defaultdict(collections.Counter)
    examples = {}
    for address, mnemonic, operands_text in judged:
        kind = classify(mnemonic, operands_text)
        if kind:
            function = names(address)
            if args.verbose:
                print(f"  {kind:6} {address:X} {mnemonic} {operands_text}   in {function}")
            counts[kind] += 1
            per_function[function][kind] += 1
            examples.setdefault((function, kind), f"{mnemonic} {operands_text}".strip())

    print(f"file {args.file}")
    print(f"instructions {len(judged)}")
    for kind in ("BMI2", "FMA3", "FMA4", "AVX2", "AVX512"):
        print(f"{kind} {counts[kind]}")
    print(f"traced {len(code)}")
    print(f"tables {len(tables)}")
    print(f"table_bytes {sum(t.size for t in tables)}")
    print(f"tables_overlapping_code {len(overlapping)}")
    print(f"unresolved_jumps {len(unresolved)}")
    for address, mnemonic, operands_text, t in hidden:
        print(f"  table data, not code: {address:X} {mnemonic} {operands_text}   (table {t.va:X}+{t.size} read by the "
              f"jump at {t.jump:X} in {names(t.jump)})")
    allowed = 0
    failing = 0
    rows = sorted(per_function.items(), key=lambda kv: -sum(kv[1].values()))
    for function, kinds in rows:
        n = sum(kinds.values())
        if any(a.search(function) for a in allow):
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
