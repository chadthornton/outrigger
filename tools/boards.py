"""The board profiles in firmware/boards.json: checks them and generates what
the build needs from them, so a new board is a new profile, not new code.

    python3 -I tools/boards.py check
    python3 -I tools/boards.py ids                  # the Makefile's BOARDS: tested boards, the ones built and installed
    python3 -I tools/boards.py all-ids              # every profile, tested or not
    python3 -I tools/boards.py fqbn <id>            # arduino-cli's board options
    python3 -I tools/boards.py get <id> <field>     # one value, e.g. site_id
    python3 -I tools/boards.py header <id> <out>    # board_config.h for the firmware
    python3 -I tools/boards.py shots <out>          # the real screens, for make ui-shots
    python3 -I tools/boards.py blob <id>            # the profile as the board stores it, in hex
    python3 -I tools/boards.py blob-cases           # tests/boards/blobs.tsv

Generated files are rewritten only when their text changes, so arduino-cli and
make don't rebuild for nothing. Tests: tests/test_boards.py.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "firmware/boards.json"

# The drivers board.cpp has, and their names in board_config_types.h.
PANEL_ENUM = {"ili9341": "ILI9341", "st7789": "ST7789", "st7796": "ST7796"}
TOUCH_ENUM = {"cst816": "CST816", "ft6336": "FT6336"}
DISPLAYS = set(PANEL_ENUM)
TOUCHES = set(TOUCH_ENUM)
EXPANDERS = {"tca9554"}
SHAPES = {"rect": "Rect", "rounded": "Rounded", "circle": "Circle"}
PSRAM = {"octal": "opi", "quad": "enabled"}

# ESP32-S3 GPIOs that exist, and the ones a board profile may not use.
S3_GPIOS = set(range(0, 22)) | set(range(26, 49))
USB_PINS = {19: "the USB host's D-", 20: "the USB host's D+"}
FLASH_PINS = set(range(26, 33))
OCTAL_PSRAM_PINS = set(range(33, 38))
# Optional fields, true or false; left out means the driver's own behaviour
# (board_config_types.h: -1).
YES_NO_DISPLAY = ("bgr", "mirror_x", "mirror_y")
YES_NO_TOUCH = ("swap_xy", "invert_x", "invert_y")
MAX_HOLD = 4
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


def all_ids(table):
    return list(table["boards"])


def ids(table):
    """The boards CI, make site and the installer build: only ones someone has run Outrigger on (tested: true).
    An untested profile still builds and probes by name (make build BOARD=...)."""
    return [bid for bid, b in table["boards"].items() if b.get("tested", False)]


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
    if "spi_mode" in d and d["spi_mode"] not in (0, 1, 2, 3):
        out.append(f"{bid}: display.spi_mode is 0-3 (board.cpp sets it on the SPI bus, for any panel)")
    for where, part, keys in (("display", d, YES_NO_DISPLAY), ("touch", t, YES_NO_TOUCH)):
        for k in keys:
            if k in part and not isinstance(part[k], bool):
                out.append(f"{bid}: {where}.{k} is true or false, or left out for the driver's own")
    for key in ("hold_low", "hold_high"):
        if len(b.get(key, [])) > MAX_HOLD:
            out.append(f"{bid}: {key} holds at most {MAX_HOLD} pins (board_config_types.h kMaxHoldPins)")
    if not isinstance(b.get("restart_on_power_on", False), bool):
        out.append(f"{bid}: restart_on_power_on is true or false")
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
    if d["rotation"] not in (0, 1, 2, 3):
        out.append(f"{bid}: display.rotation is 0-3 (Arduino_GFX's setRotation)")
    if not (isinstance(s["font_boost"], int) and 0 <= s["font_boost"] <= 4):
        out.append(f"{bid}: screen.font_boost is 0-4")
    if not (isinstance(d["spi_hz"], int) and d["spi_hz"] > 0):
        out.append(f"{bid}: display.spi_hz is a speed in Hz")
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
    """board_config.h: the tag and screen as macros, and the drivers and pins as one BoardConfig
    (firmware/waveshare_169/board_config_types.h), in its field order."""
    d, t, e = b["display"], b["touch"], b["expander"]
    tri = lambda part, k: -1 if k not in part else int(part[k])  # noqa: E731
    held = lambda key: [pin_code(p) for p in b.get(key, [])] + [-1] * (MAX_HOLD - len(b.get(key, [])))  # noqa: E731
    braces = lambda xs: "{" + ", ".join(str(x) for x in xs) + "}"  # noqa: E731
    fields = [
        ("panel", f"PanelDriver::{PANEL_ENUM[d['driver']]}"),
        ("lcd_dc", pin_code(d["dc"])), ("lcd_cs", pin_code(d.get("cs"))), ("lcd_sck", pin_code(d["sck"])),
        ("lcd_mosi", pin_code(d["mosi"])), ("lcd_miso", pin_code(d.get("miso"))), ("lcd_rst", pin_code(d.get("rst"))),
        ("lcd_bl", pin_code(d["backlight"])),
        ("spi_hz", d["spi_hz"]), ("spi_mode", d.get("spi_mode", -1)),
        ("ips", "true" if d["ips"] else "false"), ("rotation", d["rotation"]), ("offsets", braces(d["offsets"])),
        ("bgr", tri(d, "bgr")), ("mirror_x", tri(d, "mirror_x")), ("mirror_y", tri(d, "mirror_y")),
        ("touch", f"TouchDriver::{TOUCH_ENUM[t['driver']]}"), ("touch_address", f"{t['address']:#04x}"),
        ("tp_sda", pin_code(t["sda"])), ("tp_scl", pin_code(t["scl"])), ("tp_rst", pin_code(t.get("rst"))),
        ("tp_int", pin_code(t.get("int"))),
        ("touch_swap_xy", tri(t, "swap_xy")), ("touch_invert_x", tri(t, "invert_x")),
        ("touch_invert_y", tri(t, "invert_y")),
        ("expander", "ExpanderDriver::TCA9554" if e else "ExpanderDriver::None"),
        ("expander_address", f"{e['address']:#04x}" if e else "0"),
        ("hold_low", braces(held("hold_low"))), ("hold_high", braces(held("hold_high"))),
        ("restart_on_power_on", "true" if b.get("restart_on_power_on") else "false"),
    ]
    lines = [
        f"// Generated by tools/boards.py from firmware/boards.json ({bid}). Don't edit: change the profile.",
        "#pragma once",
        "",
        '#include "board_config_types.h"',
        "",
        f"#define BOARD_TAG_ID {b['tag_id']}",
        f"#define BOARD_PROFILE_INIT {profile_init(b)}",
        "",
        "// Pins: GPIO n is n, expander pin n is 100 + n, none is -1. Tri-state fields: -1 unset.",
        "constexpr BoardConfig kBoardConfig = {",
        *[f"    /* {name} */ {value}," for name, value in fields],
        "};",
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


# ---------- The profile on the wire (lib/board/src/board_blob.h) ----------
# The same bytes the board decodes; tests/boards/blobs.tsv holds the cases both run.

BLOB_VERSION, BLOB_SIZE, BLOB_NAME_MAX = 1, 120, 40
# board_blob.h BlobError: change them together.
BLOB_ERRORS = {"ok": 0, "format": 1, "crc": 2, "driver": 3, "pin_missing": 4, "pin_not_on_chip": 5, "pin_usb": 6,
               "pin_flash": 7, "pin_psram": 8, "pin_twice": 9, "exio_no_expander": 10, "screen": 11, "field": 12,
               "i2c_address": 13, "name": 14}
SHAPE_CODE = {"rect": 0, "rounded": 1, "circle": 2}
PANEL_CODE = {"st7789": 0, "st7796": 1, "ili9341": 2}
TOUCH_CODE = {"cst816": 0, "ft6336": 1}
BLOB_FORMAT = "<3sBHBBHHHB" + "BBB" + "7h" + "I" + "bBbbb" + "4h" + "B" + "4h" + "bbb" + "B" + "4h4h" + f"{BLOB_NAME_MAX}s"


def crc16_xmodem(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def blob(b):
    """A profile as the board stores it (board_blob.h): BLOB_SIZE bytes."""
    import struct
    d, t, e, s = b["display"], b["touch"], b["expander"], b["screen"]
    tri = lambda part, k: -1 if k not in part else int(part[k])  # noqa: E731
    held = lambda key: [pin_code(p) for p in b.get(key, [])] + [-1] * (MAX_HOLD - len(b.get(key, [])))  # noqa: E731
    flags = (int(d["ips"]) | int(bool(b.get("restart_on_power_on"))) << 1 | int(b["psram"] == "quad") << 2
             | SHAPE_CODE[s["shape"]] << 3)
    body = struct.pack(
        BLOB_FORMAT, b"OBP", BLOB_VERSION, BLOB_SIZE, b["tag_id"], flags,
        s["width"], s["height"], s["corner_radius"], s["font_boost"],
        PANEL_CODE[d["driver"]], TOUCH_CODE[t["driver"]], 1 if e else 0,
        *[pin_code(d.get(k)) for k in ("dc", "cs", "sck", "mosi", "miso", "rst", "backlight")],
        d["spi_hz"], d.get("spi_mode", -1), d["rotation"], tri(d, "bgr"), tri(d, "mirror_x"), tri(d, "mirror_y"),
        *d["offsets"], t["address"],
        *[pin_code(t.get(k)) for k in ("sda", "scl", "rst", "int")],
        tri(t, "swap_xy"), tri(t, "invert_x"), tri(t, "invert_y"), e["address"] if e else 0,
        *held("hold_low"), *held("hold_high"), b["name"].encode()[:BLOB_NAME_MAX - 1])
    assert len(body) == BLOB_SIZE - 2, len(body)
    return body + crc16_xmodem(body).to_bytes(2, "little")


def unblob(data):
    """The profile a blob holds, in boards.json's form (for check()); ValueError if it isn't one."""
    import struct
    if len(data) != BLOB_SIZE or data[:4] != b"OBP" + bytes([BLOB_VERSION]):
        raise ValueError("format")
    if crc16_xmodem(data[:-2]) != int.from_bytes(data[-2:], "little"):
        raise ValueError("crc")
    v = list(struct.unpack(BLOB_FORMAT, data[:-2]))
    (_, _, _, tag_id, flags, width, height, radius, boost, panel, touch, expander) = v[:12]
    lcd = v[12:19]
    spi_hz, spi_mode, rotation, bgr, mx, my = v[19:25]
    offsets = v[25:29]
    taddr = v[29]
    tp = v[30:34]
    swap, ix, iy, eaddr = v[34:38]
    hold_low, hold_high, name = v[38:42], v[42:46], v[46]
    unpin = lambda c: None if c < 0 else (f"exio{c - 100}" if c >= 100 else c)  # noqa: E731
    yesno = lambda part, k, x: part.update({k: bool(x)}) if x >= 0 else None  # noqa: E731
    shapes = {c: n for n, c in SHAPE_CODE.items()}
    panels = {c: n for n, c in PANEL_CODE.items()}
    touches = {c: n for n, c in TOUCH_CODE.items()}
    if (flags >> 3) & 3 not in shapes or panel not in panels or touch not in touches or expander > 1:
        raise ValueError("driver")
    d = dict(zip(("dc", "cs", "sck", "mosi", "miso", "rst", "backlight"), map(unpin, lcd)))
    d.update(driver=panels[panel], spi_hz=spi_hz, ips=bool(flags & 1), rotation=rotation, offsets=list(offsets))
    if spi_mode != -1:
        d["spi_mode"] = spi_mode
    t = dict(zip(("sda", "scl", "rst", "int"), map(unpin, tp)))
    t.update(driver=touches[touch], address=taddr)
    for part, k, x in ((d, "bgr", bgr), (d, "mirror_x", mx), (d, "mirror_y", my),
                       (t, "swap_xy", swap), (t, "invert_x", ix), (t, "invert_y", iy)):
        yesno(part, k, x)
    return {
        "name": name.split(b"\0")[0].decode(), "short_name": "-", "site_id": "-", "tag_id": tag_id,
        "chip": "ESP32-S3", "flash_mb": 16, "psram_mb": 8, "psram": "quad" if flags & 4 else "octal", "usb_host": "-",
        "screen": {"width": width, "height": height, "shape": shapes[(flags >> 3) & 3], "corner_radius": radius,
                   "font_boost": boost},
        "display": d, "touch": t, "expander": {"driver": "tca9554", "address": eaddr} if expander else None,
        "hold_low": [unpin(p) for p in hold_low if p >= 0], "hold_high": [unpin(p) for p in hold_high if p >= 0],
        "restart_on_power_on": bool(flags & 2),
    }


def _mutated(b, **changes):
    import copy
    b = copy.deepcopy(b)
    for path, value in changes.items():
        *parents, leaf = path.split(".")
        node = b
        for part in parents:
            node = node[part]
        node[leaf] = value
    return b


def blob_cases(table):
    """(name, hex, error code) rows for tests/boards/blobs.tsv: every profile, then one broken rule each."""
    rows = [(bid, blob(b).hex(), 0) for bid, b in table["boards"].items()]
    base = table["boards"]["waveshare_35"]
    broken = [
        ("usb pin", {"display.dc": 19}, "pin_usb"),
        ("flash pin", {"touch.sda": 29}, "pin_flash"),
        ("octal psram pin", {"hold_low": [35]}, "pin_psram"),
        ("quad psram frees 33-37", {"hold_low": [35], "psram": "quad"}, "ok"),
        ("pin twice", {"display.backlight": 3}, "pin_twice"),
        ("expander pin without expander", {"expander": None}, "exio_no_expander"),
        ("pin the S3 lacks", {"display.sck": 23}, "pin_not_on_chip"),
        ("required pin missing", {"display.dc": None}, "pin_missing"),
        ("screen too wide", {"screen.width": 800}, "screen"),
        ("rounded without radius", {"screen.shape": "rounded", "screen.corner_radius": 0}, "screen"),
        ("rotation out of range", {"display.rotation": 5}, "field"),
        ("spi mode out of range", {"display.spi_mode": 4}, "field"),
        ("font boost out of range", {"screen.font_boost": 9}, "field"),
        ("i2c address", {"touch.address": 5}, "i2c_address"),
        ("bgr set", {"display.bgr": True, "touch.invert_x": False}, "ok"),
    ]
    rows += [(name, blob(_mutated(base, **ch)).hex(), BLOB_ERRORS[code]) for name, ch, code in broken]
    good = bytearray(blob(base))
    bad_crc = bytearray(good)
    bad_crc[20] ^= 0xFF
    rows += [("damaged", bad_crc.hex(), BLOB_ERRORS["crc"]), ("not a profile", (b"XBP" + good[3:]).hex(), BLOB_ERRORS["format"]),
             ("short", good[:-1].hex(), BLOB_ERRORS["format"])]
    return rows


def blob_tsv(table):
    return ("# name<TAB>hex<TAB>error code. Generated by tools/boards.py blob-cases; tests/test_board_blob.cpp and\n"
            "# tests/test_boards.py both run it. Regenerate with: python3 -I tools/boards.py blob-cases\n"
            + "".join(f"{n}\t{h}\t{c}\n" for n, h, c in blob_cases(table)))


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
    board = lambda bid: table["boards"].get(bid) or sys.exit(f"unknown board {bid}; known: {' '.join(all_ids(table))}")  # noqa: E731
    if cmd == "check":
        print(f"boards: {len(table['boards'])} profiles, all good")
    elif cmd == "ids":
        print(" ".join(ids(table)))
    elif cmd == "all-ids":
        print(" ".join(all_ids(table)))
    elif cmd == "fqbn":
        print(fqbn(board(args[0])))
    elif cmd == "get":
        print(field(board(args[0]), args[1]))
    elif cmd == "header":
        write_if_changed(args[1], header(args[0], board(args[0])))
    elif cmd == "blob":
        print(blob(board(args[0])).hex())
    elif cmd == "blob-cases":
        write_if_changed(ROOT / "tests/boards/blobs.tsv", blob_tsv(table))
    elif cmd == "shots":
        write_if_changed(args[0], shots_table(table))
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
