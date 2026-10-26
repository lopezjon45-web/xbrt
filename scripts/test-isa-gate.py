#!/usr/bin/env python3
"""Tests isa-gate.py on small x64 PE files it builds: MSVC switch jump tables in .text, offenders reached by a
direct path, through a table, or by nothing, and an allowed body.

    test-isa-gate.py [--objdump PATH]

Each case writes a PE, runs isa-gate.py on it with --tool <objdump> (the function names come from the PE's exports),
and checks the exit code and the counts the gate prints. The exit code is the number of failing cases.
"""
import argparse
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

GATE = pathlib.Path(__file__).with_name("isa-gate.py")
TEXT_RVA = 0x1000

# Case bodies at these RVAs make the switch table's bytes (62 61 05 00 72 61 05 00 ...) decode as
# "vpsrad xmm31, xmmword ptr [rcx + 0x50], 0", an EVEX instruction a linear sweep counts as AVX-512.
EVEX_CASES = [0x56162 + 0x10 * k for k in range(4)]

RET0 = bytes.fromhex("31c0c3")                      # xor eax, eax; ret
AVX2 = bytes.fromhex("c4e27d58c1")                  # vpbroadcastd ymm0, xmm1
AVX512 = bytes.fromhex("62f1fe486f00")              # vmovdqu64 zmm0, zmmword ptr [rax]
FMA3 = bytes.fromhex("c4e2f9a9c2")                  # vfmadd213sd xmm0, xmm0, xmm2
EVEX_XMM = bytes.fromhex("62f1fe086f00")            # vmovdqu64 xmm0, xmmword ptr [rax]: AVX-512VL, no zmm or mask
MOV_RAX_IMM64 = bytes.fromhex("48b8")               # mov rax, imm64: a linear sweep reads the next 8 bytes as its operand
AVX1_YMM = bytes.fromhex("c4e27d17c1")              # vptest ymm0, ymm1
AVX2_YMM_ONLY = bytes.fromhex("c4e27d2a00")         # vmovntdqa ymm0, ymmword ptr [rax]


class Text:
    """The .text section being written, int3-filled between the pieces placed in it."""

    def __init__(self):
        self.code = bytearray()
        self.functions = []
        self.chained = []

    def rva(self):
        return TEXT_RVA + len(self.code)

    def place(self, rva, body, function=True):
        assert rva >= self.rva(), hex(rva)
        self.code += b"\xcc" * (rva - self.rva())
        if function:
            self.functions.append((rva, rva + len(body)))
        self.code += body
        return rva

    def switch(self, rva, cases, bound="ja", byte_table=None, clobber_base=False, scale=4, table_rva=None,
               default_in_path=False):
        """MSVC's x64 switch on ecx: a bound check (bound: the jump after the cmp, or None for no check), then a jump
        through a table of case RVAs placed after the code, through a byte table of entry indexes when one is given.
        The other arguments break one fact the gate relies on: clobber_base overwrites the table base register
        between the byte table and the case table, scale is the load's index scale, table_rva places the case table
        there (already written) instead of after the code, and default_in_path points the bound's jump at the load."""
        code = bytearray()
        if bound:
            code += bytes.fromhex("83f9") + bytes([len(byte_table or cases) - 1])   # cmp ecx, n-1
            default_at = len(code)
            code += {"ja": bytes.fromhex("0f87"), "jne": bytes.fromhex("0f85")}[bound] + b"\0\0\0\0"
        code += bytes.fromhex("4863c1")                                        # movsxd rax, ecx
        code += bytes.fromhex("488d15") + struct.pack("<i", -(rva + len(code) + 7))   # lea rdx, [__ImageBase]
        if byte_table:
            bytes_at = len(code)
            code += bytes.fromhex("0fb68402") + b"\0\0\0\0"                # movzx eax, byte [rdx+rax+bytes]
        if clobber_base:
            code += bytes.fromhex("4889ca")                                    # mov rdx, rcx
        load_at = len(code)
        code += bytes.fromhex("8b8c") + bytes([{4: 0x82, 8: 0xC2}[scale]]) + b"\0\0\0\0"   # mov ecx, [rdx+rax*4+table]
        code += bytes.fromhex("4803caffe1")                                    # add rcx, rdx; jmp rcx
        default = len(code)
        code += RET0
        end = len(code)
        code += b"\xcc" * (-(rva + len(code)) % 4)
        table = rva + len(code)
        if bound:
            target = load_at if default_in_path else default
            code[default_at + 2: default_at + 6] = struct.pack("<i", target - (default_at + 6))
        if table_rva is None:
            code += b"".join(struct.pack("<I", c) for c in cases)
        else:
            table = table_rva
        code[load_at + 3: load_at + 7] = struct.pack("<I", table)
        if byte_table:
            code[bytes_at + 4: bytes_at + 8] = struct.pack("<I", rva + len(code))
            code += bytes(byte_table)
        self.code += b"\xcc" * (rva - self.rva())
        self.functions.append((rva, rva + end))
        self.code += code
        return table


def align(n, a):
    return (n + a - 1) // a * a


def build_pe(text, exports=None, extra_code=None):
    """A PE32+ image with .text, an export table and unwind info in .rdata, and .pdata: what isa-gate.py reads.
    extra_code is a second executable section, also named .text, placed after .pdata."""
    exports = exports or {}
    rdata_rva = align(TEXT_RVA + len(text.code), 0x1000)
    names = sorted(exports)
    dir_size = 40
    funcs_at = rdata_rva + dir_size
    names_at = funcs_at + 4 * len(names)
    ords_at = names_at + 4 * len(names)
    strings_at = ords_at + 2 * len(names)
    strings = bytearray(b"test.exe\0")
    name_rvas = []
    for n in names:
        name_rvas.append(strings_at + len(strings))
        strings += n.encode() + b"\0"
    rdata = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, strings_at, 1, len(names), len(names), funcs_at, names_at, ords_at)
    rdata += b"".join(struct.pack("<I", exports[n]) for n in names)
    rdata += b"".join(struct.pack("<I", r) for r in name_rvas)
    rdata += b"".join(struct.pack("<H", i) for i in range(len(names)))
    rdata += strings
    rdata += b"\0" * (-len(rdata) % 4)
    unwind = rdata_rva + len(rdata)
    rdata += bytes([1, 0, 0, 0])                     # UNWIND_INFO version 1, no flags, no prolog
    chained = rdata_rva + len(rdata)
    rdata += bytes([1 | 4 << 3, 0, 0, 0])            # UNW_FLAG_CHAININFO, then the parent's RUNTIME_FUNCTION
    rdata += struct.pack("<III", *(text.functions[0] if text.functions else (0, 0)), unwind)
    pdata_rva = align(rdata_rva + len(rdata), 0x1000)
    entries = [(b, e, unwind) for b, e in text.functions] + [(b, e, chained) for b, e in text.chained]
    pdata = b"".join(struct.pack("<III", *e) for e in sorted(entries))

    sections = [(".text", TEXT_RVA, bytes(text.code), 0x60000020), (".rdata", rdata_rva, rdata, 0x40000040),
                (".pdata", pdata_rva, pdata, 0x40000040)]
    if extra_code:
        sections.append((".text", align(pdata_rva + len(pdata), 0x1000), extra_code, 0x60000020))
    headers = 0x400
    raw = headers
    table = b""
    body = b""
    for name, rva, data, flags in sections:
        size = align(len(data), 0x200)
        table += struct.pack("<8sIIIIIIHHI", name.encode(), len(data), rva, size, raw, 0, 0, 0, 0, flags)
        body += data.ljust(size, b"\0")
        raw += size
    image_size = align(sections[-1][1] + len(sections[-1][2]), 0x1000)
    dirs = [(0, 0)] * 16
    if names:
        dirs[0] = (rdata_rva, len(rdata))
    dirs[3] = (pdata_rva, len(pdata))
    opt = struct.pack("<HBBIIIII", 0x20B, 14, 0, len(text.code), len(rdata) + len(pdata), 0, 0, TEXT_RVA)
    opt += struct.pack("<QIIHHHHHHIIIIHHQQQQII", 0x140000000, 0x1000, 0x200, 6, 0, 0, 0, 6, 0, 0,
                       image_size, headers, 0, 3, 0x8160, 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    coff = struct.pack("<HHIIIHH", 0x8664, len(sections), 0, 0, 0, len(opt), 0x22)
    dos = b"MZ".ljust(0x3C, b"\0") + struct.pack("<I", 0x40)
    head = (dos + b"PE\0\0" + coff + opt + table).ljust(headers, b"\0")
    return head + body


def cases():
    """(name, the PE, extra gate arguments, expected exit code, counts the report must show)."""
    t = Text()
    t.switch(0x1000, EVEX_CASES)
    for c in EVEX_CASES:
        t.place(c, bytes.fromhex("b8") + struct.pack("<I", c) + b"\xc3", function=False)
    yield "table bytes that decode as EVEX are data", build_pe(t), [], 0, {"AVX512": 0, "failing": 0, "tables": 1}

    # MSVC gives the range after an epilog a chained .pdata entry, and that range can start with the switch table.
    t = Text()
    t.switch(0x1000, EVEX_CASES)
    t.chained.append((0x1024, 0x1034))
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a chained .pdata entry that starts at the table", build_pe(t), [], 0, {"AVX512": 0, "tables": 1}

    # The bytes before the AVX2 case make a linear sweep read the case's first instruction as an imm64 operand, so
    # only tracing the table's targets finds it.
    t = Text()
    t.switch(0x1000, EVEX_CASES)
    for i, c in enumerate(EVEX_CASES):
        if i == 2:
            t.place(c - 2, MOV_RAX_IMM64, function=False)
        t.place(c, (AVX2 if i == 2 else b"") + RET0, function=False)
    yield "AVX2 in a case reached only through the table", build_pe(t), [], 1, {"AVX2": 1, "AVX512": 0}

    t = Text()
    t.switch(0x1000, EVEX_CASES, byte_table=[3, 0, 1, 2, 2, 1])
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a switch through a byte table", build_pe(t), [], 0, {"AVX512": 0, "tables": 2}

    t = Text()
    t.switch(0x1000, EVEX_CASES, byte_table=[3, 0, 1, 2], clobber_base=True)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a base register written after the byte table counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.switch(0x1000, EVEX_CASES, scale=8)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a load scaled by 8 counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.place(0x1000, b"".join(struct.pack("<I", c) for c in EVEX_CASES), function=False)
    t.switch(0x1100, EVEX_CASES, table_rva=0x1000)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a table before its jump counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.switch(0x1000, EVEX_CASES, default_in_path=True)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a bound whose jump lands before the load counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.switch(0x1000, EVEX_CASES, bound="jne")
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a cmp not followed by ja counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.switch(0x1000, EVEX_CASES[:3] + [0x7FFF0000])
    for c in EVEX_CASES[:3]:
        t.place(c, RET0, function=False)
    yield "a table with an entry outside the code counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    table = t.switch(0x1000, EVEX_CASES)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    at = t.rva()
    t.place(at, b"\xe9" + struct.pack("<i", table - (at + 5)))                  # jmp table
    yield "a table that traced code jumps into counts as code", build_pe(t), [], 1, \
        {"AVX512": 1, "tables_overlapping_code": 1}

    t = Text()
    t.switch(0x1000, EVEX_CASES)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    t.place(0x60000, AVX512 + RET0)
    yield "AVX-512 in a function", build_pe(t), [], 1, {"AVX512": 1}

    t = Text()
    t.switch(0x1000, EVEX_CASES, bound=None)
    for c in EVEX_CASES:
        t.place(c, RET0, function=False)
    yield "a table with no bound check counts as code", build_pe(t), [], 1, {"AVX512": 1, "tables": 0}

    t = Text()
    t.place(0x1000, EVEX_XMM + RET0)
    yield "an EVEX instruction on xmm0-15 with no mask", build_pe(t), [], 1, {"AVX512": 1}

    t = Text()
    t.place(0x1000, RET0)
    yield "a second executable section with the same name", build_pe(t, extra_code=AVX2 + RET0), [], 1, {"AVX2": 1}

    t = Text()
    t.place(0x1000, AVX1_YMM + AVX2_YMM_ONLY + RET0)
    yield "vptest ymm is AVX, vmovntdqa ymm is AVX2", build_pe(t), [], 1, {"AVX2": 1}

    t = Text()
    t.place(0x1000, RET0)
    yield "a file that decodes to almost nothing fails", build_pe(t), ["--min-instructions", "1000"], 1, {"failing": 0}

    t = Text()
    t.place(0x1000, RET0)
    t.place(0x1100, AVX2 + RET0, function=False)
    yield "code nothing reaches counts", build_pe(t), [], 1, {"AVX2": 1}

    for allow, code in ((["--allow", "^cos$"], 0), ([], 1)):
        t = Text()
        cos = t.place(0x1000, FMA3 + RET0)
        yield f"an FMA3 body with {allow or 'no allow'}", build_pe(t, {"cos": cos}), allow, code, {"FMA3": 1}

    t = Text()
    cos = t.place(0x1000, FMA3 + RET0)
    t.place(0x1100, FMA3 + RET0)
    yield "an allow does not reach past its function", build_pe(t, {"cos": cos}), ["--allow", "^cos$"], 1, \
        {"FMA3": 2, "allowed": 1, "failing": 1}

    t = Text()
    cos = t.place(0x1000, FMA3 + RET0)
    t.place(0x1100, FMA3 + RET0, function=False)
    yield "an allow does not reach an unlabelled leaf after its function", build_pe(t, {"cos": cos}), \
        ["--allow", "^cos$"], 1, {"FMA3": 2, "allowed": 1, "failing": 1}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--objdump", default=shutil.which("llvm-objdump") or shutil.which("objdump"))
    args = ap.parse_args()
    failures = 0
    with tempfile.TemporaryDirectory() as work:
        for i, (name, image, extra, want_code, want) in enumerate(cases()):
            path = os.path.join(work, f"case{i}.exe")
            pathlib.Path(path).write_bytes(image)
            run = subprocess.run([sys.executable, str(GATE), "--tool", args.objdump, "--verbose", "--min-instructions", "1",
                                  *extra, path],
                                 capture_output=True, text=True)
            got = {k: int(v) for k, v in re.findall(r"^(\w+) (\d+)$", run.stdout, re.M)}
            wrong = [f"{k} {got.get(k)} (want {v})" for k, v in want.items() if got.get(k) != v]
            if run.returncode != want_code:
                wrong.insert(0, f"exit {run.returncode} (want {want_code})")
            print(f"{'pass' if not wrong else 'FAIL'}: {name}")
            if wrong:
                failures += 1
                print("  " + "; ".join(wrong))
                print("  " + (run.stdout + run.stderr).strip().replace("\n", "\n  "))
    print(f"{failures} failing")
    return failures


if __name__ == "__main__":
    sys.exit(main())
