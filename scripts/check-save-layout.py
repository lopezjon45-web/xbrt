#!/usr/bin/env python3
"""Checks where lab/save's saves landed: <saves root>\\<title id>\\TDATA and \\UDATA (INTEGRATION.md section 2).

    check-save-layout.py <saves root> <title id> <launches>

Passes when the title has exactly its TDATA and UDATA folders, T:'s state.bin counts <launches> and U:'s save.dat
holds the last launch's text, and nothing else is under the saves root: no second title folder, no title id nested
inside TDATA, no TitleData or UserData folder.
"""
import pathlib
import struct
import sys


def check(root, title_id, launches):
    problems = []
    title = root / title_id
    state = title / "TDATA" / "lab" / "state.bin"
    save = title / "UDATA" / "slot" / "save.dat"
    if not state.is_file():
        problems.append(f"no {state}")
    elif struct.unpack("<I", state.read_bytes()[:4])[0] != launches:
        problems.append(f"{state} counts {struct.unpack('<I', state.read_bytes()[:4])[0]} launches, not {launches}")
    if not save.is_file():
        problems.append(f"no {save}")
    elif save.read_bytes() != f"save {launches % 10}\0".encode():
        problems.append(f"{save} holds {save.read_bytes()!r}")
    expected = {state, save, state.parent, save.parent, title / "TDATA", title / "UDATA", title}
    for p in sorted(root.rglob("*")):
        if p not in expected:
            problems.append(f"unexpected {p.relative_to(root)}")
    return problems


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    root = pathlib.Path(sys.argv[1])
    problems = check(root, sys.argv[2], int(sys.argv[3]))
    for p in sorted(root.rglob("*")):
        print(f"  {p.relative_to(root)}")
    print("save layout: " + ("ok" if not problems else "FAIL\n  " + "\n  ".join(problems)))
    sys.exit(1 if problems else 0)
