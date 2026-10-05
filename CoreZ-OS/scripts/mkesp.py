"""生成 UEFI ESP 镜像：MBR + 单个 0xEF 分区 + FAT16 文件系统。

相对分区起始(2048)的布局:
  +0    引导扇区/BPB               1 扇区
  +1    FAT1                     256 扇区
  +257  FAT2                     256 扇区
  +513  根目录(512 项)             32 扇区   -> ROOT_LBA=2561
  +545  数据区, 簇号自 2 起, SPC=1           -> DATA_LBA=2593

固定目录树(全部 8.3 名, 不需要 LFN):
  \\EFI\\BOOT\\BOOTX64.EFI
  \\KERNEL.ELF

构造方式参考 scripts/make_fat.py 的 FAT16 部分，差异是本脚本需要子目录，
且分区类型写为 ESP(0xEF)，供 OVMF 作为可引导设备识别。
"""
from pathlib import Path
import struct

SECTOR = 512
PART_START = 2048
PART_SECTORS = 65536
RESERVED = 1
FAT_SECTORS = 256
FAT_COUNT = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
SPC = 1

ROOT_LBA = PART_START + RESERVED + FAT_COUNT * FAT_SECTORS
DATA_LBA = ROOT_LBA + ROOT_SECTORS

ATTR_DIR = 0x10
ATTR_FILE = 0x20

C_EFI = 2
C_BOOT = 3
C_FIRST = 4

ESP_LBA = PART_START + 1


def _boot_sector(total_sectors):
    b = bytearray(SECTOR)
    b[0:3] = b"\xEB\x3C\x90"
    b[3:11] = b"MSDOS5.0"
    struct.pack_into("<H", b, 11, SECTOR)
    b[13] = SPC
    struct.pack_into("<H", b, 14, RESERVED)
    b[16] = FAT_COUNT
    struct.pack_into("<H", b, 17, ROOT_ENTRIES)
    struct.pack_into("<H", b, 19, 0)
    b[21] = 0xF8
    struct.pack_into("<H", b, 22, FAT_SECTORS)
    struct.pack_into("<H", b, 24, 63)
    struct.pack_into("<H", b, 26, 255)
    struct.pack_into("<I", b, 28, PART_START)
    struct.pack_into("<I", b, 32, total_sectors)
    b[36] = 0x80
    b[38] = 0x29
    struct.pack_into("<I", b, 39, 0x5A45524F)
    b[43:54] = b"COREZ ESP  "
    b[54:62] = b"FAT16   "
    b[510:512] = b"\x55\xAA"
    return bytes(b)


def _entry(name, attr, cluster, size=0):
    e = bytearray(32)
    if name == "." or name == "..":
        base, ext = name, ""
    else:
        base, _, ext = name.partition(".")
    e[0:8] = (base + " " * 8)[:8].encode()
    e[8:11] = (ext + " " * 3)[:3].encode()
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def _disk_entry(lba, sectors):
    e = bytearray(16)
    e[0] = 0x00
    e[1:4] = bytes([0x08, 0x09, 0x00])
    e[4] = 0xEF
    end = lba + sectors - 1
    head = end % 255
    sec = (end // 255) % 63 + 1
    cyl = end // (255 * 63)
    e[5:8] = bytes([head & 0xFF, (sec & 0x3F) | ((cyl >> 2) & 0xC0), cyl & 0xFF])
    struct.pack_into("<I", e, 8, lba)
    struct.pack_into("<I", e, 12, sectors)
    return bytes(e)


def build(build_dir, out):
    bd = Path(build_dir)
    efi = (bd / "BOOTX64.EFI").read_bytes()
    kernel = (bd / "kernel.elf").read_bytes()

    def clusters(data):
        return (len(data) + SECTOR - 1) // SECTOR

    n_efi = clusters(efi)
    n_kernel = clusters(kernel)
    c_efi = C_FIRST
    c_kernel = c_efi + n_efi
    last = c_kernel + n_kernel - 1

    fat_cap = (FAT_SECTORS * SECTOR) // 2
    data_sectors = (PART_SECTORS - (DATA_LBA - PART_START)) // SPC
    if (last + 1) > fat_cap:
        raise SystemExit(f"ESP: clusters {last + 1} > FAT capacity {fat_cap}")
    if (last - 1) > data_sectors:
        raise SystemExit("ESP: files do not fit in data area")

    fat = [0] * fat_cap
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF
    for c in (C_EFI, C_BOOT):
        fat[c] = 0xFFFF

    def chain(start, n):
        for i in range(n):
            fat[start + i] = start + i + 1 if i < n - 1 else 0xFFFF

    chain(c_efi, n_efi)
    chain(c_kernel, n_kernel)

    def blobs(data):
        out_blobs = []
        pos = 0
        while pos < len(data):
            out_blobs.append(data[pos:pos + SECTOR].ljust(SECTOR, b"\x00"))
            pos += SECTOR
        return out_blobs

    efi_blobs = blobs(efi)
    kernel_blobs = blobs(kernel)

    efi_dir = bytearray(SECTOR)
    efi_dir[0:32] = _entry(".", ATTR_DIR, C_EFI)
    efi_dir[32:64] = _entry("..", ATTR_DIR, 0)
    efi_dir[64:96] = _entry("BOOT", ATTR_DIR, C_BOOT)

    boot_dir = bytearray(SECTOR)
    boot_dir[0:32] = _entry(".", ATTR_DIR, C_BOOT)
    boot_dir[32:64] = _entry("..", ATTR_DIR, C_EFI)
    boot_dir[64:96] = _entry("BOOTX64.EFI", ATTR_FILE, c_efi, len(efi))

    root = bytearray(ROOT_SECTORS * SECTOR)
    root[0:32] = _entry("EFI", ATTR_DIR, C_EFI)
    root[32:64] = _entry("KERNEL.ELF", ATTR_FILE, c_kernel, len(kernel))

    mbr = bytearray(SECTOR)
    mbr[446:462] = _disk_entry(PART_START, PART_SECTORS)
    mbr[510:512] = b"\x55\xAA"

    def clba(cluster):
        return DATA_LBA - PART_START + (cluster - 2) * SPC

    def at(rel):
        return (PART_START + rel) * SECTOR

    with open(out, "wb") as f:
        f.truncate((PART_START + PART_SECTORS) * SECTOR)
        f.seek(0)
        f.write(bytes(mbr))
        f.seek(PART_START * SECTOR)
        f.write(_boot_sector(PART_SECTORS))
        for copy in range(FAT_COUNT):
            f.seek(at(RESERVED + copy * FAT_SECTORS))
            raw = bytearray(FAT_SECTORS * SECTOR)
            for i, val in enumerate(fat):
                struct.pack_into("<H", raw, i * 2, val)
            f.write(bytes(raw))
        f.seek(at(ROOT_LBA - PART_START))
        f.write(bytes(root))
        f.seek(at(clba(C_EFI)))
        f.write(bytes(efi_dir))
        f.seek(at(clba(C_BOOT)))
        f.write(bytes(boot_dir))
        f.seek(at(clba(c_efi)))
        for blob in efi_blobs:
            f.write(blob)
        f.seek(at(clba(c_kernel)))
        for blob in kernel_blobs:
            f.write(blob)
        f.flush()

    print(f"OK: {out} ESP image")
    print(f"  MBR 0xEF @{PART_START}+{PART_SECTORS}  FAT16 data@{DATA_LBA}")
    print(f"  \\EFI\\BOOT\\BOOTX64.EFI {len(efi)}B/{n_efi}cl @cluster {c_efi}")
    print(f"  \\KERNEL.ELF {len(kernel)}B/{n_kernel}cl @cluster {c_kernel}")


if __name__ == "__main__":
    import sys
    bdir = sys.argv[1] if len(sys.argv) > 1 else "build"
    outp = sys.argv[2] if len(sys.argv) > 2 else "esp.img"
    build(bdir, outp)