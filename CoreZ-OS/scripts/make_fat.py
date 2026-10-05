"""生成 P1 FAT16 启动分区的扇区数据。

布局 (绝对 LBA, 相对 P1 起始 2048):
  +0(2048)   VBR (vbr.bin)                    1 扇区
  +1(2049)   FAT1                         256 扇区
  +257       FAT2                         256 扇区
  +513       root dir (512 项)              32 扇区   -> ROOT_LBA=2561
  +545(2593) data, 簇从 2 开始, SPC=1                -> DATA_LBA=2593

FAT 项: 0=0xFFF8(介质), 1=0xFFFF(保留), 文件链最后一簇=0xFFFF(EOC)。
根目录项: LOADER.BIN, KERNEL.BIN (8.3 大写, 目录项偏移 26=起始簇, 28=大小低, 14=大小高)。
"""
from pathlib import Path
import struct

SECTOR = 512
PART_START = 2048
RESERVED = 1
FAT_SECTORS = 256
FAT_COUNT = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = ROOT_ENTRIES * 32 // SECTOR
SPC = 1
P1_TOTAL_SECTORS = 16384

ROOT_LBA = PART_START + RESERVED + FAT_COUNT * FAT_SECTORS
DATA_LBA = ROOT_LBA + ROOT_SECTORS


def fat16(build_dir: str, out: str) -> None:
    bd = Path(build_dir)
    vbr = (bd / "vbr.bin").read_bytes()
    loader = (bd / "loader.bin").read_bytes()
    kernel = (bd / "kernel.bin").read_bytes()

    if len(vbr) > SECTOR:
        raise SystemExit(f"vbr.bin too big: {len(vbr)} > {SECTOR}")
    if len(vbr) & 0x1FF:
        vbr += b"\x00" * (SECTOR - (len(vbr) & 0x1FF))
    vbr = vbr[:SECTOR]
    vbr = vbr[:-2] + b"\x55\xAA"

    def clusters(data):
        return (len(data) + SECTOR - 1) // SECTOR

    n_loader = clusters(loader)
    n_kernel = clusters(kernel)
    cloader = 2
    ckernel = cloader + n_loader

    total_clusters = (n_loader + n_kernel)
    fat_entries = [0] * (total_clusters + 2)
    fat_entries[0] = 0xFFF8
    fat_entries[1] = 0xFFFF

    def chain(start, n):
        for i in range(n):
            fat_entries[start + i] = start + i + 1
        fat_entries[start + n - 1] = 0xFFFF

    chain(cloader, n_loader)
    chain(ckernel, n_kernel)

    def put_file(start_cluster, data):
        blobs = []
        pos = 0
        while pos < len(data):
            blobs.append(data[pos:pos + SECTOR].ljust(SECTOR, b'\x00'))
            pos += SECTOR
        return blobs

    loader_blobs = put_file(cloader, loader)
    kernel_blobs = put_file(ckernel, kernel)

    def root_entry(name, start_cluster, size):
        e = bytearray(32)
        base, _, ext = name.partition(".")
        nb = (base + " " * 8)[:8].encode()
        eb = (ext + " " * 3)[:3].encode()
        e[0:8] = nb
        e[8:11] = eb
        e[11] = 0x20
        struct.pack_into("<H", e, 26, start_cluster)
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    root = bytearray(ROOT_SECTORS * SECTOR)
    root[0:32] = root_entry("LOADER.BIN", cloader, len(loader))
    root[32:64] = root_entry("KERNEL.BIN", ckernel, len(kernel))

    with open(out, "wb") as f:
        f.truncate(P1_TOTAL_SECTORS * SECTOR)
        f.seek(0)
        f.write(vbr)

        for copy in range(FAT_COUNT):
            f.seek((RESERVED + copy * FAT_SECTORS) * SECTOR)
            fat = bytearray(FAT_SECTORS * SECTOR)
            for i, val in enumerate(fat_entries):
                struct.pack_into("<H", fat, i * 2, val)
            f.write(bytes(fat))

        f.seek((ROOT_LBA - PART_START) * SECTOR)
        f.write(bytes(root))

        f.seek((DATA_LBA - PART_START) * SECTOR)
        for blob in (loader_blobs + kernel_blobs):
            f.write(blob)

    print(f"OK: {out} FAT16 builder")
    print(f"  P1 @{PART_START}: VBR(1) FAT({FAT_SECTORS}x{FAT_COUNT}) "
          f"root({ROOT_SECTORS}) data@{DATA_LBA}")
    print(f"  LOADER.BIN {len(loader)}B/{n_loader}cl @cluster {cloader}")
    print(f"  KERNEL.BIN {len(kernel)}B/{n_kernel}cl @cluster {ckernel}")
    if n_loader + n_kernel + 2 > (FAT_SECTORS * SECTOR) // 2:
        raise SystemExit("FAT too small for file clusters")
    if (n_loader + n_kernel) > (P1_TOTAL_SECTORS - (DATA_LBA - PART_START)):
        raise SystemExit("files do not fit in data area")

F32_RESERVED = 2        
F32_FAT_SECTORS = 127  
F32_FAT_COUNT = 2
F32_SPC = 1              
F32_ROOT_CLUSTER = 2      

F32_FAT_LBA = PART_START + F32_RESERVED
F32_DATA_LBA = F32_FAT_LBA + F32_FAT_COUNT * F32_FAT_SECTORS
F32_EOC = 0x0FFFFFFF


def _fsinfo():
    b = bytearray(SECTOR)
    b[0:4] = b"\x52\x52\x61\x41"  
    b[484:488] = b"\x72\x72\x61\x41" 
    struct.pack_into("<I", b, 488, 0xFFFFFFFF) 
    struct.pack_into("<I", b, 492, 0xFFFFFFFF) 
    b[508:512] = bytes([0x00, 0x00, 0x55, 0xAA])
    return bytes(b)


def fat32(build_dir: str, out: str) -> None:
    bd = Path(build_dir)
    vbr = (bd / "vbr.bin").read_bytes()
    loader = (bd / "loader.bin").read_bytes()
    kernel = (bd / "kernel.bin").read_bytes()

    if len(vbr) > SECTOR:
        raise SystemExit(f"vbr.bin too big: {len(vbr)} > {SECTOR}")
    vbr = vbr[:SECTOR].ljust(SECTOR, b"\x00")
    vbr = vbr[:-2] + b"\x55\xAA"

    def clusters(data):
        return (len(data) + F32_SPC * SECTOR - 1) // (F32_SPC * SECTOR)

    n_loader = clusters(loader)
    n_kernel = clusters(kernel)

    c_loader = 3
    c_kernel = c_loader + n_loader
    last_used = c_kernel + n_kernel - 1

    fat_cap = (F32_FAT_SECTORS * SECTOR) // 4
    if (last_used + 1) > fat_cap:
        raise SystemExit(
            f"FAT32: data needs {last_used + 1} entries > FAT capacity {fat_cap}")
    if (last_used - 1) > (P1_TOTAL_SECTORS - (F32_DATA_LBA - PART_START)):
        raise SystemExit("FAT32: files do not fit in data area")

    fat = bytearray(F32_FAT_SECTORS * SECTOR)

    def set_fat(clu, val):
        struct.pack_into("<I", fat, clu * 4, val & 0xFFFFFFFF)

    set_fat(0, 0x0FFFFFF8)
    set_fat(1, 0x0FFFFFFF)

    def chain(start, n):
        for i in range(n):
            nxt = start + i + 1 if i < n - 1 else F32_EOC
            set_fat(start + i, nxt)

    set_fat(F32_ROOT_CLUSTER, F32_EOC)
    chain(c_loader, n_loader)
    chain(c_kernel, n_kernel)

    def put_file(start_cluster, data, blk=None):
        blobs = []
        pos = 0
        while pos < len(data):
            blobs.append(data[pos:pos + SECTOR].ljust(SECTOR, b"\x00"))
            pos += SECTOR
        return blobs

    loader_blobs = put_file(c_loader, loader)
    kernel_blobs = put_file(c_kernel, kernel)

    def root_entry(name, start_cluster, size):
        e = bytearray(32)
        base, _, ext = name.partition(".")
        nb = (base + " " * 8)[:8].encode()
        eb = (ext + " " * 3)[:3].encode()
        e[0:8] = nb
        e[8:11] = eb
        e[11] = 0x20 
        struct.pack_into("<H", e, 20, (start_cluster >> 16) & 0xFFFF)  
        struct.pack_into("<H", e, 26, start_cluster & 0xFFFF)        
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    root_dir = bytearray(SECTOR)
    root_dir[0:32] = root_entry("LOADER.BIN", c_loader, len(loader))
    root_dir[32:64] = root_entry("KERNEL.BIN", c_kernel, len(kernel))

    with open(out, "wb") as f:
        f.truncate(P1_TOTAL_SECTORS * SECTOR)
        f.seek(0)
        f.write(vbr)
        f.seek(1 * SECTOR)
        f.write(_fsinfo())
        for copy in range(F32_FAT_COUNT):
            f.seek((F32_RESERVED + copy * F32_FAT_SECTORS) * SECTOR)
            f.write(bytes(fat))

        f.seek((F32_DATA_LBA - PART_START) * SECTOR)
        f.write(bytes(root_dir))

        f.seek((F32_DATA_LBA - PART_START + 1) * SECTOR)
        for blob in loader_blobs:
            f.write(blob)
        f.seek((F32_DATA_LBA - PART_START + 1 + n_loader) * SECTOR)
        for blob in kernel_blobs:
            f.write(blob)

    print(f"OK: {out} FAT32 builder")
    print(f"  P1 @{PART_START}: VBR({F32_RESERVED}) FAT({F32_FAT_SECTORS}x{F32_FAT_COUNT}) "
          f"data@{F32_DATA_LBA}  root@clu{F32_ROOT_CLUSTER}  SPC={F32_SPC}")
    print(f"  LOADER.BIN {len(loader)}B/{n_loader}cl @cluster {c_loader}")
    print(f"  KERNEL.BIN {len(kernel)}B/{n_kernel}cl @cluster {c_kernel}")


if __name__ == "__main__":
    import sys
    fmt = sys.argv[1] if len(sys.argv) > 1 else "build"
    out = sys.argv[2] if len(sys.argv) > 2 else "fat.img"
    if fmt == "fat16":
        fat16(sys.argv[2] if len(sys.argv) > 2 else "build", out)
    else:
        fat32(sys.argv[1] if len(sys.argv) > 1 else "build", out)
