import bisect, importlib.util, re, subprocess, sys
spec = importlib.util.spec_from_file_location("gate", "scripts/isa-gate.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
old = subprocess.run([sys.executable, "old-gate.py", "--tool", "dumpbin", "--verbose", "--report-only", sys.argv[1]],
                     capture_output=True, text=True).stdout
hits = [(int(m.group(1), 16), m.group(0).strip()) for m in re.finditer(r"^\s+AVX512\s+([0-9A-F]+) .*$", old, re.M)]
print(f"old linear-sweep gate: {len(hits)} AVX512 hits")
image = gate.Image(sys.argv[1])
judged, tables, overlapping, hidden, unresolved, code = gate.analyse(image)
names = gate.Names(gate.labels("dumpbin", sys.argv[1], image), image)
for va, line in hits:
    print("old:", line)
    inside = [t for t in tables if t.va <= va < t.va + t.size]
    for t in inside:
        print(f"  new: inside proven table {t.va:X}+{t.size} read by the jump at {t.jump:X} in {names(t.jump)}")
        print("  jump:", code[t.jump].mnemonic, code[t.jump].op_str)
    print("  traced as code by the new gate:", any(a <= va < a + i.size for a, i in code.items() if va - 16 < a <= va))
