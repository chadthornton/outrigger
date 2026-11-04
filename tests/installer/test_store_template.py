"""The store template the installer writes onto a factory-blank store
(tools/store_template.py): with JieLi's loader and the starter set's packages
spliced in, it must mount as the board's littlefs does and hold the loader and
every package exactly as `make store-add` would store them; without them, it
must hold none of their bytes.

Needs littlefs-python and the fixtures: `make test` runs it with
build/tools-venv/bin/python. Writes build/store-template.json,
build/store-spliced.sha256 and build/starter-apps.json for site/installer's
tests (store.test.mjs).
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import fm1lib  # noqa: E402
from store_image import BLOB, BLOB_SHA256, BLOCK, mount, store_partition  # noqa: E402
from store_template import LOADER_PATH, build_template, read_inputs, splice, write_runs  # noqa: E402

PARTITIONS = ROOT / "firmware/waveshare_169/partitions.csv"
FIXTURES = ROOT / "tests/fixtures"

# What the catalog lists for each (thefirmwarehub.com/catalog.json, 2026-10-31),
# named the way tools/store_add.py names them.
EXPECTED = [
    ("stock-V15", "M-VAVE stock", "V15", "stock", "FM-1_015", "https://thefirmwarehub.com/fw/mvave/FM-1_015.fwsc"),
]


def files(fs, path=""):
    out = {}
    for entry in fs.scandir(path or "/"):
        p = (path + "/" + entry.name).lstrip("/")
        if entry.type == 2:
            out.update(files(fs, p))
        else:
            with fs.open(p, "rb") as f:
                out[p] = f.read()
    return out


def chunks(data):
    """64-byte pieces of data, leaving out the all-0xFF and all-zero ones any image has."""
    for i in range(0, len(data) - 63, 64):
        c = data[i:i + 64]
        if c.count(c[0]) != 64:
            yield c


def main():
    failures = []
    _, size = store_partition(PARTITIONS)
    blob, packages = read_inputs(FIXTURES)
    assert hashlib.sha256(blob).hexdigest() == BLOB_SHA256, "run `make fixtures`"
    template = build_template(size, blob, packages)

    if template["size"] != size or template["blob_size"] != len(blob):
        failures.append(f"template is for {template['size']} bytes / a {template['blob_size']}-byte loader")
    if template["blank_md5"] != hashlib.md5(b"\xff" * size).hexdigest():
        failures.append("blank_md5 isn't the MD5 of an erased store")
    got = [(p["id"], p["name"], p["version"], p["project"], p["identity"], p["url"]) for p in template["packages"]]
    if got != EXPECTED:
        failures.append(f"the starter set is {got}")
    for p in template["packages"]:
        data = packages[p["id"]]
        if p["sha256"] != hashlib.sha256(data).hexdigest():
            failures.append(f"{p['id']}: sha256 isn't the file's")
        if p["app_sha256"] != hashlib.sha256(fm1lib.parse_package(data)[1]).hexdigest():
            failures.append(f"{p['id']}: app_sha256 isn't its app's")

    for b in template["blocks"]:
        if b["offset"] % BLOCK or b["offset"] + BLOCK > size or ("hex" in b) == ("sector" in b):
            failures.append(f"block {b['offset']:#x} is misaligned, past the store, or not exactly one of hex/sector")
        elif "hex" in b and len(b["hex"]) > 2 * BLOCK:
            failures.append(f"block {b['offset']:#x} has more than a block of hex")

    # Nothing of JieLi's or any firmware author's in what the site publishes.
    published = b"".join(bytes.fromhex(b["hex"]) for b in template["blocks"] if "hex" in b)
    for name, data in [("wl82loader.bin", blob)] + [(k, fm1lib.parse_package(v)[1]) for k, v in packages.items()]:
        if any(c in published for c in chunks(data)):
            failures.append(f"the template contains part of {name}")
    text = json.dumps(template)
    if len(text) > 3_000_000:
        failures.append(f"the template is {len(text)} bytes of JSON")

    # Spliced onto an erased store, it mounts and holds exactly what store-add would.
    spliced = splice(template, blob, packages)
    image = bytearray(b"\xff" * size)
    for off, data in spliced:
        image[off:off + len(data)] = data
    mounted = mount(bytes(image))
    if not mounted:
        failures.append("the spliced store doesn't mount")
    else:
        tree = files(mounted[0])
        if tree.get(LOADER_PATH) != blob:
            failures.append("the spliced store's loader isn't wl82loader.bin")
        shutil.rmtree(ROOT / "build/starter-expected", ignore_errors=True)
        expected = fm1lib.DirStore(ROOT / "build/starter-expected")
        for pkg_id, name, version, project, _, _ in EXPECTED:
            fm1lib.add_package(expected, pkg_id, packages[pkg_id], name=name, version=version, project=project)
        want = {str(p.relative_to(expected.root)): p.read_bytes() for p in expected.root.rglob("*") if p.is_file()}
        want[LOADER_PATH] = blob
        if set(tree) != set(want):
            failures.append(f"files differ from store-add's: {sorted(set(tree) ^ set(want))[:4]}")
        for key in sorted(set(tree) & set(want)):
            if tree[key] != want[key]:
                failures.append(f"{key} isn't what store-add writes")
                break
        bad = [k for k in tree if k.startswith("s/") and fm1lib.sector_key(hashlib.sha256(tree[k]).digest()) != k]
        if bad:
            failures.append(f"{len(bad)} sector files don't hash to their names ({bad[0]})")

    # The writes: whole used blocks in runs, every block once, the superblock pair last.
    runs = write_runs(template, spliced)
    if [(r["offset"], r["size"]) for r in template["runs"]] != [(off, len(d)) for off, d in runs]:
        failures.append("write_runs doesn't follow the template's runs")
    if runs and runs[-1][0] != 0:
        failures.append("the superblock pair isn't written last")
    if runs and len(runs[-1][1]) != 2 * BLOCK:
        failures.append("the last run isn't just blocks 0 and 1")
    covered = sorted(off + i for off, d in runs for i in range(0, len(d), BLOCK))
    if covered != [off for off, _ in spliced]:
        failures.append("the runs don't cover the used blocks exactly once")
    for r, (off, data) in zip(template["runs"], runs):
        if r["md5"] != hashlib.md5(data).hexdigest():
            failures.append(f"run {off:#x}: md5 isn't its data's")
        if r.get("blocks") != [hashlib.md5(data[i:i + BLOCK]).hexdigest() for i in range(0, len(data), BLOCK)]:
            failures.append(f"run {off:#x}: its block MD5s aren't its blocks'")
        if r.get("blank_md5") != hashlib.md5(b"\xff" * r["size"]).hexdigest():
            failures.append(f"run {off:#x}: blank_md5 isn't an erased run's")
    if template.get("blank_block_md5") != hashlib.md5(b"\xff" * BLOCK).hexdigest():
        failures.append("blank_block_md5 isn't an erased block's")
    # The gaps and the runs tile the store, so a check of both covers every byte (install.js unfinished).
    pieces = sorted([(r["offset"], r["size"]) for r in template["runs"]] + [(g["offset"], g["size"]) for g in template.get("gaps", [])])
    if not pieces or pieces[0][0] != 0 or sum(n for _, n in pieces) != size or any(a + n != b for (a, n), (b, _) in zip(pieces, pieces[1:])):
        failures.append("the runs and gaps don't tile the store")
    for g in template.get("gaps", []):
        if g["size"] <= 0 or g["blank_md5"] != hashlib.md5(b"\xff" * g["size"]).hexdigest():
            failures.append(f"gap {g['offset']:#x}: empty, or blank_md5 isn't an erased gap's")
    for off, data in runs:
        if image[off:off + len(data)] != data:
            failures.append(f"run {off:#x} isn't the spliced image")

    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    (out / "store-template.json").write_text(text)
    digest = hashlib.sha256(b"".join(off.to_bytes(4, "little") + data for off, data in spliced)).hexdigest()
    if template["sha256"] != digest:
        failures.append("the template's sha256 isn't the spliced store's")
    (out / "store-spliced.sha256").write_text(digest + "\n")
    # For the JS port of parse_package (store.test.mjs): every package fixture's identity and app hash.
    apps = {}
    for p in sorted(FIXTURES.glob("*.fwsc")):
        ident, app = fm1lib.parse_package(p.read_bytes())
        apps[p.name] = [ident, hashlib.sha256(app).hexdigest()]
    (out / "starter-apps.json").write_text(json.dumps(apps))

    for f in failures:
        print("store template: " + f)
    if failures:
        return 1
    print(f"store template: {len(template['blocks'])} blocks in {len(runs)} runs, {len(text)} bytes of JSON; "
          f"spliced store mounts and holds the loader and {len(packages)} packages as store-add writes them")
    return 0


if __name__ == "__main__":
    sys.exit(main())
