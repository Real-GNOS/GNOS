from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KCONFIG_FILE = ROOT / "Kconfig"
CONFIG_FILE = ROOT / ".config"
AUTOCONF_H = ROOT / "build" / "config" / "autoconf.h"

try:
    import kconfiglib
except ImportError:
    sys.stderr.write("kconfiglib 未安装，请先执行: pip install kconfiglib\n")
    raise SystemExit(1)


def _kconf() -> "kconfiglib.Kconfig":
    os.environ["KCONFIG_CONFIG"] = str(CONFIG_FILE)
    return kconfiglib.Kconfig(str(KCONFIG_FILE))


def _write_autoconf(kconf: "kconfiglib.Kconfig") -> None:
    AUTOCONF_H.parent.mkdir(parents=True, exist_ok=True)
    kconf.write_autoconf(str(AUTOCONF_H))


def _olddefconfig() -> None:
    kconf = _kconf()
    kconf.load_config(str(CONFIG_FILE), replace=True)
    kconf.write_config(str(CONFIG_FILE), save_old=False)
    _write_autoconf(kconf)


def _defconfig(name: str) -> None:
    candidates = sorted(ROOT.glob(f"arch/*/configs/{name}"))
    if not candidates:
        candidates = sorted(ROOT.glob(f"configs/{name}"))
    if not candidates:
        sys.stderr.write(f"defconfig 未找到: {name}\n")
        raise SystemExit(1)
    CONFIG_FILE.write_text(candidates[0].read_text(encoding="utf-8"),
                           encoding="utf-8")
    _olddefconfig()


def _syncconfig() -> None:
    _olddefconfig()


def _menuconfig() -> None:
    import menuconfig as _menuconfig_mod
    kconf = _kconf()
    kconf.load_config(str(CONFIG_FILE), replace=True)
    _menuconfig_mod.menuconfig(kconf)
    kconf.write_config(str(CONFIG_FILE), save_old=False)
    _write_autoconf(kconf)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="kconfig")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("defconfig").add_argument("name")
    sub.add_parser("olddefconfig")
    sub.add_parser("syncconfig")
    sub.add_parser("menuconfig")
    args = parser.parse_args(argv)

    if args.cmd == "defconfig":
        _defconfig(args.name)
    elif args.cmd == "olddefconfig":
        _olddefconfig()
    elif args.cmd == "syncconfig":
        _syncconfig()
    elif args.cmd == "menuconfig":
        _menuconfig()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))