#!/bin/sh
# elf2linux init - minimal initramfs init script

mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs    tmpfs    /tmp
mount -t tmpfs    tmpfs    /dev/shm

mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null

export PATH=/bin:/usr/bin
export HOME=/tmp
export LD_LIBRARY_PATH=/lib:/lib64:/usr/lib:/usr/lib/x86_64-linux-gnu

echo ""
echo "=============================="
echo "  elf2linux - ELF Boot"
echo "=============================="
echo ""

exec /bin/os.elf "$@"
