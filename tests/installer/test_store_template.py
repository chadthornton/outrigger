"""The store template the installer writes onto a factory-blank store
(tools/store_template.py): with JieLi's loader spliced in at its extents it
must mount as the board's littlefs does and hold the loader byte for byte,
and without it, it must hold none of the loader's bytes.

Needs littlefs-python and the fixtures: `make test` runs it with
build/tools-venv/bin/python. Writes build/store-template.json and
build/store-spliced.sha256 for site/installer's test (store.test.mjs).
"""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from store_image import BLOB, BLOB_SHA256, BLOCK, mount, store_partition  # noqa: E402
from store_template import LOADER_PATH, build_template, splice  # noqa: E402

PARTITIONS = ROOT / "firmware/waveshare_169/partitions.csv"


def main():
    failures = []
    _, size = store_partition(PARTITIONS)
    template = build_template(size)
    blob = BLOB.read_bytes()
    assert hashlib.sha256(blob).hexdigest() == BLOB_SHA256, "run `make fixtures`"

    if template["size"] != size or template["blob_size"] != len(blob):
        failures.append(f"template is for {template['size']} bytes / a {template['blob_size']}-byte loader")
    if template["blank_md5"] != hashlib.md5(b"\xff" * size).hexdigest():
        failures.append("blank_md5 isn't the MD5 of an erased store")
    blocks = [(b["offset"], bytes.fromhex(b["hex"])) for b in template["blocks"]]
    if any(off % BLOCK or len(data) != BLOCK or off + BLOCK > size for off, data in blocks):
        failures.append("a template block is misaligned, short, or past the store")
    if len(blocks) > 16:
        failures.append(f"{len(blocks)} blocks: the template should touch only the few the file system uses")

    # The extents cover the loader exactly once, and every one lies inside a template block.
    covered = sorted((b, b + n) for _, b, n in template["extents"])
    if covered[0][0] != 0 or covered[-1][1] != len(blob) or any(a[1] != b[0] for a, b in zip(covered, covered[1:])):
        failures.append(f"extents don't tile the loader: {covered}")
    starts = {off for off, _ in blocks}
    for img, _, n in template["extents"]:
        if img - img % BLOCK not in starts or (img + n - 1) - (img + n - 1) % BLOCK not in starts:
            failures.append(f"extent at {img:#x} isn't inside a template block")

    # No JieLi bytes in what the site publishes.
    published = b"".join(data for _, data in blocks)
    if any(blob[i:i + 64] in published for i in range(0, len(blob) - 64, 64)):
        failures.append("the template contains part of wl82loader.bin")

    # Spliced onto an erased store, it mounts and holds the loader.
    spliced = splice(template, blob)
    image = bytearray(b"\xff" * size)
    for off, data in spliced:
        image[off:off + len(data)] = data
    mounted = mount(bytes(image))
    if not mounted:
        failures.append("the spliced store doesn't mount")
    else:
        with mounted[0].open(LOADER_PATH, "rb") as f:
            if f.read() != blob:
                failures.append("the spliced store's loader isn't wl82loader.bin")

    out = ROOT / "build"
    out.mkdir(exist_ok=True)
    (out / "store-template.json").write_text(json.dumps(template))
    digest = hashlib.sha256(b"".join(off.to_bytes(4, "little") + data for off, data in spliced)).hexdigest()
    (out / "store-spliced.sha256").write_text(digest + "\n")

    for f in failures:
        print("store template: " + f)
    if failures:
        return 1
    print(f"store template: {len(blocks)} blocks, {len(template['extents'])} extents; spliced store mounts and holds the loader")
    return 0


if __name__ == "__main__":
    sys.exit(main())
