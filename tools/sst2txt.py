#!/usr/bin/env python3
"""Convert SingleStepTests/680x0 JSON vectors into the flat text format that
tests/cputest reads.  Usage: sst2txt.py <dir-with-json.gz> <tests-per-file>"""
import gzip, json, sys, os

REGS = ["d0","d1","d2","d3","d4","d5","d6","d7","a0","a1","a2","a3","a4","a5","a6","usp","ssp","sr","pc"]

def state(s):
    out = [str(s[r]) for r in REGS]
    out += [str(s["prefetch"][0]), str(s["prefetch"][1]), str(len(s["ram"]))]
    for a, v in s["ram"]:
        out += [str(a), str(v)]
    return out

d, limit = sys.argv[1], int(sys.argv[2])
for fn in sorted(os.listdir(d)):
    if not fn.endswith(".json.gz"):
        continue
    tests = json.load(gzip.open(os.path.join(d, fn)))
    for t in tests[:limit]:
        name = t["name"].replace(" ", "_")
        print(" ".join([fn[:-8], name, str(t["length"])] + state(t["initial"]) + state(t["final"])))
