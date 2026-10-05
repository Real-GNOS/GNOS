#!/usr/bin/env python3
import argparse
import os
import re
import shutil
import sys


def _alltypes_lines(arch_in: str, root_in: str):
    typedef_re = re.compile(r"^TYPEDEF\s+(.*?)\s+([^ ]+);\s*$")
    struct_re = re.compile(r"^STRUCT\s+(\S+)\s+(.*);\s*$")
    union_re = re.compile(r"^UNION\s+(\S+)\s+(.*);\s*$")

    def transform(line: str) -> str:
        m = typedef_re.match(line)
        if m:
            typ, name = m.group(1), m.group(2)
            return (
                f"#if defined(__NEED_{name}) && !defined(__DEFINED_{name})\n"
                f"typedef {typ} {name};\n"
                f"#define __DEFINED_{name}\n"
                f"#endif\n"
            )
        m = struct_re.match(line)
        if m:
            name, body = m.group(1), m.group(2)
            return (
                f"#if defined(__NEED_struct_{name}) && !defined(__DEFINED_struct_{name})\n"
                f"struct {name} {body};\n"
                f"#define __DEFINED_struct_{name}\n"
                f"#endif\n"
            )
        m = union_re.match(line)
        if m:
            name, body = m.group(1), m.group(2)
            return (
                f"#if defined(__NEED_union_{name}) && !defined(__DEFINED_union_{name})\n"
                f"union {name} {body};\n"
                f"#define __DEFINED_union_{name}\n"
                f"#endif\n"
            )
        return line

    for path in (arch_in, root_in):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                out = transform(line.rstrip("\n"))
                if not out.endswith("\n"):
                    out += "\n"
                yield out


def _gen_alltypes(src: str, out_bits: str) -> None:
    arch_in = os.path.join(src, "arch", "x86_64", "bits", "alltypes.h.in")
    root_in = os.path.join(src, "include", "alltypes.h.in")
    if not (os.path.isfile(arch_in) and os.path.isfile(root_in)):
        raise SystemExit(f"missing alltypes templates: {arch_in} / {root_in}")
    with open(os.path.join(out_bits, "alltypes.h"), "w", encoding="utf-8") as fh:
        fh.writelines(_alltypes_lines(arch_in, root_in))


def _gen_syscall(src: str, out_bits: str) -> None:
    sh_in = os.path.join(src, "arch", "x86_64", "bits", "syscall.h.in")
    if not os.path.isfile(sh_in):
        raise SystemExit(f"missing syscall template: {sh_in}")
    with open(sh_in, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    out = list(lines)
    for line in lines:
        if "__NR_" in line:
            out.append(line.replace("__NR_", "SYS_"))
    with open(os.path.join(out_bits, "syscall.h"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))
        if out:
            fh.write("\n")


def _copy_bits(src: str, out_bits: str, sub: str) -> None:
    src_bits = os.path.join(src, "arch", sub, "bits")
    if not os.path.isdir(src_bits):
        raise SystemExit(f"missing arch bits dir: {src_bits}")
    for name in os.listdir(src_bits):
        if not name.endswith(".h"):
            continue
        shutil.copyfile(os.path.join(src_bits, name), os.path.join(out_bits, name))


def _rmtree_robust(path: str) -> None:
    def _onerr(func, p, exc_info):
        try:
            os.chmod(p, 0o755)
            func(p)
        except OSError:
            pass
    if os.path.isdir(path):
        shutil.rmtree(path, onerror=_onerr)


def _copy_tree_no_meta(src_root: str, dst_root: str) -> None:
    for root, dirs, files in os.walk(src_root):
        rel = os.path.relpath(root, src_root)
        dst_dir = dst_root if rel == "." else os.path.join(dst_root, rel)
        os.makedirs(dst_dir, exist_ok=True)
        for name in files:
            shutil.copyfile(os.path.join(root, name), os.path.join(dst_dir, name))


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble musl headers into an install dir")
    ap.add_argument("--src", required=True, help="musl source root")
    ap.add_argument("--out", required=True, help="destination include dir (e.g. build/musl/include)")
    ap.add_argument("--arch", default="x86_64", help="target architecture")
    args = ap.parse_args()

    src = args.src
    out = args.out
    if not os.path.isdir(os.path.join(src, "include")):
        raise SystemExit(f"musl source has no include/ dir: {src}")

    _rmtree_robust(out)
    _copy_tree_no_meta(os.path.join(src, "include"), out)

    out_bits = os.path.join(out, "bits")
    os.makedirs(out_bits, exist_ok=True)
    _copy_bits(src, out_bits, "generic")
    _copy_bits(src, out_bits, args.arch)

    _gen_alltypes(src, out_bits)
    _gen_syscall(src, out_bits)

    print(f"musl headers assembled -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
