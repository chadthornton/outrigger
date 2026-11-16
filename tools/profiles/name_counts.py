"""Rank normalised parameter names across built profiles (evidence for standard-parameters.json).

    python3 -I tools/profiles/name_counts.py build/profiles/midi-guide [TOP]

Prints the profiles' total JSON size, then per name (lower-cased, numbers as #) the number of devices using it
and its two most common CCs.
"""
import collections
import json
import re
import sys
from pathlib import Path

root, top = Path(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 120
size, devices, ccs = 0, collections.defaultdict(set), collections.defaultdict(collections.Counter)
files = [p for p in root.rglob("*.json") if p.name != "report.json"]
for p in files:
    data = p.read_bytes()
    size += len(data)
    prof = json.loads(data)
    for s in prof["sections"]:
        for q in s["parameters"]:
            n = re.sub(r"\s+", " ", re.sub(r"\b\d+\b", "#", q["name"].lower())).strip()
            devices[n].add(prof["id"])
            if q["cc"]:
                ccs[n][q["cc"]["msb"]] += 1
print(f"profiles {len(files)} bytes {size}")
for n, ids in sorted(devices.items(), key=lambda x: (-len(x[1]), x[0]))[:top]:
    print(len(ids), repr(n), ccs[n].most_common(2))
