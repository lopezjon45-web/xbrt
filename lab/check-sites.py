#!/usr/bin/env python3
"""check-sites.py <main.c> <default.xbe>: every instruction a lab writes as `.byte` must reach the XBE exactly as
often as the source has it. The compiler may inline, unroll or specialize the function around a site; fs-forms runs
each site twice so the first run can trap and the second take the promoted gate, and a duplicated site runs once.
"""
import collections
import re
import sys

source, xbe = open(sys.argv[1]).read(), open(sys.argv[2], "rb").read()
sites = collections.Counter(bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", m))
                            for m in re.findall(r"\.byte ((?:0x[0-9A-Fa-f]{2}(?:, )?)+)", source))
for site, want in sites.items():
    got = xbe.count(site)
    if got != want:
        sys.exit(f"{sys.argv[1]}: site {site.hex(' ')} is in the source {want}x but in the XBE {got}x")
