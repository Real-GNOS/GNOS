#!/bin/bash
# hudos-run.sh <guest.elf> [guest 参数...]
# 在宿主 qemu-aarch64 内运行 hudos aarch64 app（无需 hudos-server）：
#   guest 侧 loader 把 ELF 段映射进 guest 内存 → 打网关跳转补丁 → 注入服务分发器
#   → 跳入 guest main；Linux 号段服务经 svc 直通宿主，自定义号段由分发器 stub 处理。
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ELF="$1"; shift || true
[ -f "$ELF" ] || { echo "[hudos-run] 用法: $0 <guest.elf> [参数...]" >&2; exit 2; }
[ -f "$HERE/loader.elf" ] || { echo "[hudos-run] 请先运行 ./build.sh" >&2; exit 2; }

SYS=$(nm "$ELF" | awk '$2=="T" && $3=="syscall"{print $1}')
MAIN=$(nm "$ELF" | awk '$2=="T" && $3=="main"{print $1}')
if [ -z "$SYS" ] || [ -z "$MAIN" ]; then
  echo "[hudos-run] 未找到 syscall/main 符号: $ELF" >&2; exit 2
fi
# 注意: 直接透传 nm 的十六进制地址(loader 的 htoi 按 hex 解析)，勿转十进制
# W/H = 帧缓冲逻辑分辨率(loader argv[5]/argv[6])，其后才是 guest 参数
exec qemu-aarch64 "$HERE/loader.elf" "$ELF" "$SYS" "$MAIN" "$HERE/disp.bin" "${W:-1024}" "${H:-768}" "$@"
