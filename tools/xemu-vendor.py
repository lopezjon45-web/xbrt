#!/usr/bin/env python3
r"""The vendored upstream sources: copy them at their pins (sync), or prove the tree is exactly that (check).

    xemu-vendor.py sync   [--root <repo>] [--cache <dir>]
    xemu-vendor.py check  [--root <repo>] [--cache <dir>]
    xemu-vendor.py list-c [--root <repo>]      the compile list build.ps1 reads: external\<source>\<path> per .c

external/xemu-vendor/MANIFEST lists every vendored file under "@<source> <url> <commit>" sections and the patch
budget on its "@cap" line. external/xemu-vendor/patches/*.patch apply in name order, with paths relative to
external/ (a/xemu/hw/xbox/..., b/xemu/hw/xbox/...).

sync   fetches each source at its pin into --cache (a blobless clone per source, reused), writes the listed files
       into external/<source>/, deletes any other file there, applies the patches, and runs the same check. An
       upstream bump is: edit the commit on the @xemu line, run sync, fix what no longer applies or compiles, commit.
check  rebuilds the same tree in a temporary directory and fails unless external/ matches it byte for byte, nothing
       unlisted sits under external/<source>/, and the patches stay within the cap. CI runs it on every push; its
       table is the diff against upstream.
"""
import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

COMMIT = re.compile(r'[0-9a-f]{40}')


def parse_manifest(path):
    sources, cap, current = {}, {'lines': 0, 'files': 0}, None
    for raw in path.read_text().splitlines():
        line = raw.split('#')[0].strip()
        if not line:
            continue
        if line.startswith('@cap'):
            cap.update({k: int(v) for k, v in (kv.split('=') for kv in line.split()[1:])})
        elif line.startswith('@'):
            name, url, rev = line[1:].split()
            if not COMMIT.fullmatch(rev):
                sys.exit(f'{path}: @{name} must pin a full commit id, not "{rev}"')
            current = sources.setdefault(name, {'url': url, 'rev': rev, 'files': []})
        elif current is None:
            sys.exit(f'{path}: path before any @source line: {line}')
        elif line in current['files']:
            sys.exit(f'{path}: {line} listed twice under @{name}')
        else:
            current['files'].append(line)
    return sources, cap


def git(*args, cwd=None, data=None):
    r = subprocess.run(['git', *args], cwd=cwd, input=data, capture_output=True)
    if r.returncode:
        sys.exit(f'git {" ".join(args)}: {r.stderr.decode(errors="replace").strip()}')
    return r.stdout


def fetch(source, cache):
    """A blobless clone holding the pinned commit and the blobs of the listed files; returns the clone dir."""
    d = cache / source['url'].rstrip('/').rsplit('/', 1)[-1]
    if not (d / '.git').exists():
        d.mkdir(parents=True, exist_ok=True)
        git('init', '-q', cwd=d)
        git('remote', 'add', 'origin', source['url'], cwd=d)
        git('config', 'remote.origin.promisor', 'true', cwd=d)
        git('config', 'remote.origin.partialclonefilter', 'blob:none', cwd=d)
    if subprocess.run(['git', 'cat-file', '-e', source['rev'] + '^{commit}'], cwd=d, capture_output=True).returncode:
        git('fetch', '-q', '--filter=blob:none', '--depth', '1', 'origin', source['rev'], cwd=d)
    tree = git('ls-tree', '-r', '-z', source['rev'], '--', *source['files'], cwd=d).decode().split('\0')
    blobs = {e.split('\t', 1)[1]: e.split()[2] for e in tree if e}
    missing = sorted(set(source['files']) - set(blobs))
    if missing:
        sys.exit(f'{source["url"]} {source["rev"]} has no {", ".join(missing)}')
    # One request for every blob, as git's own lazy fetch would make one per file.
    no_lazy = dict(os.environ, GIT_NO_LAZY_FETCH='1')
    absent = [oid for oid in blobs.values()
              if subprocess.run(['git', 'cat-file', '-e', oid], cwd=d, capture_output=True, env=no_lazy).returncode]
    if absent:
        git('-c', 'fetch.negotiationAlgorithm=noop', 'fetch', '-q', 'origin', '--no-tags', '--no-write-fetch-head',
            '--recurse-submodules=no', '--filter=blob:none', '--stdin', cwd=d, data='\n'.join(absent).encode() + b'\n')
    return d, blobs


def read_blobs(clone, oids):
    out = git('cat-file', '--batch', cwd=clone, data=''.join(o + '\n' for o in oids).encode())
    contents, at = [], 0
    for oid in oids:
        header_end = out.index(b'\n', at)
        size = int(out[at:header_end].split()[2])
        contents.append(out[header_end + 1:header_end + 1 + size])
        at = header_end + 1 + size + 1
    return contents


def build_tree(sources, patches_dir, cache, out):
    """Pinned files plus patches, written under out/<source>/. Returns the per-patch budget rows."""
    for name, s in sources.items():
        clone, blobs = fetch(s, cache)
        paths = list(s['files'])
        for path, data in zip(paths, read_blobs(clone, [blobs[p] for p in paths])):
            dest = out / name / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
    rows = []
    for patch in sorted(patches_dir.glob('*.patch')):
        git('apply', '--whitespace=nowarn', str(patch.resolve()), cwd=out)
        text = patch.read_text(errors='replace').splitlines()
        files = {l[6:] for l in text if l.startswith('+++ b/')}
        changed = sum(1 for l in text if l.startswith(('+', '-')) and not l.startswith(('+++', '---')))
        rows.append((patch.name, len(files), changed, files))
    return rows


def compare(sources, expected, actual):
    problems = []
    for name, s in sources.items():
        listed = set(s['files'])
        for f in s['files']:
            a, e = actual / name / f, expected / name / f
            if not a.exists():
                problems.append(f'missing: external/{name}/{f}')
            elif not filecmp.cmp(a, e, shallow=False):
                problems.append(f'differs from pinned upstream + patches: external/{name}/{f}')
        root = actual / name
        if root.exists():
            for p in root.rglob('*'):
                if p.is_file() and p.relative_to(root).as_posix() not in listed:
                    problems.append(f'not in MANIFEST: external/{name}/{p.relative_to(root).as_posix()}')
    return problems


def report(sources, rows, cap):
    total = sum(len(s['files']) for s in sources.values())
    touched = set().union(*(r[3] for r in rows)) if rows else set()
    lines = sum(r[2] for r in rows)
    for name, n_files, changed, _ in rows:
        print(f'  patch {name}: {n_files} file(s), {changed} changed line(s)')
    print(f'vendor: {len(sources)} sources, {total} files, {total - len(touched)} byte-identical to their pin, '
          f'{len(touched)} patched; patch lines {lines}/{cap["lines"]}, patched files {len(touched)}/{cap["files"]}')
    return lines <= cap['lines'] and len(touched) <= cap['files']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mode', choices=['sync', 'check', 'list-c'])
    ap.add_argument('--root', type=Path, default=Path('.'))
    ap.add_argument('--cache', type=Path, default=Path('.xemu-vendor-cache'))
    a = ap.parse_args()
    vendor = a.root / 'external' / 'xemu-vendor'
    sources, cap = parse_manifest(vendor / 'MANIFEST')
    if a.mode == 'list-c':
        for name, s in sources.items():
            for f in s['files']:
                if f.endswith('.c'):
                    print('external\\' + name + '\\' + f.replace('/', '\\'))
        return
    with tempfile.TemporaryDirectory() as tmp:
        expected = Path(tmp)
        rows = build_tree(sources, vendor / 'patches', a.cache.resolve(), expected)
        if a.mode == 'sync':
            for name in sources:
                shutil.rmtree(a.root / 'external' / name, ignore_errors=True)
                shutil.copytree(expected / name, a.root / 'external' / name)
        problems = compare(sources, expected, a.root / 'external')
        within = report(sources, rows, cap)
    for p in problems:
        print(f'FAIL {p}')
    if not within:
        print('FAIL patches exceed the @cap in MANIFEST: upstream the change, or move it into the shim')
    sys.exit(1 if problems or not within else 0)


if __name__ == '__main__':
    main()
