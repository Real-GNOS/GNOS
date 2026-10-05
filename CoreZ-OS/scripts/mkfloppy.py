import sys
import struct

SECTOR_SIZE = 512
TOTAL_SECTORS = 2880

FAT1_START = 1                 
FAT2_START = 10               
SECTORS_PER_FAT = 9
ROOT_DIR_START = 19             
ROOT_DIR_SECTORS = 14          
DATA_START = 33                
LOADER_OFFSET = DATA_START * SECTOR_SIZE  


def make_fat12():
    fat = bytearray(SECTORS_PER_FAT * SECTOR_SIZE)
    fat[0] = 0xF0  
    fat[1] = 0xFF
    fat[2] = 0xFF
    return fat


def fat12_set_entry(fat, cluster, value):
    offset = cluster * 3 // 2
    if cluster % 2 == 0:
        fat[offset] = value & 0xFF
        fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F)
    else:
        fat[offset] = (fat[offset] & 0x0F) | ((value & 0x0F) << 4)
        fat[offset + 1] = (value >> 4) & 0xFF


def make_dir_entry(name, ext, attr, first_cluster, file_size):
    entry = bytearray(32)
    entry[0:8] = name.upper().ljust(8).encode('ascii')[:8]
    entry[8:11] = ext.upper().ljust(3).encode('ascii')[:3]
    entry[11] = attr
    struct.pack_into('<H', entry, 0x1A, first_cluster)
    struct.pack_into('<I', entry, 0x1C, file_size)
    struct.pack_into('<H', entry, 0x18, 0x5821)
    struct.pack_into('<H', entry, 0x16, 0x0000)
    return entry


def split_filename(filepath):
    import os
    basename = os.path.basename(filepath)
    if '.' in basename:
        name, ext = basename.rsplit('.', 1)
    else:
        name = basename
        ext = ''
    return name[:8], ext[:3]


def create_floppy(boot_path, loader_path, kernel_path, output_path, extra_files=None):
    with open(boot_path, "rb") as f:
        boot = f.read()
    with open(loader_path, "rb") as f:
        loader = f.read()
    with open(kernel_path, "rb") as f:
        kernel = f.read()

    files = []
    if extra_files:
        for filepath in extra_files:
            with open(filepath, "rb") as f:
                data = f.read()
            name, ext = split_filename(filepath)
            files.append((name, ext, data))
            print(f"  Extra file: {name}.{ext} ({len(data)} bytes)")

    if len(boot) < SECTOR_SIZE:
        boot = boot + b"\x00" * (SECTOR_SIZE - len(boot))
    else:
        boot = boot[:SECTOR_SIZE]

    img = bytearray(TOTAL_SECTORS * SECTOR_SIZE)

    img[0:SECTOR_SIZE] = boot

    fat = make_fat12()

    loader_size = len(loader)
    kernel_size = len(kernel)

    test_content = b"Hello World\r\n"
    test_size = len(test_content)

    loader_clusters = (loader_size + SECTOR_SIZE - 1) // SECTOR_SIZE
    kernel_clusters = (kernel_size + SECTOR_SIZE - 1) // SECTOR_SIZE
    test_clusters = 1

    loader_start = 2
    kernel_start = loader_start + loader_clusters
    test_start = kernel_start + kernel_clusters

    extra_starts = []
    current_cluster = test_start + test_clusters
    for name, ext, data in files:
        clusters = (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE
        extra_starts.append((current_cluster, clusters))
        current_cluster += clusters

    for i in range(loader_clusters):
        c = loader_start + i
        next_c = c + 1 if i < loader_clusters - 1 else 0xFFF
        fat12_set_entry(fat, c, next_c)

    for i in range(kernel_clusters):
        c = kernel_start + i
        next_c = c + 1 if i < kernel_clusters - 1 else 0xFFF
        fat12_set_entry(fat, c, next_c)

    fat12_set_entry(fat, test_start, 0xFFF)

    for (start_cluster, clusters), (name, ext, data) in zip(extra_starts, files):
        for i in range(clusters):
            c = start_cluster + i
            next_c = c + 1 if i < clusters - 1 else 0xFFF
            fat12_set_entry(fat, c, next_c)

    fat1_off = FAT1_START * SECTOR_SIZE
    fat2_off = FAT2_START * SECTOR_SIZE
    img[fat1_off:fat1_off + len(fat)] = fat
    img[fat2_off:fat2_off + len(fat)] = fat

    root = bytearray(ROOT_DIR_SECTORS * SECTOR_SIZE)

    root[0:32] = make_dir_entry("LOADER", "BIN", 0x20, loader_start, loader_size)
    root[32:64] = make_dir_entry("KERNEL", "BIN", 0x20, kernel_start, kernel_size)
    root[64:96] = make_dir_entry("HELLO", "TXT", 0x20, test_start, test_size)

    for i, ((start_cluster, clusters), (name, ext, data)) in enumerate(zip(extra_starts, files)):
        offset = 96 + i * 32
        root[offset:offset + 32] = make_dir_entry(name, ext, 0x20, start_cluster, len(data))

    root_off = ROOT_DIR_START * SECTOR_SIZE
    img[root_off:root_off + len(root)] = root

    # NOTE: 不要 patch loader.bin 的前 4 字节。
    # 之前这里把 loader 的 kernel_addr 改成 0x8000+OFFSET+size,
    # 会让 loader 的 memcpy 从错误地址拷贝, 内核被垃圾数据覆盖 -> 三重故障。
    # loader.asm 里 kernel_addr 已经是 0x00010000, 直接用就好。
    loader_off = DATA_START * SECTOR_SIZE + (loader_start - 2) * SECTOR_SIZE
    img[loader_off:loader_off + len(loader)] = loader

    kernel_off = DATA_START * SECTOR_SIZE + (kernel_start - 2) * SECTOR_SIZE
    img[kernel_off:kernel_off + len(kernel)] = kernel

    test_off = DATA_START * SECTOR_SIZE + (test_start - 2) * SECTOR_SIZE
    img[test_off:test_off + len(test_content)] = test_content

    for (start_cluster, clusters), (name, ext, data) in zip(extra_starts, files):
        file_off = DATA_START * SECTOR_SIZE + (start_cluster - 2) * SECTOR_SIZE
        img[file_off:file_off + len(data)] = data

    print(f"FAT12 Floppy Image Builder")
    print(f"  Boot sector:   sector 0")
    print(f"  FAT1:          sectors {FAT1_START}-{FAT1_START + SECTORS_PER_FAT - 1}")
    print(f"  FAT2:          sectors {FAT2_START}-{FAT2_START + SECTORS_PER_FAT - 1}")
    print(f"  Root dir:      sectors {ROOT_DIR_START}-{ROOT_DIR_START + ROOT_DIR_SECTORS - 1}")
    print(f"  Data area:     sector {DATA_START}+")
    print(f"  LOADER.BIN:    cluster {loader_start}, {loader_size} bytes ({loader_clusters} clusters)")
    print(f"  KERNEL.BIN:    cluster {kernel_start}, {kernel_size} bytes ({kernel_clusters} clusters)")
    print(f"  HELLO.TXT:     cluster {test_start}, {test_size} bytes")
    for (start_cluster, clusters), (name, ext, data) in zip(extra_starts, files):
        print(f"  {name}.{ext.upper()}:   cluster {start_cluster}, {len(data)} bytes ({clusters} clusters)")
    print(f"  Kernel load addr: 0x00010000 (loader 内核加载点)")

    with open(output_path, "wb") as f:
        f.write(img)

    print(f"\nOutput: {output_path}")
    print("OK!")


if __name__ == "__main__":
    if len(sys.argv) < 5:
        print(f"Usage: python {sys.argv[0]} <boot.bin> <loader.bin> <kernel.bin> <output.img> [extra_files...]")
        sys.exit(1)
    boot_path = sys.argv[1]
    loader_path = sys.argv[2]
    kernel_path = sys.argv[3]
    output_path = sys.argv[4]
    extra_files = sys.argv[5:] if len(sys.argv) > 5 else None
    create_floppy(boot_path, loader_path, kernel_path, output_path, extra_files)
