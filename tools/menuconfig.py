#!/usr/bin/env python3
"""
menuconfig.py — Interactive (and scriptable) editor for include/rtos_config.h.

Quartz has ~25 compile-time configuration knobs living in
include/rtos_config.h, each guarded with the
"#ifndef NAME / #define NAME value / #endif" idiom. This tool discovers
every one of them (name, current value, section, description) straight from
that header — there is nothing else to keep in sync — and lets you browse
and edit them without hand-editing C.

Usage:
    python3 tools/menuconfig.py                # interactive curses UI
    python3 tools/menuconfig.py --list          # dump all options and exit
    python3 tools/menuconfig.py --set RTOS_MAX_TASKS=32 RTOS_ENABLE_TRACE=1

Only the value token on each "#define NAME value" line is ever rewritten —
comments, spacing, and section banners are left untouched, so `git diff`
shows a minimal, single-token change per edited option.

Per-target CMake overrides (samples, toolchain files, the Arduino auto-tune
block) still take precedence at compile time over whatever default this
script writes here — editing rtos_config.h only changes the fallback for
targets that don't already force a value.
"""

import argparse
import curses
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG_PATH = REPO_ROOT / "include" / "rtos_config.h"

# The Arduino auto-tune block (top of the file) pre-defines a few of these
# same macro names conditionally, ahead of the canonical #ifndef blocks
# below it. It must be excluded from parsing/editing — it is fallback logic,
# not the option we want to expose.
ARDUINO_BLOCK_RE = re.compile(r"#if defined\(ARDUINO\).*?#endif // ARDUINO\n", re.S)

OPTION_RE = re.compile(
    r"#ifndef\s+(RTOS_\w+)\s*\n"
    r"#\s*define\s+\1\s+(\S+)[^\n]*\n"
    r"#endif\n"
)

SECTION_ORDER = [
    "Core sizing",
    "RISC-V CLINT",
    "ESP32-S3",
    "Portable tick type",
    "Debug and instrumentation",
    "Multi-core (AMP)",
    "Feature enable/disable",
    "Chrome trace recorder",
]

# name -> (section, kind).  kind is "bool" | "int" | "hex".
# Hardcoded rather than inferred from the literal default because a value of
# "1" means "boolean flag, on" for RTOS_ENABLE_EVENT_GROUPS but "single
# core" for RTOS_NUM_CORES — the two are not distinguishable from the value
# alone.
OPTION_META = {
    "RTOS_MAX_TASKS":                   ("Core sizing", "int"),
    "RTOS_MAX_PRIORITIES":              ("Core sizing", "int"),
    "RTOS_TICK_RATE_HZ":                ("Core sizing", "int"),
    "RTOS_MAX_TIMERS":                  ("Core sizing", "int"),
    "RTOS_TASK_NAME_LEN":               ("Core sizing", "int"),
    "RTOS_IDLE_STACK_WORDS":            ("Core sizing", "int"),
    "RTOS_STACK_BYTES_PER_WORD":        ("Core sizing", "int"),

    "RTOS_CLINT_BASE_ADDR":             ("RISC-V CLINT", "hex"),
    "RTOS_MTIME_HZ":                    ("RISC-V CLINT", "hex"),

    "RTOS_ESP32S3_CPU_HZ":              ("ESP32-S3", "hex"),
    "RTOS_TIMG0_BASE_ADDR":             ("ESP32-S3", "hex"),

    "RTOS_TICK_TYPE_16BIT":             ("Portable tick type", "bool"),

    "RTOS_STACK_OVERFLOW_CHECK":        ("Debug and instrumentation", "bool"),
    "RTOS_STACK_WATERMARK":             ("Debug and instrumentation", "bool"),
    "RTOS_ENABLE_TRACE":                ("Debug and instrumentation", "bool"),
    "RTOS_ENABLE_RUNTIME_STATS":        ("Debug and instrumentation", "bool"),
    "RTOS_TICKLESS_IDLE":               ("Debug and instrumentation", "bool"),

    "RTOS_NUM_CORES":                   ("Multi-core (AMP)", "int"),

    "RTOS_ENABLE_TASK_DELETE":          ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_TASK_SUSPEND":         ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_TASK_NOTIFY":          ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_SOFTWARE_TIMERS":      ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_PRIORITY_INHERITANCE": ("Feature enable/disable", "bool"),
    "RTOS_CM4_FPU":                     ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_RECURSIVE_MUTEX":      ("Feature enable/disable", "bool"),
    "RTOS_ENABLE_EVENT_GROUPS":         ("Feature enable/disable", "bool"),

    "RTOS_TRACE_HANDLE_TABLE_SIZE":     ("Chrome trace recorder", "int"),
    "RTOS_TRACE_BUFFER_BYTES":          ("Chrome trace recorder", "int"),
}


class Option:
    def __init__(self, name, value, value_start, value_end, section, kind, description):
        self.name = name
        self.value = value
        self.value_start = value_start
        self.value_end = value_end
        self.section = section
        self.kind = kind
        self.description = description


def extract_description(text, ifndef_start):
    line_start = text.rfind("\n", 0, ifndef_start) + 1
    # text[:line_start] ends with the newline that terminates the line just
    # above ifndef_start, so split("\n") leaves a trailing "" — start one
    # index below the end to land on that actual line.
    lines_before = text[:line_start].split("\n")
    desc_lines = []
    blank_skipped = False
    i = len(lines_before) - 2
    while i >= 0:
        line = lines_before[i].strip()
        if line.startswith("//") and not line.startswith("// ---"):
            desc_lines.append(line.lstrip("/").strip())
            blank_skipped = False
            i -= 1
        elif line == "" and not blank_skipped:
            # Allow a single blank line between the comment block and the
            # #ifndef (e.g. the "Portable tick type" option).
            blank_skipped = True
            i -= 1
        else:
            break
    desc_lines.reverse()
    return " ".join(desc_lines)


def parse_config(text):
    """Return {name: Option} for every macro in OPTION_META found in text."""
    arduino_match = ARDUINO_BLOCK_RE.search(text)
    arduino_span = arduino_match.span() if arduino_match else None

    options = {}
    for m in OPTION_RE.finditer(text):
        name = m.group(1)
        if arduino_span and arduino_span[0] <= m.start() < arduino_span[1]:
            continue
        if name not in OPTION_META:
            continue
        section, kind = OPTION_META[name]
        value_start, value_end = m.span(2)
        options[name] = Option(
            name, text[value_start:value_end], value_start, value_end,
            section, kind, extract_description(text, m.start()),
        )

    missing = set(OPTION_META) - set(options)
    if missing:
        raise RuntimeError(
            f"menuconfig: could not find {sorted(missing)} in {DEFAULT_CONFIG_PATH} "
            "— has the file structure changed? OPTION_META in this script needs updating."
        )
    return options


def format_value(kind, old_raw, new_raw):
    new_raw = new_raw.strip()
    if not new_raw:
        raise ValueError("value cannot be empty")

    if kind == "bool":
        if new_raw not in ("0", "1"):
            raise ValueError(f"{new_raw!r} is not 0 or 1")
        return new_raw

    if kind == "int":
        int(new_raw, 0)  # raises ValueError if not a valid integer literal
        return new_raw

    if kind == "hex":
        m = re.match(r"^(0[xX][0-9a-fA-F]+|\d+)([uUlL]*)$", new_raw)
        if not m:
            raise ValueError(f"{new_raw!r} is not a valid number (with optional U/L suffix)")
        numeric, suffix = m.group(1), m.group(2)
        int(numeric, 0)
        if not suffix:
            old_suffix = re.search(r"[uUlL]+$", old_raw)
            suffix = old_suffix.group(0) if old_suffix else ""
        return f"{numeric}{suffix}"

    raise ValueError(f"unknown option kind {kind!r}")


def apply_edits(text, options, edits):
    """edits: {name: new_raw_value}. Returns (new_text, [(name, old, new), ...])."""
    changes = []
    spans = []
    for name, new_value in edits.items():
        opt = options[name]
        if opt.value == new_value:
            continue
        spans.append((opt.value_start, opt.value_end, new_value))
        changes.append((name, opt.value, new_value))

    # Apply back-to-front so earlier offsets stay valid.
    for start, end, new_value in sorted(spans, key=lambda s: s[0], reverse=True):
        text = text[:start] + new_value + text[end:]

    return text, changes


def sorted_options(options):
    """Options grouped by SECTION_ORDER, in OPTION_META declaration order within each section."""
    by_section = {section: [] for section in SECTION_ORDER}
    for name in OPTION_META:
        if name in options:
            by_section[options[name].section].append(options[name])
    return by_section


def do_list(options):
    by_section = sorted_options(options)
    for section in SECTION_ORDER:
        opts = by_section[section]
        if not opts:
            continue
        print(f"# {section}")
        for opt in opts:
            print(f"{opt.name}={opt.value}")
            if opt.description:
                print(f"    {opt.description}")
        print()


def do_set(config_path, text, options, assignments):
    edits = {}
    for assignment in assignments:
        if "=" not in assignment:
            print(f"menuconfig: bad assignment {assignment!r}, expected NAME=VALUE", file=sys.stderr)
            sys.exit(2)
        name, raw_value = assignment.split("=", 1)
        name = name.strip()
        if name not in options:
            print(f"menuconfig: unknown option {name!r}", file=sys.stderr)
            sys.exit(2)
        opt = options[name]
        try:
            edits[name] = format_value(opt.kind, opt.value, raw_value)
        except ValueError as e:
            print(f"menuconfig: {name}: {e}", file=sys.stderr)
            sys.exit(2)

    new_text, changes = apply_edits(text, options, edits)
    if not changes:
        print("No changes.")
        return

    config_path.write_text(new_text)
    for name, old, new in changes:
        print(f"{name}: {old} -> {new}")
    print(f"\nWrote {config_path}. Rebuild to pick up the new defaults.")


# ---------------------------------------------------------------------------
# Interactive curses UI
# ---------------------------------------------------------------------------

def _build_rows(by_section):
    """Flatten sections+options into display rows: ("header", text) or ("option", Option)."""
    rows = []
    for section in SECTION_ORDER:
        opts = by_section[section]
        if not opts:
            continue
        rows.append(("header", section))
        for opt in opts:
            rows.append(("option", opt))
    return rows


def _prompt_value(stdscr, opt):
    height, width = stdscr.getmaxyx()
    y = height - 1
    prompt = f"New value for {opt.name} (current: {opt.value}): "
    stdscr.move(y, 0)
    stdscr.clrtoeol()
    stdscr.addstr(y, 0, prompt[: max(width - 1, 0)])
    stdscr.refresh()

    curses.echo()
    curses.curs_set(1)
    try:
        raw = stdscr.getstr(y, min(len(prompt), width - 1), 32).decode(errors="replace")
    finally:
        curses.noecho()
        curses.curs_set(0)
    return raw


def _curses_main(stdscr, config_path, text, options):
    curses.curs_set(0)
    stdscr.keypad(True)

    by_section = sorted_options(options)
    rows = _build_rows(by_section)
    edits = {}  # name -> pending new raw value (already formatted)
    selected = next(i for i, r in enumerate(rows) if r[0] == "option")
    message = ""

    def current_value(opt):
        return edits.get(opt.name, opt.value)

    while True:
        stdscr.erase()
        height, width = stdscr.getmaxyx()
        stdscr.addstr(0, 0, "Quartz RTOS configuration  ".ljust(width - 1)[: width - 1], curses.A_REVERSE)

        list_height = height - 4
        top = max(0, selected - list_height + 1) if selected >= list_height else 0
        for row_idx in range(top, min(len(rows), top + list_height)):
            kind, payload = rows[row_idx]
            y = 1 + (row_idx - top)
            if kind == "header":
                stdscr.addstr(y, 0, f"-- {payload} --"[: width - 1], curses.A_BOLD)
            else:
                opt = payload
                marker = "*" if opt.name in edits else " "
                line = f"  {marker} {opt.name} = {current_value(opt)}"
                attr = curses.A_REVERSE if row_idx == selected else curses.A_NORMAL
                stdscr.addstr(y, 0, line[: width - 1], attr)

        desc_y = height - 3
        if rows[selected][0] == "option":
            opt = rows[selected][1]
            desc = f"{opt.description} [{opt.kind}]"
        else:
            desc = ""
        stdscr.addstr(desc_y, 0, desc[: width - 1])

        help_line = "Up/Down move  Enter edit  s save & quit  q quit without saving"
        stdscr.addstr(height - 2, 0, help_line[: width - 1])
        stdscr.addstr(height - 1, 0, message[: width - 1])
        message = ""
        stdscr.refresh()

        ch = stdscr.getch()
        if ch in (curses.KEY_UP, ord("k")):
            new_sel = selected
            while new_sel > 0:
                new_sel -= 1
                if rows[new_sel][0] == "option":
                    selected = new_sel
                    break
        elif ch in (curses.KEY_DOWN, ord("j")):
            new_sel = selected
            while new_sel < len(rows) - 1:
                new_sel += 1
                if rows[new_sel][0] == "option":
                    selected = new_sel
                    break
        elif ch in (curses.KEY_ENTER, 10, 13):
            opt = rows[selected][1]
            raw = _prompt_value(stdscr, opt)
            if raw:
                try:
                    edits[opt.name] = format_value(opt.kind, opt.value, raw)
                    message = f"{opt.name} staged: {opt.value} -> {edits[opt.name]}"
                except ValueError as e:
                    message = f"error: {e}"
        elif ch == ord("s"):
            new_text, changes = apply_edits(text, options, edits)
            if changes:
                config_path.write_text(new_text)
            return changes
        elif ch == ord("q"):
            if edits:
                stdscr.addstr(height - 1, 0, "Discard unsaved changes? (y/n) "[: width - 1])
                stdscr.refresh()
                confirm = stdscr.getch()
                if confirm in (ord("y"), ord("Y")):
                    return []
            else:
                return []


def do_interactive(config_path, text, options):
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print(
            "menuconfig: not running in an interactive terminal; use --list or --set instead.",
            file=sys.stderr,
        )
        sys.exit(1)
    try:
        changes = curses.wrapper(_curses_main, config_path, text, options)
    except curses.error as e:
        print(f"menuconfig: terminal does not support curses ({e}); use --list/--set instead.", file=sys.stderr)
        sys.exit(1)

    if changes:
        for name, old, new in changes:
            print(f"{name}: {old} -> {new}")
        print(f"\nWrote {config_path}. Rebuild to pick up the new defaults.")
    else:
        print("No changes.")


def main():
    parser = argparse.ArgumentParser(
        description="Browse and edit Quartz RTOS compile-time configuration (include/rtos_config.h).",
    )
    parser.add_argument("--list", action="store_true", help="List all options and exit (no editing)")
    parser.add_argument(
        "--set", nargs="+", metavar="NAME=VALUE",
        help="Set one or more options non-interactively",
    )
    parser.add_argument(
        "--config", type=Path, default=DEFAULT_CONFIG_PATH,
        help=f"Path to rtos_config.h (default: {DEFAULT_CONFIG_PATH.relative_to(REPO_ROOT)})",
    )
    args = parser.parse_args()

    text = args.config.read_text()
    options = parse_config(text)

    if args.list:
        do_list(options)
    elif args.set:
        do_set(args.config, text, options, args.set)
    else:
        do_interactive(args.config, text, options)


if __name__ == "__main__":
    main()
