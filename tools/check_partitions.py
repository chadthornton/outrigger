"""Checks a sketch's partitions.csv against the layout every install keeps
(build_site.check_table): NVS and otadata unchanged, app0 at 0x10000 with 3 MB,
the store clear of everything else. Run by `make test`.

Usage: python3 tools/check_partitions.py firmware/<board>/partitions.csv
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_site import check_table  # noqa: E402

FLASH_BYTES = 16 * 1024 * 1024


def read_csv(path):
    table = {}
    for line in Path(path).read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        name, _type, _sub, off, size = [f.strip() for f in line.split(",")[:5]]
        table[name] = (int(off, 0), int(size, 0))
    return table


def main():
    table = read_csv(sys.argv[1])
    problems = check_table(table, FLASH_BYTES)
    # A table that breaks the rules must be refused (the checker itself is tested).
    broken = dict(table, nvs=(0x9000, 0x6000))
    if not check_table(broken, FLASH_BYTES):
        problems.append("check_table accepted a moved NVS")
    overlapping = dict(table, store=(table["app0"][0] + 0x1000, 0x1000))
    if not check_table(overlapping, FLASH_BYTES):
        problems.append("check_table accepted a store overlapping app0")
    for p in problems:
        print("partitions: " + p)
    if problems:
        return 1
    print("partitions: %s keeps the install layout (store %s)" % (sys.argv[1], table.get("store")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
