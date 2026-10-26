#!/usr/bin/env python3
"""Packs the pipeline corpus that xemu's Vulkan renderer recorded (lopezjon45-web/xemu branch m0-corpus) into the
bundle host/m0/corpus.cpp reads, dropping repeated pipelines.

    pack-corpus.py <corpus dir> <out bundle>

The corpus dir holds pipelines.jsonl (one JSON object per created pipeline) and shaders/<sha256>.<vert|geom|frag>.
The bundle is text:

    xbrt-m0-corpus 1
    shader <sha256> <v|g|f> <byte count>
    <the GLSL, exactly that many bytes>
    ...
    pipeline <vs sha256> <gs sha256 or -> <ps sha256> key=value ...

Pipeline keys are the Vulkan values xemu created the pipeline with; host/m0 maps them to D3D. Prints a summary.
"""
import json
import sys
from pathlib import Path

STAGES = {"vert": "v", "geom": "g", "frag": "f"}


def flat(p):
    t = [
        f"topology={p['topology']}",
        f"polygon={p['raster']['polygon_mode']}",
        f"cull={p['raster']['cull_mode']}",
        f"front={p['raster']['front_face']}",
        f"clamp={p['raster']['depth_clamp']}",
        f"color={p['color_format']}",
        f"zeta={p['zeta_format']}",
        f"push={p['push_constant_bytes']}",
    ]
    d = p["depth"]
    if d is not None:
        t += [f"dtest={d['test']}", f"dwrite={d['write']}", f"dcmp={d['compare']}", f"stencil={d['stencil']}"]
        for side in ("front", "back"):
            s = d[side]
            t.append(f"s{side[0]}={s['fail']}:{s['pass']}:{s['depth_fail']}:{s['compare']}:"
                     f"{s['compare_mask']}:{s['write_mask']}:{s['reference']}")
    b = p["blend"]
    if b is not None:
        t += [f"blend={b['enable']}:{b['src_color']}:{b['dst_color']}:{b['color_op']}:"
              f"{b['src_alpha']}:{b['dst_alpha']}:{b['alpha_op']}",
              f"wmask={b['write_mask']}",
              "bconst=" + ":".join(repr(float(c)) for c in b["constants"])]
    t += [f"bind={x['binding']}:{x['stride']}:{x['rate']}" for x in p["bindings"]]
    t += [f"attr={x['location']}:{x['binding']}:{x['format']}:{x['offset']}" for x in p["attributes"]]
    return t


def main(src, out):
    src = Path(src)
    shaders = {}
    for f in sorted((src / "shaders").iterdir()):
        h, ext = f.name.split(".")
        shaders[h] = (STAGES[ext], f.read_bytes())
    seen, pipelines = set(), []
    records = 0
    for line in (src / "pipelines.jsonl").read_text().splitlines():
        if not line.strip():
            continue
        records += 1
        p = json.loads(line)
        row = " ".join([p["vs"], p["gs"] or "-", p["ps"]] + flat(p))
        if row not in seen:
            seen.add(row)
            pipelines.append(row)
    used = {h for row in pipelines for h in row.split()[:3] if h != "-"}
    missing = used - shaders.keys()
    if missing:
        sys.exit(f"{len(missing)} shaders named by pipelines have no file, e.g. {sorted(missing)[0]}")
    with open(out, "wb") as o:
        o.write(b"xbrt-m0-corpus 1\n")
        for h in sorted(used):
            stage, text = shaders[h]
            o.write(f"shader {h} {stage} {len(text)}\n".encode())
            o.write(text + b"\n")
        for row in pipelines:
            o.write(f"pipeline {row}\n".encode())
    per_stage = {s: sum(1 for h in used if shaders[h][0] == s) for s in "vgf"}
    print(f"records {records}, pipelines {len(pipelines)}, shaders {len(used)} "
          f"(vertex {per_stage['v']}, geometry {per_stage['g']}, pixel {per_stage['f']}), "
          f"GLSL bytes {sum(len(shaders[h][1]) for h in used)}, bundle {Path(out).stat().st_size} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
