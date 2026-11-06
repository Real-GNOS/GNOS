#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SYSNUM = ROOT / "src" / "include" / "uapi" / "linux" / "sysnum.h"
SYSCALL = ROOT / "src" / "kernel" / "syscall.c"
EXCEPTIONS = ROOT / "tools" / "verify" / "syscall-exceptions.tsv"

DEFINE_RE = re.compile(r"^\s*#\s*define\s+(SYS_[A-Za-z0-9_]+)\s+(.+?)\s*$")
CASE_RE = re.compile(r"^\s*case\s+(SYS_[A-Za-z0-9_]+)\s*:", re.MULTILINE)
PRIVATE_RE = re.compile(r"^\(\s*SYS_GNOS_BASE\s*\+\s*(\d+)\s*\)$")
BASE_NAME = "SYS_GNOS_BASE"


def strip_comment(value: str) -> str:
    return re.split(r"/\*|//", value, maxsplit=1)[0].strip()


def parse_definitions(path: Path) -> tuple[dict[str, int], dict[str, str], list[str]]:
    values: dict[str, int] = {}
    kinds: dict[str, str] = {}
    errors: list[str] = []
    base: int | None = None
    pending: list[tuple[str, str]] = []

    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        match = DEFINE_RE.match(line)
        if not match:
            continue
        name, raw = match.group(1), strip_comment(match.group(2))
        if name == BASE_NAME:
            try:
                base = int(raw, 0)
            except ValueError:
                errors.append(f"{path}:{lineno}: cannot parse {BASE_NAME} value {raw!r}")
            continue
        if raw.isdigit() or raw.lower().startswith("0x"):
            try:
                values[name] = int(raw, 0)
            except ValueError:
                errors.append(f"{path}:{lineno}: cannot parse value {raw!r} for {name}")
            else:
                kinds[name] = "gnos" if raw.startswith("(") else "linux"
            continue
        pending.append((name, raw))

    if base is None:
        errors.append(f"{path}: missing {BASE_NAME}")

    for name, raw in pending:
        match = PRIVATE_RE.match(raw)
        if not match:
            errors.append(f"{path}: unsupported definition {name} = {raw}")
            continue
        if base is None:
            continue
        values[name] = base + int(match.group(1))
        kinds[name] = "gnos"

    return values, kinds, errors


def parse_cases(path: Path) -> tuple[list[str], set[str], list[str]]:
    names = CASE_RE.findall(path.read_text())
    errors = [f"duplicate case label {name}" for name in sorted({n for n in names if names.count(n) > 1})]
    return names, set(names), errors


def load_exceptions(path: Path) -> tuple[dict[str, str], list[str]]:
    exceptions: dict[str, str] = {}
    errors: list[str] = []
    if not path.exists():
        return exceptions, [f"{path}: missing exception registry"]
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            errors.append(f"{path}:{lineno}: expected name<TAB>state<TAB>reason")
            continue
        name, state, reason = parts
        if state != "MISSING":
            errors.append(f"{path}:{lineno}: unsupported state {state!r} for {name}")
            continue
        if not reason.strip():
            errors.append(f"{path}:{lineno}: empty reason for {name}")
            continue
        if name in exceptions:
            errors.append(f"{path}:{lineno}: duplicate exception {name}")
            continue
        exceptions[name] = reason
    return exceptions, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Check syscall header/dispatcher consistency")
    parser.add_argument("--list", action="store_true", help="print the syscall inventory")
    args = parser.parse_args()

    values, kinds, errors = parse_definitions(SYSNUM)
    case_names, cases, case_errors = parse_cases(SYSCALL)
    errors.extend(case_errors)
    exceptions, exception_errors = load_exceptions(EXCEPTIONS)
    errors.extend(exception_errors)

    declared = set(values)
    unknown_cases = sorted(cases - declared)
    for name in unknown_cases:
        errors.append(f"{SYSCALL}: case {name} has no SYS_ definition")

    for name in sorted(exceptions):
        if name not in declared:
            errors.append(f"{EXCEPTIONS}: exception {name} is not defined")
        elif name in cases:
            errors.append(f"{EXCEPTIONS}: {name} is now handled; remove the stale exception")

    by_value: dict[int, list[str]] = {}
    for name, value in values.items():
        by_value.setdefault(value, []).append(name)
    for value, names in sorted(by_value.items()):
        if len(names) > 1:
            errors.append(f"duplicate syscall number {value}: {', '.join(sorted(names))}")

    missing = declared - cases
    unexpected = sorted(missing - set(exceptions))
    wired = declared & cases

    if args.list:
        for name in sorted(declared, key=lambda n: (values[n], n)):
            state = "MISSING" if name in exceptions else ("WIRED" if name in cases else "UNWIRED")
            print(f"{values[name]}\t{name}\t{state}\t{kinds.get(name, 'unknown')}\t{exceptions.get(name, '')}")

    linux = sum(1 for name in declared if kinds.get(name) == "linux")
    gnos = sum(1 for name in declared if kinds.get(name) == "gnos")
    print(
        "syscall_audit: "
        f"declared={len(declared)} linux={linux} gnos={gnos} "
        f"cases={len(case_names)} wired={len(wired)} "
        f"known-missing={len(exceptions)} unexpected-missing={len(unexpected)}"
    )

    for name in sorted(exceptions):
        print(f"syscall_audit: known missing: {name} ({exceptions[name]})")
    for name in unexpected:
        print(f"syscall_audit: unexpected missing: {name} ({values[name]})")
    for error in errors:
        print(f"syscall_audit: error: {error}", file=sys.stderr)

    if errors or unexpected:
        print("syscall_audit: FAIL", file=sys.stderr)
        return 1
    print("syscall_audit: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
