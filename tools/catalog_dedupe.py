"""How much do FM-1 firmware packages share? Measures 4 KiB sectors of each
package's flash image (the part a ROM-boot installer writes).
Parsing follows Quixotic7's fm1_unbrick.py (parse_fwsc, ufw_flash)."""
import collections
import hashlib
import sys
import zlib
from pathlib import Path

SECTOR = 0x1000
HEAD_END, APP_END = 0x4000, 0x93000
FWSC_BLOCK, FWSC_MARKED, FWSC_NO_CHAR = 0x2F, 20, 0x7D


def parse_fwsc(data):
    stride = FWSC_BLOCK + 1
    image, chars = bytearray(), []
    for i in range(FWSC_MARKED):
        block = data[i * stride:(i + 1) * stride]
        image += block[:FWSC_BLOCK]
        if block[FWSC_BLOCK] != FWSC_NO_CHAR:
            chars.append(chr((block[FWSC_BLOCK] - i - 1) & 0xFF))
    image += data[stride * FWSC_MARKED:]
    return "".join(chars), bytes(image)


def crc16(data, c=0):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def jl_enc(buf):
    out, k = bytearray(buf), 0xFFFF
    for i in range(len(out)):
        out[i] ^= k & 0xFF
        k = ((k << 1) ^ (0x1021 if k & 0x8000 else 0)) & 0xFFFF
    return bytes(out)


def u16(p, o): return p[o] | p[o + 1] << 8
def u32(p, o): return u16(p, o) | u16(p, o + 2) << 16


def flash_bin(image):
    hdr = jl_enc(image[:0x40])
    assert crc16(hdr[2:0x40]) == u16(hdr, 0), "UFW header CRC"
    n = u16(hdr, 8)
    for i in range(n):
        e = jl_enc(image[0x40 + i * 0x50:0x90 + i * 0x50])
        if u16(e, 0) == 0:
            off, length = u32(e, 8), u32(e, 12)
            assert length >= APP_END
            return image[off:off + APP_END]
    raise ValueError("no flash.bin")


pkgs = sorted(Path(sys.argv[1]).glob("*.fwsc"))
sector_users = collections.defaultdict(set)     # sector hash -> packages using it
sector_bytes = {}
per_pkg = {}
blank = b"\xff" * SECTOR
idents = {}
for p in pkgs:
    ident, image = parse_fwsc(p.read_bytes())
    fl = flash_bin(image)
    idents[p.stem] = ident
    hashes = []
    for a in range(HEAD_END, APP_END, SECTOR):
        s = fl[a:a + SECTOR]
        if s == blank:
            hashes.append(None)
            continue
        h = hashlib.sha256(s).digest()
        sector_users[h].add(p.stem)
        sector_bytes[h] = s
        hashes.append(h)
    per_pkg[p.stem] = hashes

n = len(pkgs)
app_sectors = (APP_END - HEAD_END) // SECTOR
total = n * app_sectors
nonblank = sum(1 for hs in per_pkg.values() for h in hs if h)
unique = len(sector_users)
mb = lambda sectors: sectors * SECTOR / 1048576
print(f"packages: {n}, app area: {app_sectors} sectors ({mb(app_sectors):.2f} MB) each")
print(f"stored as-is (app areas): {mb(total):.1f} MB; non-blank: {mb(nonblank):.1f} MB")
print(f"unique non-blank sectors: {unique} = {mb(unique):.1f} MB  (dedupe ratio {nonblank / unique:.2f}x)")
shared = sum(1 for u in sector_users.values() if len(u) > 1)
print(f"sectors shared by 2+ packages: {shared} ({100 * shared / unique:.1f}% of unique)")

# What each package costs on top of the others: sectors only it uses.
own = {k: sum(1 for h in set(hs) if h and len(sector_users[h]) == 1) for k, hs in per_pkg.items()}
costs = sorted(own.values())
print(f"sectors only one package uses: median {costs[n // 2]} ({mb(costs[n // 2]) * 1024:.0f} KB), "
      f"min {costs[0]}, max {costs[-1]}")

# Compression of the unique sectors, to see whether encryption defeats it.
raw = b"".join(sector_bytes.values())
print(f"zlib-9 of all unique sectors: {len(zlib.compress(raw, 9)) / len(raw):.2f} of original")

# Family view: identity prefixes and how much a typical pair of same-family packages shares.
fam = collections.defaultdict(list)
for k, ident in idents.items():
    fam[ident[:8]].append(k)
print("identity groups (first 8 chars):",
      ", ".join(f"{g}:{len(v)}" for g, v in sorted(fam.items(), key=lambda x: -len(x[1]))[:8]))

# How many packages fit in a given budget, packing the cheapest additions greedily.
for budget_mb in (6.4, 8.4, 10.0):
    budget = int(budget_mb * 1048576 / SECTOR)
    have, count = set(), 0
    remaining = dict(per_pkg)
    while remaining:
        best = min(remaining, key=lambda k: sum(1 for h in set(remaining[k]) if h and h not in have))
        cost = sum(1 for h in set(remaining[best]) if h and h not in have)
        if len(have) + cost > budget:
            break
        have |= {h for h in remaining.pop(best) if h}
        count += 1
    print(f"budget {budget_mb} MB: about {count} packages fit with sharing (vs {int(budget_mb / mb(app_sectors))} without)")

# Realistic: packages picked in random order, not cheapest-first.
import random
for budget_mb in (6.4, 8.4, 10.0):
    budget = int(budget_mb * 1048576 / SECTOR)
    counts = []
    for trial in range(300):
        order = list(per_pkg)
        random.Random(trial).shuffle(order)
        have, count = set(), 0
        for k in order:
            new = {h for h in per_pkg[k] if h} - have
            if len(have) + len(new) > budget:
                break
            have |= new
            count += 1
        counts.append(count)
    counts.sort()
    print(f"budget {budget_mb} MB, random picks: median {counts[150]} packages (10th-90th pct {counts[30]}-{counts[270]})")
top = sorted(sector_users.values(), key=len, reverse=True)[:5]
print("most-shared sectors are used by", [len(u) for u in top], "packages")
