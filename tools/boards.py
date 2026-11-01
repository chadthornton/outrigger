"""The board profiles in firmware/boards.json: checks them and generates what
the build needs from them, so a new board is a new profile, not new code.

    python3 -I tools/boards.py check
    python3 -I tools/boards.py ids                  # the Makefile's BOARDS
    python3 -I tools/boards.py fqbn <id>            # arduino-cli's board options
    python3 -I tools/boards.py get <id> <field>     # one value, e.g. site_id
    python3 -I tools/boards.py header <id> <out>    # board_config.h for the firmware
    python3 -I tools/boards.py shots <out>          # the real screens, for make ui-shots

Generated files are rewritten only when their text changes, so arduino-cli and
make don't rebuild for nothing. Tests: tests/test_boards.py.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "firmware/boards.json"

DISPLAYS = {"ili9341", "st7789", "st7796"}
TOUCHES = {"cst816", "ft6336"}
EXPANDERS = {"tca9554"}
SHAPES = {"rect": "Rect", "rounded": "Rounded", "circle": "Circle"}
PSRAM = {"octal": "opi", "quad": "enabled"}

# ESP32-S3 GPIOs that exist, and the ones a board profile may not use.
S3_GPIOS = set(range(0, 22)) | set(range(26, 49))
USB_PINS = {19: "the USB host's D-", 20: "the USB host's D+"}
FLASH_PINS = set(range(26, 33))
OCTAL_PSRAM_PINS = set(range(33, 38))
# ui-shots draws into a 480x480 frame.
MAX_SIDE = 480

REQUIRED = ["name", "short_name", "site_id", "tag_id", "chip", "flash_mb", "psram_mb", "psram", "usb_host",
            "screen.width", "screen.height", "screen.shape", "screen.corner_radius", "screen.font_boost",
            "display.driver", "display.spi_hz", "display.dc", "display.sck", "display.mosi", "display.backlight",
            "display.ips", "display.rotation", "display.offsets",
            "touch.driver", "touch.address", "touch.sda", "touch.scl", "hold_low"]
# Pins a profile names, and whether each may be null.
PINS = [("display.dc", False), ("display.cs", True), ("display.sck", False), ("display.mosi", False),
        ("display.miso", True), ("display.rst", True), ("display.backlight", False),
        ("touch.sda", False), ("touch.scl", False), ("touch.rst", True), ("touch.int", True)]


def load(path=TABLE):
    return json.loads(Path(path).read_text())


def ids(table):
    return list(table["boards"])


def field(board, path, default=KeyError):
    node = board
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            if default is KeyError:
                raise KeyError(path)
            return default
        node = node[part]
    return node


def check(table):
    """Every problem with the table, as sentences; empty when it's good."""
    out = []
    tag_ids, site_ids = {}, {}
    for bid, b in table["boards"].items():
        if not re.fullmatch(r"[a-z][a-z0-9_]*", bid):
            out.append(f"{bid}: board ids are lowercase letters, digits and _ (a make variable and a C name)")
        missing = [p for p in REQUIRED if field(b, p, None) is None]
        if "expander" not in b:
            missing.append("expander (null for none)")
        elif b["expander"] is not None and "address" not in b["expander"]:
            missing.append("expander.address")
        out += [f"{bid}: missing {p}" for p in missing]
        if missing:
            continue
        out += check_board(bid, b)
        if b["tag_id"] in tag_ids:
            out.append(f"{bid}: tag_id {b['tag_id']} is {tag_ids[b['tag_id']]}'s too")
        tag_ids.setdefault(b["tag_id"], bid)
        if b["site_id"] in site_ids:
            out.append(f"{bid}: site_id {b['site_id']} is {site_ids[b['site_id']]}'s too")
        site_ids.setdefault(b["site_id"], bid)
    return out


def check_board(bid, b):
    out = []
    d, t, s = b["display"], b["touch"], b["screen"]
    if d["driver"] not in DISPLAYS:
        out.append(f"{bid}: display driver {d['driver']} isn't one board.cpp drives ({', '.join(sorted(DISPLAYS))})")
    if t["driver"] not in TOUCHES:
        out.append(f"{bid}: touch driver {t['driver']} isn't one board.cpp drives ({', '.join(sorted(TOUCHES))})")
    if b["expander"] is not None and b["expander"].get("driver") not in EXPANDERS:
        out.append(f"{bid}: expander {b['expander'].get('driver')} isn't one board.cpp drives")
    for where, chip in (("touch.address", t), ("expander.address", b["expander"])):
        if chip is not None and not (isinstance(chip["address"], int) and 0x08 <= chip["address"] <= 0x77):
            out.append(f"{bid}: {where} {chip['address']} isn't a 7-bit I2C address (0x08-0x77)")
    if not isinstance(b.get("tested", False), bool):
        out.append(f"{bid}: tested is true or false, not {b['tested']!r}")
    if b["chip"] != "ESP32-S3":
        out.append(f"{bid}: chip {b['chip']}: only ESP32-S3 builds exist so far")
    if b["flash_mb"] != 16:
        out.append(f"{bid}: the store's layout (partitions.csv) needs 16 MB of flash, not {b['flash_mb']}")
    if b["psram_mb"] < 8:
        out.append(f"{bid}: the loader needs 8 MB of PSRAM, not {b['psram_mb']}")
    if b["psram"] not in PSRAM:
        out.append(f"{bid}: psram is octal or quad, not {b['psram']}")
    if s["shape"] not in SHAPES:
        out.append(f"{bid}: screen shape {s['shape']} isn't one of {', '.join(SHAPES)}")
    if s["shape"] == "rounded" and s["corner_radius"] <= 0:
        out.append(f"{bid}: a rounded screen needs its corner_radius")
    if not (0 < s["width"] <= MAX_SIDE and 0 < s["height"] <= MAX_SIDE):
        out.append(f"{bid}: screens up to {MAX_SIDE}x{MAX_SIDE} so far (ui-shots), not {s['width']}x{s['height']}")
    if not (isinstance(d["offsets"], list) and len(d["offsets"]) == 4):
        out.append(f"{bid}: display.offsets is four numbers (Arduino_GFX's col1, row1, col2, row2)")

    used = {}

    def use(where, pin, optional):
        if pin is None:
            if not optional:
                out.append(f"{bid}: {where} needs a pin")
            return
        if isinstance(pin, str):
            m = re.fullmatch(r"exio([0-7])", pin)
            if not m:
                out.append(f"{bid}: {where} is {pin}: a pin is a GPIO number or exio0-exio7")
                return
            if b["expander"] is None:
                out.append(f"{bid}: {where} is on {pin}, but the board has no expander")
        elif isinstance(pin, bool) or not isinstance(pin, int) or pin not in S3_GPIOS:
            out.append(f"{bid}: {where} is on GPIO{pin}, which the ESP32-S3 doesn't have")
            return
        else:
            if pin in USB_PINS:
                out.append(f"{bid}: {where} is on GPIO{pin}, {USB_PINS[pin]}")
            if pin in FLASH_PINS:
                out.append(f"{bid}: {where} is on GPIO{pin}, a flash line")
            if pin in OCTAL_PSRAM_PINS and b["psram"] == "octal":
                out.append(f"{bid}: {where} is on GPIO{pin}, an octal-PSRAM line")
        name = pin if isinstance(pin, str) else f"GPIO{pin}"
        if name in used:
            out.append(f"{bid}: {name} is both {used[name]} and {where}")
        used.setdefault(name, where)

    for path, optional in PINS:
        use(path, field(b, path, None), optional)
    for key in ("hold_low", "hold_high"):
        for i, pin in enumerate(b.get(key, [])):
            use(f"{key}[{i}]", pin, False)
    return out


def checked(table):
    problems = check(table)
    if problems:
        sys.exit("firmware/boards.json:\n  " + "\n  ".join(problems))
    return table


def fqbn(b):
    return f"esp32:esp32:esp32s3:USBMode=default,FlashSize={b['flash_mb']}M,PSRAM={PSRAM[b['psram']]}"


def pin_code(pin):
    """The firmware's encoding: GPIO n is n, expander pin n is 100 + n, none is -1."""
    if pin is None:
        return -1
    if isinstance(pin, str):
        return 100 + int(pin[4:])
    return pin


def profile_init(b):
    s = b["screen"]
    # json.dumps quotes and escapes the way a C string literal needs (the names are ASCII).
    return (f'{{{json.dumps(b["name"])}, {s["width"]}, {s["height"]}, ScreenShape::{SHAPES[s["shape"]]}, '
            f'{s["corner_radius"]}, {s["font_boost"]}}}')


def header(bid, b):
    d, t, e = b["display"], b["touch"], b["expander"]
    pins = lambda *names: ", ".join(f"{n} = {pin_code(v)}" for n, v in names)  # noqa: E731
    hold = lambda key: ", ".join(str(pin_code(p)) for p in b.get(key, [])) or "-1"  # noqa: E731
    lines = [
        f"// Generated by tools/boards.py from firmware/boards.json ({bid}). Don't edit: change the profile.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define BOARD_DISPLAY_{d['driver'].upper()} 1",
        f"#define BOARD_TOUCH_{t['driver'].upper()} 1",
        *([f"#define BOARD_EXPANDER_{e['driver'].upper()} 1"] if e else []),
        f"#define BOARD_TAG_ID {b['tag_id']}",
        f"#define BOARD_PROFILE_INIT {profile_init(b)}",
        "",
        "namespace board_config {",
        "// Pins: GPIO n is n, expander pin n is 100 + n, none is -1.",
        f"constexpr int16_t {pins(('kLcdDc', d['dc']), ('kLcdCs', d.get('cs')), ('kLcdSck', d['sck']), ('kLcdMosi', d['mosi']))};",
        f"constexpr int16_t {pins(('kLcdMiso', d.get('miso')), ('kLcdRst', d.get('rst')), ('kLcdBl', d['backlight']))};",
        f"constexpr uint32_t kSpiHz = {d['spi_hz']};",
        f"constexpr bool kIps = {'true' if d['ips'] else 'false'};",
        f"constexpr uint8_t kRotation = {d['rotation']};",
        f"constexpr int16_t kOffsets[4] = {{{', '.join(str(o) for o in d['offsets'])}}};",
        f"constexpr int16_t {pins(('kTpSda', t['sda']), ('kTpScl', t['scl']), ('kTpRst', t.get('rst')), ('kTpInt', t.get('int')))};",
        f"constexpr uint8_t kTouchAddress = {t['address']:#04x};",
        *([f"constexpr uint8_t kExpanderAddress = {e['address']:#04x};"] if e else []),
        f"constexpr int16_t kHoldLow[] = {{{hold('hold_low')}}};",
        f"constexpr int16_t kHoldHigh[] = {{{hold('hold_high')}}};",
        "}  // namespace board_config",
        "",
    ]
    return "\n".join(lines)


def shots_table(table):
    """The real boards' screens as ui_shots.cpp's SimBoard entries, one per distinct screen. Each id names a folder
    and a sheet, so a second screen at one resolution gets its radius (and boost) added."""
    seen, ids, rows = set(), set(), []
    for b in table["boards"].values():
        s = b["screen"]
        key = (s["width"], s["height"], s["corner_radius"], s["font_boost"])
        if key in seen:
            continue
        seen.add(key)
        sid = f'{s["width"]}x{s["height"]}'
        for extra in (f'-r{s["corner_radius"]}', f'-b{s["font_boost"]}'):
            if sid in ids:
                sid += extra
        ids.add(sid)
        rows.append(f'{{"{sid}", {s["width"]}, {s["height"]}, {s["corner_radius"]}, '
                    f'{s["font_boost"]}, InputKind::Touch}}')
    return ("// Generated by tools/boards.py from firmware/boards.json: every board's screen, in table order.\n"
            "#define BOARD_SHOTS \\\n  " + ", \\\n  ".join(rows) + "\n")


def write_if_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text() == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return True


def main(argv):
    table = checked(load())
    cmd, args = (argv[0], argv[1:]) if argv else ("check", [])
    board = lambda bid: table["boards"].get(bid) or sys.exit(f"unknown board {bid}; known: {' '.join(ids(table))}")  # noqa: E731
    if cmd == "check":
        print(f"boards: {len(table['boards'])} profiles, all good")
    elif cmd == "ids":
        print(" ".join(ids(table)))
    elif cmd == "fqbn":
        print(fqbn(board(args[0])))
    elif cmd == "get":
        print(field(board(args[0]), args[1]))
    elif cmd == "header":
        write_if_changed(args[1], header(args[0], board(args[0])))
    elif cmd == "shots":
        write_if_changed(args[0], shots_table(table))
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
