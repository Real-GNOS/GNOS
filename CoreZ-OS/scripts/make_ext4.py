import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import make_fat

SECTOR = 512
TOTAL_SECTORS = 80 * 1024 * 1024 // SECTOR
P2_START = 18432
P2_SECTORS = TOTAL_SECTORS - P2_START
BLOCK = 1024
INODE_SIZE = 256
MKE2FS = "mke2fs"
DEBUGFS = "debugfs"
E2FSCK = "e2fsck"

MKFS_FEATURES = ",".join([
    "extents", "64bit", "flex_bg", "metadata_csum", "has_journal",
    "extra_isize", "filetype", "sparse_super", "large_file", "huge_file",
    "dir_nlink", "ext_attr",
    "^dir_index", "^resize_inode", "^orphan_file", "^sparse_super2",
    "^ea_inode", "^large_dir", "^inline_data", "^encrypt", "^bigalloc",
    "^mmp", "^quota", "^project", "^verity",
])

FILES = [
    "prog_no_arg.elf", "prog_arg.elf", "cat.elf", "fork_demo.elf",
    "prog_pipe.elf", "font_demo.elf", "heap_demo.elf", "signal_demo.elf",
    "orphan.elf", "tls_test.elf", "sig_test.elf", "badptr_test.elf",
    "cow_stress.elf",
    "cwd_test.elf",
    "echocat.elf",
    "canary_test.elf", "clone_stress.elf", "clone_demo.elf", "path_probe.elf",
    "kaddr_probe.elf", "sock_probe.elf", "init_sh.elf", "gs_probe.elf",
    "futex_probe.elf", "eintr_probe.elf", "sel_probe.elf",
    "font_subset.ttf", "ping.elf",
    "lc_demo.elf", "libc_testsuite.elf", "musl_demo.elf", "udp_echo.elf",
    "musl_abi_test.elf", "dev_demo.elf", "gui.elf", "toybox", "dyn_demo.elf",
    "py_compat_probe.elf", "sh.elf", "cpp_hello.elf", "termios_probe.elf",
    "pty_demo.elf", "jc_demo.elf", "pcre2_demo.elf", "at_probe.elf",
    "futex_bs_probe.elf",
    "rust_hello.elf", "rust_probe.elf", "rust_probe2.elf", "fish.elf",
    "t.fish", "shell.elf",
    "wallpaper.png", "pic1.png", "pic2.png", "win_main.exe", "win_gui.exe",
]
ALIASES = {"forktest.elf": "fork_demo.elf", "suidsh": "toybox"}
SPECIAL_MODES = {"suidsh": 0o104755}
FILE_MODE = 0o100755

DEV_NODES = [("null", 1, 3), ("zero", 1, 5), ("tty", 5, 0),
             ("console", 5, 1), ("random", 1, 8), ("urandom", 1, 9),
             ("ptmx", 136, 0),
             ("pty0", 137, 0), ("pty1", 137, 1), ("pty2", 137, 2),
             ("pty3", 137, 3), ("pty4", 137, 4), ("pty5", 137, 5),
             ("pty6", 137, 6), ("pty7", 137, 7)]
CHAR_DEV_MODE = 0o20666

DIRS = [("etc", 0o40755), ("home", 0o40755), ("bin", 0o40755),
        ("tmp", 0o41777), ("lib", 0o40755), ("root", 0o40755),
        ("dev", 0o40755)]

BIN_LINKS = ["sh", "su", "login", "id", "ls", "cat", "echo", "ps", "passwd",
             "adduser", "groups", "chmod", "chown", "mkdir", "rm", "cp",
             "env", "uname", "free", "setsid", "hostname", "date", "clear",
             "dmesg", "kill"]

ETC_FILES = [
    ("passwd",
     b"root:x:0:0:root:/:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n",
     0o100644),
    ("group", b"root:x:0:\nuser:x:1000:\n", 0o100644),
    ("shadow", b"root::0:0:99999:7:::\nuser::0:0:99999:7:::\n", 0o100600),
    ("hosts", b"127.0.0.1 localhost localhost.localdomain\n", 0o100644),
    ("services",
     b"ftp 21/tcp\nssh 22/tcp\ndomain 53/udp\ndomain 53/tcp\n"
     b"http 80/tcp\nhttps 443/tcp\n", 0o100644),
]

LIB_FILES = ["ld-musl-x86_64.so.1", "libc.so", "libdyndemo.so"]

SYMLINKS = [("catlink", "/cat.elf"),
            ("longlink", "/cat.elf" + "/sub/dir/padding/xyz" * 3)]

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


def part_entry(bootable, fs_type, start_lba, sec_cnt):
    return struct.pack("<BBBBBBBBII", bootable, 0, 0, 0, fs_type, 0, 0, 0,
                       start_lba & 0xFFFFFFFF, sec_cnt & 0xFFFFFFFF)


def make_mbr(boot_bin):
    p1 = part_entry(0x80, 0x0C, make_fat.PART_START,
                    make_fat.P1_TOTAL_SECTORS)
    p2 = part_entry(0x00, 0x83, P2_START, P2_SECTORS)
    mbr = bytearray(boot_bin)
    if len(mbr) < SECTOR:
        mbr += b"\x00" * (SECTOR - len(mbr))
    mbr = mbr[:SECTOR]
    mbr[446:446 + 16] = p1
    mbr[446 + 16:446 + 32] = p2
    mbr[510] = 0x55
    mbr[511] = 0xAA
    return bytes(mbr)


def run_tool(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"FAIL: {' '.join(cmd)}\n{r.stdout}\n{r.stderr}")
    return r.stdout


def mkfs(p2_img):
    run_tool([MKE2FS, "-q", "-F", "-t", "ext4", "-b", str(BLOCK),
              "-I", str(INODE_SIZE), "-m", "0", "-L", "NITIAN",
              "-O", MKFS_FEATURES, str(p2_img)])


def emit_file(cmds, src, dest, mode, uid=0, gid=0):
    cmds.append(f"write {src} {dest}")
    cmds.append(f"sif {dest} mode 0{mode:o}")
    cmds.append(f"sif {dest} uid {uid}")
    cmds.append(f"sif {dest} gid {gid}")
    cmds.append(f"sif {dest} links_count 1")


def debugfs_cmds(bd, tmp, pre, names, smoke):
    cmds = ["cd /"]
    for name, mode in DIRS:
        cmds.append(f"mkdir {name}")
        cmds.append(f"sif /{name} mode 0{mode:o}")

    for name in names:
        mode = SPECIAL_MODES.get(name, FILE_MODE)
        emit_file(cmds, pre[name], f"/{name}", mode)

    for name, payload, mode in ETC_FILES:
        src = tmp / f"etc_{name}"
        src.write_bytes(payload)
        emit_file(cmds, src, f"/etc/{name}", mode)

    for name in LIB_FILES:
        src = bd / name
        if src.exists():
            emit_file(cmds, src, f"/lib/{name}", 0o100755)

    for name in BIN_LINKS:
        tgt = "/sh.elf" if name == "sh" else "/toybox"
        cmds.append(f"symlink /bin/{name} {tgt}")

    for name, tgt in SYMLINKS:
        cmds.append(f"symlink /{name} {tgt}")

    empty = tmp / "empty"
    empty.write_bytes(b"")
    for name, maj, mnr in DEV_NODES:
        path = f"/dev/{name}"
        cmds.append(f"write {empty} {path}")
        cmds.append(f"sif {path} flags 0")
        cmds.append(f"sif {path} mode 0{CHAR_DEV_MODE:o}")
        cmds.append(f"sif {path} block[0] {(maj << 8) | mnr}")
        cmds.append(f"sif {path} links_count 1")
    return cmds


def populate(bd, tmp, p2_img, smoke, autoexec):
    pre = {}
    for name in list(FILES) + [a for a in ALIASES if (bd / a).exists()]:
        src = bd / ALIASES.get(name, name)
        if src.exists():
            pre[name] = src
    if smoke:
        pre["autoexec"] = tmp / "autoexec"
        pre["autoexec"].write_bytes(
            SMOKE_AUTOEXEC if autoexec is None else autoexec)
        pre["shell.conf"] = tmp / "shell.conf"
        pre["shell.conf"].write_bytes(b"/init_sh.elf\n")
    else:
        pre["autoexec"] = tmp / "autoexec"
        pre["autoexec"].write_bytes(b"toybox login\n")

    cmds = debugfs_cmds(bd, tmp, pre, list(pre), smoke)
    script = tmp / "debugfs.cmds"
    script.write_text("\n".join(cmds) + "\n")
    run_tool([DEBUGFS, "-w", "-f", str(script), str(p2_img)])


def finalize(p2_img):
    r = subprocess.run([E2FSCK, "-fy", str(p2_img)],
                       capture_output=True, text=True)
    if r.returncode & 4:
        raise SystemExit(f"FAIL: e2fsck -fy left errors\n{r.stdout}{r.stderr}")
    r = subprocess.run([E2FSCK, "-fn", str(p2_img)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"FAIL: e2fsck -fn rc={r.returncode}\n{r.stdout}")


def feature_summary(p2_img):
    out = subprocess.run(["dumpe2fs", "-h", str(p2_img)],
                         capture_output=True, text=True).stdout
    lines = []
    for key in ("Filesystem features", "Block size", "Inode size",
                "Block count", "Free blocks", "Inode count", "Journal inode",
                "Filesystem state"):
        for line in out.splitlines():
            if line.startswith(key):
                lines.append(line.strip())
    return lines


def build(build_dir, out, smoke=False, autoexec=None):
    bd = Path(build_dir).resolve()
    boot_bin = (bd / "boot.bin").read_bytes()
    fat_img = bd / "fat32.img"
    make_fat.fat32(build_dir, str(fat_img))
    sys.stdout.flush()
    fat_data = fat_img.read_bytes()

    tmp = Path(tempfile.mkdtemp(prefix="mk_ext4_"))
    p2_img = tmp / "p2.img"
    with open(p2_img, "wb") as f:
        f.truncate(P2_SECTORS * SECTOR)

    mkfs(p2_img)
    populate(bd, tmp, p2_img, smoke, autoexec)
    finalize(p2_img)

    mbr = make_mbr(boot_bin)
    with open(out, "wb") as f:
        f.truncate(TOTAL_SECTORS * SECTOR)
        f.seek(0)
        f.write(mbr)
        f.seek(make_fat.PART_START * SECTOR)
        f.write(fat_data)
        f.seek(P2_START * SECTOR)
        f.write(p2_img.read_bytes())

    print(f"OK: {out} ({TOTAL_SECTORS * SECTOR // 1024 // 1024}MB)")
    print(f"  P1 @{make_fat.PART_START} FAT32({make_fat.P1_TOTAL_SECTORS}sec) "
          f"[bootable] -> {fat_img}")
    print(f"  P2 @{P2_START} ext4 ({(P2_SECTORS * SECTOR) // 1024 // 1024}MB), "
          f"block={BLOCK}, inode={INODE_SIZE}")
    for line in feature_summary(p2_img):
        print(f"  {line}")
    shutil.rmtree(tmp, ignore_errors=True)


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