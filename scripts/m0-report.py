#!/usr/bin/env python3
"""Reads the M0 probe's m0.tsv (host/m0/probe.cpp) and applies SYNTHESIS.md's M0 decision rule.

    m0-report.py <m0.tsv> [--label <where it ran>]
    m0-report.py --self-test

Proceed when 99% or more of the corpus's pipelines are accepted on D3D11On12 and every refusal is attributable, the
median shader takes under 150 ms through the chain, and a draw with a pipeline change every other draw costs under
40 us of CPU. A refusal is attributable when the row names its class: the chain step and its error, the D3D11 object
and HRESULT that failed, the direct D3D12 pipeline's HRESULT, or the exception code. A draw D3D11On12 dropped while
the same state built as a D3D12 pipeline, or an item that ended the process with no code, is not.

Exit 0 on proceed, 3 on do-not-proceed, 1 on an unreadable file.
"""
import re
import statistics
import sys
from collections import Counter

ACCEPT_SHARE = 0.99
CHAIN_MEDIAN_MS = 150.0
DRAW_US = 40.0


def pct(values, p):
    if not values:
        return 0.0
    v = sorted(values)
    return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]


def error_class(text):
    t = re.sub(r"0x[0-9A-Fa-f]+", "N", text)
    t = re.sub(r"\b\d+\b", "N", t)
    return t[:120]


def analyse(rows):
    shaders = [r for r in rows if r[0] == "shader"]
    pipes = [r for r in rows if r[0] == "pipeline"]
    benches = {r[1]: r for r in rows if r[0] == "bench"}
    meta = {r[1]: r[2] for r in rows if r[0] == "meta" and len(r) > 2}
    out, refusals, unattributed = [], [], []

    ok = [r for r in shaders if r[4] == "ok"]
    times = {k: [float(r[i]) for r in ok] for k, i in (("glslang", 6), ("spirv-cross", 7), ("d3dcompile", 8))}
    total = [float(r[6]) + float(r[7]) + float(r[8]) for r in ok]
    stage = Counter(r[2] for r in shaders)
    out.append(f"corpus: {meta.get('corpus_shaders', len(shaders))} shaders "
               f"({', '.join(f'{s} {n}' for s, n in sorted(stage.items()))}), "
               f"{meta.get('corpus_pipelines', len(pipes))} pipelines, {meta.get('corpus_bytes', '?')} bytes")
    out.append(f"adapter: {meta.get('adapter', '?')}")
    out.append(f"chain: {len(ok)}/{len(shaders)} shaders ok; per shader median {pct(total, .5):.1f} ms, "
               f"p95 {pct(total, .95):.1f} ms, max {max(total, default=0):.1f} ms, sum {sum(total) / 1000:.1f} s")
    for k, v in times.items():
        out.append(f"  {k}: median {pct(v, .5):.2f} ms, p95 {pct(v, .95):.2f} ms, sum {sum(v) / 1000:.1f} s")
    for s in sorted(stage):
        v = [float(r[6]) + float(r[7]) + float(r[8]) for r in ok if r[2] == s]
        if v:
            out.append(f"  {s}: median {pct(v, .5):.1f} ms over {len(v)}")
    rewrites = Counter(r[11] for r in ok if len(r) > 11 and r[11].startswith("rewrite"))
    for rw, n in rewrites.most_common():
        out.append(f"  {n} shaders took the D3D {rw}")
    fails = Counter((r[5], error_class(r[11] if len(r) > 11 else "")) for r in shaders if r[4] == "fail")
    for (step, cls), n in fails.most_common():
        out.append(f"  chain FAIL {n} at {step}: {cls}")
    died = [r for r in shaders if r[4] == "died"]
    if died:
        unattributed.append(f"{len(died)} shaders ended the process inside the chain")

    on12 = Counter(r[2] for r in pipes)
    accepted = on12.get("ok", 0)
    out.append(f"pipelines on D3D11On12: {accepted}/{len(pipes)} accepted "
               f"({100.0 * accepted / max(1, len(pipes)):.2f}%); " +
               ", ".join(f"{k} {n}" for k, n in sorted(on12.items()) if k != "ok"))
    d3d12 = Counter(r[3] for r in pipes)
    out.append("direct D3D12 pipelines: " + ", ".join(f"{k} {n}" for k, n in sorted(d3d12.items())))
    pso = [float(r[5]) for r in pipes if r[3] == "ok"]
    create = [float(r[6]) for r in pipes if r[2] in ("ok", "dropped")]
    draw = [float(r[7]) + float(r[8]) for r in pipes if r[2] in ("ok", "dropped")]
    out.append(f"  D3D12 pipeline creation: median {pct(pso, .5):.2f} ms, p95 {pct(pso, .95):.2f} ms, "
               f"sum {sum(pso) / 1000:.1f} s")
    out.append(f"  D3D11 object creation: median {pct(create, .5):.2f} ms; first draw to done (D3D11On12's own "
               f"pipeline): median {pct(draw, .5):.2f} ms, p95 {pct(draw, .95):.2f} ms")
    notes = Counter(n for r in pipes if len(r) > 10 and r[10] != "-" for n in r[10].split(","))
    if notes:
        out.append("  renderer mapping rules exercised: " + ", ".join(f"{k} {n}" for k, n in notes.most_common()))

    refusing_suites = Counter()
    for r in pipes:
        res, r12, hr12, detail = r[2], r[3], r[4], (r[11] if len(r) > 11 else "-")
        if res == "ok":
            continue
        refusing_suites[r[12] if len(r) > 12 else "-"] += 1
        if res == "shader_failed":
            refusals.append("shader chain failed (see chain FAIL)")
        elif res == "create_fail":
            refusals.append(f"D3D11 {error_class(detail)}")
        elif res == "dropped" and r12 == "fail":
            refusals.append(f"dropped; D3D12 pipeline refused hr={hr12}")
        elif res == "thrown" and detail != "-":
            refusals.append(f"thrown: {error_class(detail)}")
        elif res == "removed":
            refusals.append(f"device removed: {error_class(detail)}" + (f"; D3D12 pipeline hr={hr12}" if r12 == "fail" else ""))
        else:
            unattributed.append(f"pipeline {r[1]} {res} (D3D12 {r12} hr={hr12}) {detail}")
    for cls, n in Counter(refusals).most_common():
        out.append(f"  refusal {n}: {cls}")
    if refusing_suites:
        out.append("  refusals by test suite: " + ", ".join(f"{k} {n}" for k, n in refusing_suites.most_common(15)))

    for name in ("same", "switch", "switch_cb"):
        b = benches.get(name)
        if b:
            out.append(f"bench {name}: {b[4]} us/draw median (p95 {b[5]}), {b[6]} us/frame median (p95 {b[7]}), "
                       f"CPU {b[8]} us/draw on the thread, {b[9]} us/draw in the process, GPU wait {b[10]} ms/frame")
    bench_done = [r for r in rows if r[0] == "benchdone"]
    if bench_done and bench_done[-1][1] != "ok":
        out.append(f"bench not run: {bench_done[-1][1]}")
    out.append("launches: " + str(sum(1 for r in rows if r[0] == "launch")) +
               ", relaunches: " + str(sum(1 for r in rows if r[0] == "relaunch")) +
               ", cache clear: " + " ".join(f"{r[1]}={r[2]}" for r in rows if r[0] == "cache"))
    for u in unattributed:
        out.append(f"  UNATTRIBUTED {u}")

    share = accepted / max(1, len(pipes))
    switch = benches.get("switch")
    switch_us = float(switch[4]) if switch else None
    checks = [
        (share >= ACCEPT_SHARE, f"accepted {100 * share:.2f}% (needs {100 * ACCEPT_SHARE:.0f}%)"),
        (not unattributed, f"{len(unattributed)} unattributed refusals (needs 0)"),
        (bool(total) and pct(total, .5) < CHAIN_MEDIAN_MS,
         f"chain median {pct(total, .5):.1f} ms (needs under {CHAIN_MEDIAN_MS:.0f})"),
        (switch_us is not None and switch_us < DRAW_US,
         f"switch draw {switch_us if switch_us is not None else 'not measured'} us (needs under {DRAW_US:.0f})"),
        (any(r[0] == "end" for r in rows), "the probe reached its end row"),
    ]
    proceed = all(ok for ok, _ in checks)
    out.append(("PROCEED" if proceed else "DO NOT PROCEED") + ": " +
               "; ".join(("ok " if ok else "FAIL ") + text for ok, text in checks))
    return proceed, out


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return [line.rstrip("\n").split("\t") for line in f if line.strip()]


def self_test():
    base = [
        ["launch", "1"], ["meta", "corpus_shaders", "2"], ["meta", "corpus_pipelines", "2"],
        ["shader", "0", "vs", "aaa", "ok", "-", "10", "5", "20", "100", "200", "-"],
        ["shader", "1", "ps", "bbb", "ok", "-", "12", "6", "30", "100", "200", "-"],
        ["pipeline", "0", "ok", "ok", "0x00000000", "3", "1", "1", "1", "3", "-", "-"],
        ["pipeline", "1", "ok", "ok", "0x00000000", "3", "1", "1", "1", "3", "-", "-"],
        ["bench", "switch", "200", "1000", "12.5", "15", "12500", "15000", "11", "13", "1.2"],
        ["benchdone", "ok"], ["end"],
    ]
    proceed, _ = analyse(base)
    assert proceed, "a clean run proceeds"
    dropped = [r if r[:2] != ["pipeline", "1"] else ["pipeline", "1", "dropped", "ok", "0x00000000", "3", "1", "1",
                                                      "1", "0", "-", "-"] for r in base]
    proceed, out = analyse(dropped)
    assert not proceed and any("UNATTRIBUTED" in o for o in out), "a draw dropped with D3D12 accepting is unattributed"
    slow = [r if r[:2] != ["bench", "switch"] else ["bench", "switch", "200", "1000", "45", "50", "45000", "50000",
                                                     "44", "46", "1"] for r in base]
    assert not analyse(slow)[0], "45 us per draw does not proceed"
    unfinished = [r for r in base if r[0] != "end"]
    assert not analyse(unfinished)[0], "a run without its end row does not proceed"
    print("m0-report self-test ok")


def main(argv):
    if argv[1:] == ["--self-test"]:
        self_test()
        return 0
    if len(argv) not in (2, 4) or (len(argv) == 4 and argv[2] != "--label"):
        sys.exit(__doc__)
    try:
        rows = read(argv[1])
    except OSError as e:
        print(f"cannot read {argv[1]}: {e}")
        return 1
    proceed, out = analyse(rows)
    if len(argv) == 4:
        print(f"M0 report ({argv[3]})")
    print("\n".join(out))
    return 0 if proceed else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
