#!/usr/bin/env python3
"""Format all C/C/H sources under src/kernel with the kernel-local
.clang-format.  The style file lives in src/kernel/.clang-format, so only
kernel sources are affected — everything else keeps the repo root style.

Usage:
    python scripts/format_kernel.py            # format in place
    python scripts/format_kernel.py --check    # only report unformatted files
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KERNEL_DIR = ROOT / "src" / "kernel"

SKIP = {"stb_truetype.h"}


def find_sources() -> list[Path]:
    files = []
    for p in KERNEL_DIR.rglob("*"):
        if not p.is_file():
            continue
        if p.suffix not in (".c", ".h"):
            continue
        if p.name in SKIP:
            continue
        files.append(p)
    return sorted(files)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="report unformatted files without modifying them")
    parser.add_argument("--diff", action="store_true",
                        help="print a diff for each unformatted file")
    args = parser.parse_args(argv)

    clang_format = shutil.which("clang-format")
    if clang_format is None:
        print("error: clang-format not found on PATH", file=sys.stderr)
        return 2

    files = find_sources()
    changed: list[Path] = []
    for path in files:
        cmd = [clang_format, "--style=file", "--dry-run",
               "--Werror", str(path)]
        if not args.check:
            cmd = [clang_format, "--style=file", "-i", str(path)]
        res = subprocess.run(cmd, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        if args.check:
            if res.returncode != 0:
                changed.append(path)
                if args.diff:
                    subprocess.run(
                        [clang_format, "--style=file", str(path)],
                        stdout=subprocess.PIPE, text=True)
                    orig = path.read_text(encoding="utf-8", errors="replace")
                    diff = subprocess.run(
                        [clang_format, "--style=file", str(path)],
                        capture_output=True, text=True)
                    new = diff.stdout
                    import difflib
                    sys.stdout.writelines(difflib.unified_diff(
                        orig.splitlines(keepends=True),
                        new.splitlines(keepends=True),
                        fromfile=str(path), tofile=str(path) + " (formatted)"))
        else:
            if res.returncode != 0:
                print(f"error formatting {path}", file=sys.stderr)
                return 1

    if args.check:
        if changed:
            print(f"{len(changed)} file(s) need formatting:")
            for p in changed:
                print(f"  {p.relative_to(ROOT)}")
            return 1
        print(f"ok: {len(files)} kernel sources are formatted")
        return 0

    print(f"formatted {len(files)} kernel sources")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
