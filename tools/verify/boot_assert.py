#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PASS_RE = re.compile(r"PASS")
FAIL_RE = re.compile(r"FAIL")
INITTAB_RE = re.compile(r"p=1 .*name=inittab")
OPENRC_RE = re.compile(r"^EXEC p=\d+ path=/sbin/openrc", re.MULTILINE)
AGETTY_RE = re.compile(r"^EXEC p=\d+ path=/sbin/agetty", re.MULTILINE)
FORBIDDEN = (
    re.compile(r"Kernel panic - not syncing:"),
    re.compile(r"GNOS: DOUBLE FAULT"),
    re.compile(r"GNOS: RING0 FAULT"),
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Assert GNOS headless boot log invariants")
    parser.add_argument("log", nargs="?", default="build/dbg.log")
    args = parser.parse_args()

    path = Path(args.log)
    if not path.is_file():
        print(f"boot_assert: missing log {path}", file=sys.stderr)
        return 1

    text = path.read_text(errors="replace")
    failures: list[str] = []

    if not PASS_RE.search(text):
        failures.append(f"no PASS lines in {path}")
    fail_lines = [line for line in text.splitlines() if FAIL_RE.search(line)]
    if fail_lines:
        failures.append(f"FAIL present ({len(fail_lines)} line(s))")
        for line in fail_lines[:10]:
            failures.append(f"  {line}")

    if "init is pid 1" not in text:
        failures.append("real init never became pid 1")
    if not INITTAB_RE.search(text):
        failures.append("pid 1 never read /etc/inittab")
    if "NEW pid=3" not in text:
        failures.append("init never forked its first child")

    openrc = len(OPENRC_RE.findall(text))
    agetty = len(AGETTY_RE.findall(text))
    if openrc < 3 or agetty < 1:
        failures.append(f"boot incomplete: openrc runlevels {openrc}/3, agetty spawns {agetty} (need 3 and 1)")

    for pattern in FORBIDDEN:
        for match in pattern.finditer(text):
            line_start = text.rfind("\n", 0, match.start()) + 1
            line_end = text.find("\n", match.end())
            if line_end == -1:
                line_end = len(text)
            failures.append(f"fatal marker: {text[line_start:line_end].strip()}")

    if failures:
        for failure in failures:
            print(f"boot_assert: {failure}", file=sys.stderr)
        print("boot_assert: FAIL", file=sys.stderr)
        return 1

    print("test: PASS lines present, no FAIL")
    init_markers = sum(
        marker in text for marker in ("init is pid 1", "name=inittab", "NEW pid=3")
    )
    print(
        "NOTICE: real Alpine init started correctly "
        f"(pid 1 + inittab + child), {init_markers}/3 markers"
    )
    print(
        "NOTICE: OpenRC + real AGeTTy started correctly, "
        f"openrc={openrc}/3 runlevels, agetty={agetty} gettys"
    )
    print("boot_assert: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
