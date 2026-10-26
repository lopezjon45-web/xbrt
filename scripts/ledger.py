#!/usr/bin/env python3
"""Turns survey.tsv (xbrt-inspect --survey, or the console's xboxrt:?survey=) into coverage.md: one row per title,
then every gap ranked by how many titles it touches. That ranking is the order of work toward all games.

    ledger.py survey.tsv [--hits <dir>] [--coverage-dir <dir>] [-o coverage.md]

--hits          a directory of titles/<sha>/hits.tsv from runs (the dynamic pass, U2 on)
--coverage-dir  xbox-runtime/coverage: compares the survey's fs and rdtsc counts with Ghidra's and its named
                functions with XbSymbolDatabase's, from <title id>/summary.json

The output is derived from game data: it stays on the machine, like survey.tsv.
"""
import argparse
import json
import os
import sys

COLUMNS = ["image", "title", "title_id", "entry", "verdict", "reason", "xbes", "libraries", "imports_total",
           "imports_no_bridge", "imports_stub", "fs_sites", "fs_unpromotable", "rdtsc_sites", "privileged_sites",
           "bytes_decoded", "functions", "overrides_placed", "calls_launch_new_image"]

GAP_CLASSES = ["kernel export with no bridge", "stub bridge imported", "library the database does not know",
               "unpromotable fs forms"]


def split(cell):
    return [v for v in cell.split(";") if v]


def read_survey(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    if not lines or lines[0].split("\t") != COLUMNS:
        sys.exit(f"{path}: the header is not survey.tsv's ({' '.join(COLUMNS)})")
    titles = []
    for n, line in enumerate(lines[1:], start=2):
        cells = line.split("\t")
        if len(cells) != len(COLUMNS):
            sys.exit(f"{path}:{n}: {len(cells)} cells, the header has {len(COLUMNS)}")
        row = dict(zip(COLUMNS, cells))
        libraries = []
        for lib in split(row["libraries"]):
            name, build, known, named = lib.split(":")
            libraries.append({"name": name, "build": build, "known": known == "known", "named": int(named)})
        row["libraries"] = libraries
        row["label"] = row["title"] or os.path.basename(row["image"])
        titles.append(row)
    return titles


def gaps_of(t):
    if t["verdict"] == "unparsed":
        return []
    out = [(GAP_CLASSES[0], o) for o in split(t["imports_no_bridge"])]
    out += [(GAP_CLASSES[1], o) for o in split(t["imports_stub"])]
    out += [(GAP_CLASSES[2], lib["name"]) for lib in t["libraries"] if not lib["known"]]
    if int(t["fs_unpromotable"]):
        out.append((GAP_CLASSES[3], "fs form with no micro-gate"))
    return out


def ghidra_rdtsc(ghidra):
    return sum(n["count"] for n in ghidra.get("notable", []) if n["instruction"] == "RDTSC")


def compare(t, coverage_dir):
    if t["verdict"] == "unparsed":
        return "", ""
    path = os.path.join(coverage_dir, t["title_id"], "summary.json")
    try:
        with open(path, encoding="utf-8") as f:
            summary = json.load(f)
    except OSError:
        return "no summary.json", "no summary.json"
    ghidra = summary.get("ghidra", {})
    if ghidra.get("status") != "ok":
        code = f"Ghidra {ghidra.get('status', 'absent')}"
    else:
        code = f"fs {t['fs_sites']}/{ghidra['fs']['uses']}, rdtsc {t['rdtsc_sites']}/{ghidra_rdtsc(ghidra)}"
    matches = summary.get("xbsdb", {}).get("matches", {})
    named = sum(lib["named"] for lib in t["libraries"])
    return code, f"{named}/{sum(len(m.get('functions', [])) for m in matches.values())}"


def hits_files(hits_dir):
    found = []
    for root, _, files in os.walk(hits_dir):
        found += [os.path.join(root, f) for f in files if f == "hits.tsv"]
    return found


def name_order(name):
    ordinal = name.split("=")[0]
    return (int(ordinal), name) if ordinal.isdigit() else (0, name)


def md(text):
    return str(text).replace("|", "\\|")


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    out += ["| " + " | ".join(md(c) for c in r) + " |" for r in rows]
    return out


def render(titles, survey_path, coverage_dir, hits_note):
    out = ["# Coverage ledger", "",
           f"From `{os.path.basename(survey_path)}`: {len(titles)} titles. Local only: derived from game data.", ""]
    counts = {v: sum(t["verdict"] == v for t in titles) for v in ("ready", "named-gaps", "unparsed")}
    out += [f"Verdicts: {counts['ready']} ready, {counts['named-gaps']} named gaps, {counts['unparsed']} unparsed.",
            "", "## Titles", ""]
    header = ["title", "title id", "verdict", "XDK builds", "imports", "no bridge", "stub", "unknown libraries",
              "fs sites", "fs unpromotable", "rdtsc sites"]
    if coverage_dir:
        header += ["survey/Ghidra", "named survey/xbsdb"]
    rows = []
    for t in titles:
        if t["verdict"] == "unparsed":
            row = [t["label"], t["title_id"], f"unparsed ({t['reason']})"] + [""] * (len(header) - 3)
            rows.append(row)
            continue
        builds = sorted({lib["build"] for lib in t["libraries"]}, key=int)
        row = [t["label"], t["title_id"], t["verdict"], ", ".join(builds), t["imports_total"],
               len(split(t["imports_no_bridge"])), len(split(t["imports_stub"])),
               ", ".join(lib["name"] for lib in t["libraries"] if not lib["known"]) or "none",
               t["fs_sites"], t["fs_unpromotable"], t["rdtsc_sites"]]
        if coverage_dir:
            row += list(compare(t, coverage_dir))
        rows.append(row)
    out += table(header, rows) + [""]

    touched = {}
    for t in titles:
        for gap in dict.fromkeys(gaps_of(t)):
            touched.setdefault(gap, []).append(t["label"])
    ranked = sorted(touched.items(), key=lambda g: (-len(g[1]), GAP_CLASSES.index(g[0][0]), name_order(g[0][1])))
    out += ["## Gaps by titles touched", "",
            "Imports with no bridge and unpromotable fs forms stop a title when reached; a stub warns once; an "
            "unknown library loses names and overrides, never correctness.", ""]
    if ranked:
        out += table(["gap", "class", "titles", "which"],
                     [[name, cls, len(who), ", ".join(who)] for (cls, name), who in ranked])
    else:
        out.append("No gaps.")
    out += ["", "## Dynamic pass", "", hits_note, ""]
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("survey")
    ap.add_argument("--hits")
    ap.add_argument("--coverage-dir")
    ap.add_argument("-o", "--out")
    args = ap.parse_args()

    titles = read_survey(args.survey)
    if args.coverage_dir and not os.path.isdir(args.coverage_dir):
        sys.exit(f"--coverage-dir {args.coverage_dir}: not a directory")
    hits_note = "No --hits given."
    if args.hits:
        found = hits_files(args.hits)
        if found:
            sys.exit(f"{len(found)} hits.tsv under {args.hits}, but joining them arrives with U2's hits.tsv format")
        hits_note = "No hits yet."
    text = render(titles, args.survey, args.coverage_dir, hits_note)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
