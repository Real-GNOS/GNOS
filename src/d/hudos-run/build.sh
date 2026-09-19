#!/bin/bash
# build.sh — 构建 hudos-run：loader.elf(guest 侧加载器) + disp.bin(服务分发器代码)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
CC=clang
CODE_VA=0x20080000   # 分发器固定加载地址 = loader CODE_AREA(v4 用绝对 adrp 引用自身头部, 必须按运行地址链接)

# 1) aarch64 服务分发器 → disp.bin（读 RX 段字节）
$CC --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld \
    -Wl,-e,_dispatch -Wl,-Ttext=$CODE_VA dispatcher.S -o /tmp/hudos_disp.elf
python3 - <<'EOF'
import struct
d = open('/tmp/hudos_disp.elf', 'rb').read()
e_phoff = struct.unpack_from('<Q', d, 32)[0]
esz = struct.unpack_from('<H', d, 54)[0]
pn = struct.unpack_from('<H', d, 56)[0]
for i in range(pn):
    p = e_phoff + i * esz
    if struct.unpack_from('<I', d, p)[0] == 1:
        fl = struct.unpack_from('<I', d, p + 4)[0]
        off, va, _, fsz, _, _ = struct.unpack_from('<QQQQQQ', d, p + 8)
        if fl & 1:
            open('/home/elaina/gnos/src/d/hudos-run/disp.bin', 'wb').write(d[off:off + fsz])
            print('disp.bin len =', fsz)
            break
EOF

# 2) guest 侧加载器 loader.elf
$CC --target=aarch64-linux-gnu -nostdlib -static -fuse-ld=lld \
    -Wl,-e,_start syscalls.S loader.c -o loader.elf
echo "built: loader.elf, disp.bin"
