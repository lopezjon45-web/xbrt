#!/usr/bin/env python3
"""Classifies every xboxkrnl ordinal by what the fork's kernel bridge does for it and writes the table core/survey.cpp
compiles in, so the survey names stubs and missing exports from the bridge source itself, never from a hand list.

    gen-bridge-status.py <kernel_bridge.c> <out .inc>          write the table
    gen-bridge-status.py <kernel_bridge.c> --check <.inc>      exit 1 when the committed table differs (the CI test)

A stub is refused when the guest calls it (an unimplemented export is a named refusal, never a faked success),
unless scripts/kernel-noop-exports.tsv lists it with the reason doing nothing is the console's answer in xbrt; those
are class noop. Listing an ordinal that is not a stub, under another name or without a reason, is an error, so the
list cannot outlive the bridge it excuses.

Classes, from bridge_for_ordinal(), kernel_data_va_for_ordinal() and each bridge_<Name> body:
    none     no case in either switch (the thunk slot gets the "missing" handler at run time)
    data     a data export the fork serves from its kernel data page (KeTickCount, XboxHardwareInfo, ...)
    stub     bridge_generic_stub, or a body that only pops its arguments and writes constants into g_eax/g_edx:
             no call to anything but logging, argument macros and idle layer-A functions, no write to guest
             memory; a body whose own comment says "stub" and calls nothing in layer A counts too. An idle
             layer-A function is an xbox_* function in the bridge's folder whose body only casts its
             arguments to void or returns a constant: calling it does nothing
    partial  a body that calls a real implementation but carries a TODO, "not implemented", "not supported",
             "unsupported" or "ignored" note of its own
    noop     a stub listed in scripts/kernel-noop-exports.tsv
    full     everything else that calls into layer A (xbox_*) or does the work itself
"""
import pathlib
import re
import sys

NOOP_LIST = pathlib.Path(__file__).with_name("kernel-noop-exports.tsv")

ORDINAL_MAX = 378
LOG_CALLS = {"fprintf", "fflush", "printf", "snprintf", "KERNEL_LOG_ON_HALF",
             "STACK_ARG", "BRIDGE_MEM8", "BRIDGE_MEM16", "BRIDGE_MEM32", "MEM8", "MEM16", "MEM32", "XBOX_TO_NATIVE",
             "XBOX_PTR", "bridge_buf_ok", "sizeof", "if", "for", "while", "switch", "return"}
PARTIAL_WORDS = re.compile(r"\b(TODO|FIXME|not implemented|not supported|unsupported|ignored|unimplemented)\b", re.I)
STUB_WORDS = re.compile(r"\bstub\b", re.I)


def routes(src):
    body = src[src.index("static bridge_func_t bridge_for_ordinal("):]
    body = body[:body.index("\n}\n")]
    return {int(o): name for o, name in re.findall(r"case\s+(\d+):\s*return\s+(bridge_\w+)\s*;", body)}


def data_exports(src):
    body = src[src.index("static uint32_t kernel_data_va_for_ordinal("):]
    body = body[:body.index("\n}\n")]
    return {int(o) for o in re.findall(r"case\s+(\d+):\s*return\s+XBOX_KERNEL_DATA_BASE", body)}


def bodies(src):
    out = {}
    for m in re.finditer(r"static void (bridge_\w+)\(void\)\s*\{", src):
        start = m.end()
        depth, i = 1, start
        while depth and i < len(src):
            depth += {"{": 1, "}": -1}.get(src[i], 0)
            i += 1
        out[m.group(1)] = src[start:i - 1]
    return out


def strip_comments(body):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S)


CONSTANT = re.compile(r"^\s*\(?\s*(0x[0-9A-Fa-f]+|\d+|[A-Z][A-Z0-9_]+|TRUE|FALSE)\s*\)?\s*$")


def idle_layer_a(folder):
    """The xbox_* functions in `folder` whose bodies do nothing: argument casts to void and constant returns only."""
    idle = set()
    for path in sorted(pathlib.Path(folder).glob("*.c")):
        src = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"^[A-Za-z_][\w \t*]*?\b(xbox_\w+)\s*\([^;{]*?\)\s*\{", src, re.M):
            depth, i = 1, m.end()
            while depth:
                depth += {"{": 1, "}": -1}.get(src[i], 0)
                i += 1
            rest = re.sub(r"\(void\)\s*\w+\s*;", "", strip_comments(src[m.end():i - 1]))
            rest = re.sub(r"return\s+\(?\s*(0x[0-9A-Fa-f]+|\d+|[A-Z][A-Z0-9_]+|TRUE|FALSE)\s*\)?\s*;", "", rest)
            if not rest.strip():
                idle.add(m.group(1))
    return idle


def does_work(code, idle):
    calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", code)) - LOG_CALLS - idle
    if {c for c in calls if not c.isupper()}:
        return True
    if re.search(r"\b(BRIDGE_)?MEM(8|16|32)\s*\([^;]*\)\s*=[^=]", code):
        return True
    # A store through a pointer or into an array, or into file-scope state other than the result registers.
    if re.search(r"\b\w+\s*\[[^\]]*\]\s*=[^=]", code) or re.search(r"\bg_(?!e(?:ax|dx)\b)\w+\s*=[^=]", code):
        return True
    return any(not CONSTANT.match(rhs) for rhs in re.findall(r"\bg_e(?:ax|dx)\s*=([^;]*);", code))


def classify(name, body, idle):
    if name == "bridge_generic_stub":
        return "stub"
    code = strip_comments(body)
    comments = " ".join(re.findall(r"/\*.*?\*/|//[^\n]*", body, flags=re.S))
    if not does_work(code, idle):
        return "stub"
    real = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", code)) - LOG_CALLS - idle
    if STUB_WORDS.search(comments) and not any(c.startswith("xbox_") for c in real):
        return "stub"
    if PARTIAL_WORDS.search(comments):
        return "partial"
    return "full"


def noop_list():
    rows = {}
    for line in NOOP_LIST.read_text(encoding="utf-8").splitlines()[1:]:
        if not line.strip():
            continue
        ordinal, name, why = (line.split("\t") + ["", ""])[:3]
        rows[int(ordinal)] = (name, why.strip())
    return rows


def table(src, idle):
    route, body, data = routes(src), bodies(src), data_exports(src)
    rows, errors = [], []
    for o in range(ORDINAL_MAX + 1):
        name = route.get(o)
        if o in data:
            rows.append((o, "data", "kernel data page"))
        elif name is None:
            rows.append((o, "none", ""))
        else:
            rows.append((o, classify(name, body[name], idle), name))
    for o, (name, why) in noop_list().items():
        cls, bridge = rows[o][1], rows[o][2]
        if cls != "stub" or bridge != "bridge_" + name or not why:
            errors.append(f"{NOOP_LIST.name}: {o} {name} is {cls} ({bridge or 'no bridge'}){'' if why else ', no reason'}: "
                          "only a stub with a reason may be listed")
        else:
            rows[o] = (o, "noop", bridge)
    return rows, errors


def render(rows):
    names = {"none": "None", "stub": "Stub", "noop": "NoOp", "partial": "Partial", "full": "Full", "data": "Data"}
    lines = ["// Generated by scripts/gen-bridge-status.py from external/xboxrecomp/src/kernel/kernel_bridge.c. Index = ordinal.",
             f"inline constexpr BridgeStatus kBridgeStatus[{ORDINAL_MAX + 1}] = {{"]
    for o, cls, name in rows:
        lines.append(f"    BridgeStatus::{names[cls]},   // {o} {name}".rstrip())
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv):
    rows, errors = table(open(argv[1], encoding="utf-8", errors="replace").read(),
                         idle_layer_a(pathlib.Path(argv[1]).parent))
    if errors:
        print("\n".join(errors))
        return 1
    text = render(rows)
    counts = {}
    for _, cls, _ in rows:
        counts[cls] = counts.get(cls, 0) + 1
    summary = " ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    if argv[2] == "--check":
        committed = open(argv[3], encoding="utf-8").read()
        if committed != text:
            print(f"{argv[3]} is out of date: rerun gen-bridge-status.py ({summary})")
            return 1
        print(f"bridge status table matches kernel_bridge.c ({summary})")
        return 0
    open(argv[2], "w", encoding="utf-8").write(text)
    print(f"{argv[2]}: {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
