#!/usr/bin/env python3
"""从大字体中提取渲染所需字符子集, 生成小 TTF。
用法:
  python3 scripts/make_font_subset.py <大字体> <输出字体> [--charset=NAME]
charset:
  ascii      仅可见 ASCII (最小, 约 20KB)
  latin      ASCII + Latin-1 (内嵌内核用, 默认)
  gb2312-l1  latin + GB2312 一级汉字(3755) + 常用中文标点 (约 2.5MB, 供磁盘加载)
  gb2312     latin + GB2312 全部汉字(6763) + 常用中文标点
"""
import os
import sys
from fontTools import subset

CJK_PUNCT = "，。！？：；“”‘’（）《》、—…·【】〔〕〖〗"
def gb2312_chars(level1: bool):
    out = set()
    hi_end = 0xD8 if level1 else 0xF8
    for hi in range(0xB0, hi_end):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                pass
    return out
def charset(name: str):
    base = set(chr(c) for c in range(0x20, 0x7F))
    if name == "ascii":
        return base
    if name == "latin":
        return set(chr(c) for c in range(0x20, 0x100))
    if name == "gb2312-l1":
        return set(chr(c) for c in range(0x20, 0x100)) | gb2312_chars(True) | set(CJK_PUNCT)
    if name == "gb2312":
        return set(chr(c) for c in range(0x20, 0x100)) | gb2312_chars(False) | set(CJK_PUNCT)
    raise SystemExit(f"make_font_subset: unknown charset '{name}'")

def main():
    cs = "latin"
    pos = []
    for a in sys.argv[1:]:
        if a.startswith("--charset="):
            cs = a.split("=", 1)[1]
        else:
            pos.append(a)

    src = pos[0] if len(pos) > 0 else "lib/assets/font.ttf"
    dst = pos[1] if len(pos) > 1 else "build/font_subset.ttf"
    if not os.path.exists(src):
        raise SystemExit(f"make_font_subset: source font not found: {src}")

    chars = charset(cs)
    opts = subset.Options()
    opts.flavor = None
    opts.desubroutinize = True
    opts.hinting = False
    opts.drop_tables += ["GSUB", "GPOS", "meta", "name", "post", "gasp"]

    font = subset.load_font(src, opts)
    ss = subset.Subsetter()
    ss.populate(text="".join(sorted(chars)))
    ss.subset(font)
    font.save(dst)
    print(f"subset[{cs}] {len(chars)} chars -> {dst} "
          f"({os.path.getsize(dst) / 1024:.1f} KiB)")

if __name__ == "__main__":
    main()
