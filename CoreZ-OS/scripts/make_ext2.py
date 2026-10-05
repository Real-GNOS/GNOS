import struct
import sys
from pathlib import Path

import make_fat

SECTOR = 512
TOTAL_SECTORS = 80 * 1024 * 1024 // SECTOR
P2_START = 18432

EXT2_SUPER_MAGIC = 0xEF53
BLOCK = 1024
SECT_PER_BLOCK = BLOCK // SECTOR
INODES_PER_GROUP = 1024
INODE_SIZE = 128
FIRST_DATA_BLOCK = 1

TOTAL_BLOCKS = (TOTAL_SECTORS - P2_START) // SECT_PER_BLOCK

SUPER_BLK = 1
GDT_BLK = FIRST_DATA_BLOCK + 1
BLOCK_BITMAP_BLK = 3
BLOCK_BITMAP_BLOCKS = (TOTAL_BLOCKS + 8 * BLOCK - 1) // (8 * BLOCK)
INODE_BITMAP_BLK = BLOCK_BITMAP_BLK + BLOCK_BITMAP_BLOCKS
ITABLE_BLK = INODE_BITMAP_BLK + 1
ITABLE_BLOCKS = (INODES_PER_GROUP * INODE_SIZE + BLOCK - 1) // BLOCK
DATA_START = ITABLE_BLK + ITABLE_BLOCKS

FILES = [
    "prog_no_arg.elf", "prog_arg.elf", "cat.elf", "fork_demo.elf",
    "prog_pipe.elf", "font_demo.elf", "heap_demo.elf", "signal_demo.elf",
    "orphan.elf", "tls_test.elf", "sig_test.elf", "badptr_test.elf",
    "cow_stress.elf",
    "cwd_test.elf",
    "echocat.elf",
    "canary_test.elf", "clone_stress.elf", "clone_demo.elf", "path_probe.elf", "kaddr_probe.elf", "sock_probe.elf", "init_sh.elf", "gs_probe.elf", "futex_probe.elf", "eintr_probe.elf", "sel_probe.elf",
    "font_subset.ttf", "ping.elf",
    "lc_demo.elf", "libc_testsuite.elf", "musl_demo.elf", "udp_echo.elf",
    "musl_abi_test.elf", "dev_demo.elf", "gui.elf", "toybox", "dyn_demo.elf",
    "py_compat_probe.elf", "sh.elf", "cpp_hello.elf", "termios_probe.elf",
    "pty_demo.elf", "jc_demo.elf", "pcre2_demo.elf", "at_probe.elf",
    "futex_bs_probe.elf",
    "rust_hello.elf", "rust_probe.elf", "rust_probe2.elf", "fish.elf",
    "t.fish", "shell.elf",
    "wallpaper.png", "pic1.png", "pic2.png", "win_main.exe", "win_gui.exe"
]
ALIASES = {"forktest.elf": "fork_demo.elf", "suidsh": "toybox"}
SPECIAL_MODES = {"suidsh": 0x81ED | 0o4000}


def part_entry(bootable, fs_type, start_lba, sec_cnt):
    return struct.pack("<BBBBBBBBII", bootable, 0, 0, 0, fs_type, 0, 0, 0,
                       start_lba & 0xFFFFFFFF, sec_cnt & 0xFFFFFFFF)


def make_mbr(boot_bin):
    p1 = part_entry(0x80, 0x0C, make_fat.PART_START,
                    make_fat.P1_TOTAL_SECTORS)
    p2 = part_entry(0x00, 0x83, P2_START,
                    TOTAL_SECTORS - P2_START)
    mbr = bytearray(boot_bin)
    if len(mbr) < SECTOR:
        mbr += b"\x00" * (SECTOR - len(mbr))
    mbr = mbr[:SECTOR]
    mbr[446:446 + 16] = p1
    mbr[446 + 16:446 + 32] = p2
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def build_dirent_blocks(entries, block=BLOCK):
    out = bytearray()
    n = len(entries)
    for i, (ino, ftype, name) in enumerate(entries):
        nl = len(name)
        reclen = (8 + nl + 3) & ~3
        if len(out) % block + reclen > block:
            out += bytearray(block - len(out) % block)
        if i == n - 1:
            reclen = block - len(out) % block
        out += struct.pack("<IHBB", ino, reclen, nl, ftype)
        out += name.encode()
        out += bytearray((-len(out)) % 4)
    out += bytearray(block - len(out) % block)
    return bytes(out)


DEV_NODES = [("null", 1, 3), ("zero", 1, 5), ("tty", 5, 0),
             ("console", 5, 1), ("random", 1, 8), ("urandom", 1, 9),
             ("ptmx", 136, 0),
             ("pty0", 137, 0), ("pty1", 137, 1), ("pty2", 137, 2),
             ("pty3", 137, 3), ("pty4", 137, 4), ("pty5", 137, 5),
             ("pty6", 137, 6), ("pty7", 137, 7)]

EXTRA_DIRS = [("etc", 0o40755), ("home", 0o40755), ("bin", 0o40755),
              ("tmp", 0x41ED | 0o777), ("lib", 0o40755), ("root", 0o40755)]

BIN_LINKS = ["sh", "su", "login", "id", "ls", "cat", "echo", "ps", "passwd",
             "adduser", "groups", "chmod", "chown", "mkdir", "rm", "cp",
             "env", "uname", "free", "setsid", "hostname", "date", "clear",
             "dmesg", "kill"]

ETC_FILES = [
    ("passwd",
     b"root:x:0:0:root:/:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n",
     0x81A4 & ~0o777 | 0o644, 0, 0),
    ("group", b"root:x:0:\nuser:x:1000:\n", 0x81A4 & ~0o777 | 0o644, 0, 0),
    ("shadow", b"root::0:0:99999:7:::\nuser::0:0:99999:7:::\n",
     0x81A4 & ~0o777 | 0o600, 0, 0),
    ("hosts",
     b"127.0.0.1 localhost localhost.localdomain\n",
     0x81A4 & ~0o777 | 0o644, 0, 0),
    ("services",
     b"ftp 21/tcp\nssh 22/tcp\ndomain 53/udp\ndomain 53/tcp\n"
     b"http 80/tcp\nhttps 443/tcp\n",
     0x81A4 & ~0o777 | 0o644, 0, 0),
]

# /lib：动态链接器与共享库。libc.so 在 x86_64 上同时就是 ld-musl-x86_64.so.1，
# 两个名字都要有（程序按 PT_INTERP 找后者，ld.so 按 DT_NEEDED 找前者）
LIB_DIR = "lib"
LIB_FILES = ["ld-musl-x86_64.so.1", "libc.so", "libdyndemo.so"]


SYMLINKS = [("catlink", "/cat.elf"),
            ("longlink", "/cat.elf" + "/sub/dir/padding/xyz" * 3)]

# 冒烟启动脚本：由 init_sh 逐行执行。可用 build(autoexec=...) 覆盖，便于单独
# 调试某个程序（默认这份是回归用的完整序列）
SMOKE_AUTOEXEC = (b"mkdir /tmp/dw\nls /tmp\nrmdir /tmp/dw\nls /tmp\n"
                  b"toybox ls -l /lib\n"
                  b"at_probe.elf\nfutex_bs_probe.elf\n"
                  b"rust_hello.elf\nrust_probe.elf\n"
                  b"musl_abi_test.elf\n"
                  b"dyn_demo.elf\n"
                  b"fork_demo.elf\ncow_stress.elf\nfork_demo.elf\n"
                  b"dev_demo.elf\npcre2_demo.elf\ntoybox cat /proc/meminfo\n"
                  b"toybox echo TOYBOX_ECHO_OK\n"
                  b"toybox id\ntoybox ls -l /etc/passwd\n"
                  b"toybox su user -c id\ntoybox cat /proc/self/status\n"
                  b"toybox ls /\n")


def put_symlink(table, ino, target, block):
    off = (ino - 1) * INODE_SIZE
    struct.pack_into("<H", table, off + 0, 0xA1FF)
    struct.pack_into("<I", table, off + 4, len(target))
    if len(target) < 60:
        table[off + 40:off + 40 + len(target)] = target.encode()
    else:
        struct.pack_into("<I", table, off + 40, block)


def alloc_file_blocks(cur_block, payload, var_blocks, var_indirect):
    """分配 payload 的数据块并填好 inode 块指针。

    @param cur_block    起始空闲块号
    @param payload      文件内容
    @param var_blocks   输出，追加 (数据块列表, 内容)
    @param var_indirect 输出，追加 (间接块, 块号列表)
    @returns (15 个块指针, 新的 cur_block)

    @remarks
    超过 12 块要补一级间接（ptrs[12]）与二级间接（ptrs[13]），共享库这类
    几百 KB 的文件必然走到这条路径：只填直接块会让 i_size 与实际块图不一致，
    内核按 i_size 读到的后半段全是零
    """
    nblk = (len(payload) + BLOCK - 1) // BLOCK
    ptrs = [0] * 15
    blocks = list(range(cur_block, cur_block + nblk))
    cur_block += nblk
    if nblk <= 12:
        ptrs[0:nblk] = blocks
    else:
        ptrs[0:12] = blocks[0:12]
        n_single = min(nblk - 12, 256)
        n_double = nblk - 12 - n_single
        if n_single:
            sing = cur_block
            cur_block += 1
            ptrs[12] = sing
            var_indirect.append((sing, blocks[12:12 + n_single]))
        if n_double:
            dbl = cur_block
            cur_block += 1
            n_sub = (n_double + 255) // 256
            subs = list(range(cur_block, cur_block + n_sub))
            cur_block += n_sub
            ptrs[13] = dbl
            var_indirect.append((dbl, subs))
            for k, sb in enumerate(subs):
                lo = 12 + n_single + k * 256
                var_indirect.append((sb, blocks[lo:lo + 256]))
    var_blocks.append((blocks, payload))
    return ptrs, cur_block


def put_inode(table, ino, payload_len, blocks, is_dir, rdev=0, uid=0, gid=0,
              mode=None):
    off = (ino - 1) * INODE_SIZE
    m = mode if mode is not None else (0x41ED if is_dir else
                                       (0x21B6 if rdev else 0x81ED))
    struct.pack_into("<H", table, off + 0, m)
    struct.pack_into("<H", table, off + 2, uid)
    struct.pack_into("<H", table, off + 24, gid)
    struct.pack_into("<I", table, off + 4, payload_len)
    struct.pack_into("<H", table, off + 26, 2)
    for i in range(15):
        b = blocks[i] if i < len(blocks) else 0
        struct.pack_into("<I", table, off + 40 + 4 * i, b)
    if rdev:
        struct.pack_into("<I", table, off + 40, rdev)


def build(build_dir, out, smoke=False, autoexec=None):
    bd = Path(build_dir)
    boot_bin = (bd / "boot.bin").read_bytes()
    loader_bin = (bd / "loader.bin").read_bytes()
    kernel_bin = (bd / "kernel.bin").read_bytes()

    names = list(FILES)
    names += [a for a in ALIASES if (bd / a).exists()]

    pre = {}
    for name in names:
        src = bd / (ALIASES.get(name, name))
        if src.exists():
            pre[name] = src.read_bytes()
    names = [n for n in names if n in pre]
    if smoke:
        pre["autoexec"] = SMOKE_AUTOEXEC if autoexec is None else autoexec
        names.append("autoexec")
        pre["shell.conf"] = b"/init_sh.elf\n"
        names.append("shell.conf")
    else:
        pre["autoexec"] = b"toybox login\n"
        names.append("autoexec")

    ino_map = {}
    next_ino = 3
    lib_files = []
    for name in LIB_FILES:
        src = bd / name
        if src.exists():
            lib_files.append((name, src.read_bytes(), 0o100755, 0, 0))
    subdir_files = [("etc", list(ETC_FILES)), (LIB_DIR, lib_files)]
    dir_entries = [(2, 2, "."), (2, 2, "..")]
    for name in names:
        ino_map[name] = next_ino
        dir_entries.append((next_ino, 1, name))
        next_ino += 1

    link_ino = next_ino
    next_ino += len(SYMLINKS)
    link_blk_list = []
    for i, (_, tgt) in enumerate(SYMLINKS):
        dir_entries.append((link_ino + i, 7, SYMLINKS[i][0]))
        link_blk_list.append(0)

    dev_ino = next_ino
    next_ino += 1 + len(DEV_NODES)
    dev_entries = [(dev_ino, 2, "."), (2, 2, "..")]
    for i, (name, maj, mnr) in enumerate(DEV_NODES):
        dev_entries.append((dev_ino + 1 + i, 3, name))
    dir_entries.append((dev_ino, 2, "dev"))

    dir_inos = {}
    for name, _ in EXTRA_DIRS:
        dir_inos[name] = next_ino
        dir_entries.append((next_ino, 2, name))
        next_ino += 1
    bin_ino = dir_inos["bin"]
    bin_entries = [(bin_ino, 2, "."), (2, 2, "..")]
    bin_link_inos = {}
    for name in BIN_LINKS:
        bin_link_inos[name] = next_ino
        bin_entries.append((next_ino, 7, name))
        next_ino += 1
    subdir_entries = {}
    subdir_inos = {}
    for dirname, files in subdir_files:
        dino = dir_inos[dirname]
        entries = [(dino, 2, "."), (2, 2, "..")]
        for name, payload, mode, uid, gid in files:
            subdir_inos[(dirname, name)] = (next_ino, payload, mode, uid, gid)
            entries.append((next_ino, 1, name))
            next_ino += 1
        subdir_entries[dirname] = entries

    used_inodes = next_ino - 1

    root_block = DATA_START
    root_dir = build_dirent_blocks(dir_entries)
    n_root_blks = len(root_dir) // BLOCK
    dev_dir = build_dirent_blocks(dev_entries)

    cur_block = DATA_START + n_root_blks
    file_ptrs = {}
    var_blocks = []
    var_indirect = []
    for name in names:
        ptrs, cur_block = alloc_file_blocks(cur_block, pre[name], var_blocks,
                                            var_indirect)
        file_ptrs[name] = ptrs

    for i, (_, tgt) in enumerate(SYMLINKS):
        if len(tgt) >= 60:
            link_blk_list[i] = cur_block
            cur_block += 1

    dir_blk_list = {}
    for name, _ in EXTRA_DIRS:
        dir_blk_list[name] = cur_block
        cur_block += 1
    bin_dir_block = cur_block
    cur_block += 1
    subdir_dir_blocks = {}
    subdir_ptrs = {}
    for dirname, files in subdir_files:
        subdir_dir_blocks[dirname] = cur_block
        cur_block += 1
        for name, payload, mode, uid, gid in files:
            ptrs, cur_block = alloc_file_blocks(cur_block, payload, var_blocks,
                                                var_indirect)
            subdir_ptrs[(dirname, name)] = ptrs
    dev_dir_block = cur_block
    cur_block += 1

    itable = bytearray(ITABLE_BLOCKS * BLOCK)
    put_inode(itable, 2, len(root_dir),
              [root_block + i for i in range(n_root_blks)], True)
    for name, dmode in EXTRA_DIRS:
        put_inode(itable, dir_inos[name], BLOCK, [dir_blk_list[name]], True,
                  mode=dmode)
    put_inode(itable, bin_ino, BLOCK, [bin_dir_block], True, mode=0o40755)
    for dirname, entries in subdir_entries.items():
        put_inode(itable, dir_inos[dirname], BLOCK,
                  [subdir_dir_blocks[dirname]], True, mode=0o40755)
    for name in BIN_LINKS:
        tgt = b"/sh.elf" if name == "sh" else b"/toybox"
        off = (bin_link_inos[name] - 1) * INODE_SIZE
        struct.pack_into("<H", itable, off + 0, 0xA1FF)
        struct.pack_into("<I", itable, off + 4, len(tgt))
        itable[off + 40:off + 40 + len(tgt)] = tgt
    for i, (_, tgt) in enumerate(SYMLINKS):
        put_symlink(itable, link_ino + i, tgt, link_blk_list[i])
    put_inode(itable, dev_ino, BLOCK, [dev_dir_block], True)
    for i, (_, maj, mnr) in enumerate(DEV_NODES):
        put_inode(itable, dev_ino + 1 + i, 0, [], False,
                  rdev=(maj << 8) | mnr)
    for key, (ino, payload, mode, uid, gid) in subdir_inos.items():
        put_inode(itable, ino, len(payload), subdir_ptrs[key], False,
                  mode=mode, uid=uid, gid=gid)
    for name in names:
        put_inode(itable, ino_map[name], len(pre[name]),
                  file_ptrs[name], False,
                  mode=SPECIAL_MODES.get(name))

    used_blocks = set(range(0, DATA_START))
    for i in range(n_root_blks):
        used_blocks.add(root_block + i)
    for blocks, _ in var_blocks:
        used_blocks.update(blocks)
    for iblk, _ in var_indirect:
        used_blocks.add(iblk)
    used_blocks.update(b for b in link_blk_list if b)
    used_blocks.add(dev_dir_block)
    used_blocks.add(bin_dir_block)
    used_blocks.update(dir_blk_list.values())
    used_blocks.update(subdir_dir_blocks.values())
    total_blocks = (TOTAL_SECTORS - P2_START) // SECT_PER_BLOCK
    free_blocks = total_blocks - len(used_blocks)

    block_bitmap = bytearray(BLOCK_BITMAP_BLOCKS * BLOCK)
    for b in used_blocks:
        block_bitmap[b >> 3] |= 0x80 >> (b & 7)

    inode_bitmap = bytearray(BLOCK)
    for i in range(1, used_inodes + 1):
        inode_bitmap[(i - 1) >> 3] |= 0x80 >> ((i - 1) & 7)

    gdt = bytearray(BLOCK)
    struct.pack_into("<III", gdt, 0,
                     BLOCK_BITMAP_BLK, INODE_BITMAP_BLK, ITABLE_BLK)

    sb = bytearray(BLOCK)
    struct.pack_into("<IIIIIIIIIII", sb, 0,
                     INODES_PER_GROUP, total_blocks, 0, free_blocks,
                     INODES_PER_GROUP - used_inodes, FIRST_DATA_BLOCK,
                     0, 0, total_blocks, total_blocks, INODES_PER_GROUP)
    struct.pack_into("<IIHHH", sb, 44, 0, 0, 0, 0, 0) 
    struct.pack_into("<H", sb, 56, EXT2_SUPER_MAGIC)

    mbr = make_mbr(boot_bin)

    fat_img = bd / "fat32.img"
    make_fat.fat32(build_dir, str(fat_img))
    fat_data = fat_img.read_bytes()

    base = P2_START
    with open(out, "wb") as f:
        f.truncate(TOTAL_SECTORS * SECTOR)
        f.seek(0)
        f.write(mbr)
        f.seek(make_fat.PART_START * SECTOR)
        f.write(fat_data)
        f.seek(base * SECTOR + SUPER_BLK * BLOCK)
        f.write(bytes(sb))
        f.seek(base * SECTOR + GDT_BLK * BLOCK)
        f.write(bytes(gdt))
        f.seek(base * SECTOR + BLOCK_BITMAP_BLK * BLOCK)
        f.write(bytes(block_bitmap))
        f.seek(base * SECTOR + INODE_BITMAP_BLK * BLOCK)
        f.write(bytes(inode_bitmap))
        f.seek(base * SECTOR + ITABLE_BLK * BLOCK)
        f.write(bytes(itable))
        f.seek(base * SECTOR + root_block * BLOCK)
        f.write(root_dir)
        f.seek(base * SECTOR + dev_dir_block * BLOCK)
        f.write(dev_dir)
        for name, _ in EXTRA_DIRS:
            ino = dir_inos[name]
            f.seek(base * SECTOR + dir_blk_list[name] * BLOCK)
            f.write(build_dirent_blocks([(ino, 2, "."), (2, 2, "..")]))
        for dirname, entries in subdir_entries.items():
            f.seek(base * SECTOR + subdir_dir_blocks[dirname] * BLOCK)
            f.write(build_dirent_blocks(entries))
        f.seek(base * SECTOR + bin_dir_block * BLOCK)
        f.write(build_dirent_blocks(bin_entries))
        for i, (_, tgt) in enumerate(SYMLINKS):
            if link_blk_list[i]:
                f.seek(base * SECTOR + link_blk_list[i] * BLOCK)
                f.write(tgt.encode())
        for iblk, data in var_indirect:
            idx = bytearray(BLOCK)
            for j, b in enumerate(data):
                struct.pack_into("<I", idx, 4 * j, b)
            f.seek(base * SECTOR + iblk * BLOCK)
            f.write(bytes(idx))
        for blocks, payload in var_blocks:
            f.seek(base * SECTOR + blocks[0] * BLOCK)
            f.write(payload)

    print(f"OK: {out} ({TOTAL_SECTORS * SECTOR // 1024 // 1024}MB)")
    print(f"  P1 @{make_fat.PART_START} FAT32({make_fat.P1_TOTAL_SECTORS}sec) "
          f"[bootable] -> {fat_img}")
    print(f"  P2 @{P2_START} ext2: {len(names)} files, {free_blocks} free blocks")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--smoke"]
    arg1 = args[0] if len(args) > 0 else "build"
    arg2 = args[1] if len(args) > 1 else "test_hd.img"
    smoke = "--smoke" in sys.argv
    autoexec = None
    if "--autoexec" in args:
        i = args.index("--autoexec")
        autoexec = Path(args[i + 1]).read_bytes()
        del args[i:i + 2]
    build(arg1, arg2, smoke=smoke, autoexec=autoexec)
    nvme_img = Path(arg2).with_name("nvme.img")
    build(arg1, str(nvme_img), smoke=smoke, autoexec=autoexec)
    print(f"OK: {nvme_img} (nvme copy for -device nvme)")
