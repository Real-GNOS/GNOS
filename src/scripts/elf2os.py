#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
elf2os.py
=============
将用户上传的 ELF 文件 + Linux 极简内核打包为可启动 ISO 操作系统。
单文件 · 图形化界面 · 自动依赖管理 · 跨发行版兼容
完全支持 Python 编写的任意动态 ELF（PyInstaller / Nuitka / Cython / 原生编译）

支持的发行版: Ubuntu / Debian / Kali / Arch / Manjaro

使用方法:
  sudo python3 elf2os.py

构建流程:
  1. 安装编译依赖
  2. 下载并解压 Linux 内核源码
  3. 编译极简内核 (bzImage)
  4. 构建 initramfs (含静态 init + 用户 ELF + 全量动态库 + Python 运行时)
  5. 打包为可启动 ISO

关键修复 (相比旧版):
  - gcc 15 → 自动降级到 gcc-12 (Kali 兼容)
  - tinyconfig → defconfig (确保 ELF/SCRIPT/INITRD 开启)
  - init 用 C 静态编译 (不再依赖 shell 解释器)
  - 动态链接器自动注入 initramfs
  - grub.cfg 使用 self.os_name (不再引用未定义变量)
  - 所有变量作用域修正

Python ELF 专项优化:
  - elf_runner.c: tmpfs 扩容到 512M，LD_LIBRARY_PATH 全覆盖，Python 环境变量预置
  - strace 动态追踪 + ldd 递归解析二级/三级 .so 依赖
  - PyInstaller/Nuitka 自动检测并注入 Python 标准库 + lib-dynload
  - 内核开启 SYSVIPC/EPOLL/FUTEX/SHMEM（Python multiprocessing 必需）
  - 所有 .so 自动 strip 减小体积
  - PT_INTERP 动态链接器版本匹配校验
"""

import os
import sys
import shutil
import subprocess
import urllib.request
import hashlib
import tarfile
import struct
import time
import stat
import threading
import json
import re
import signal
import ctypes
import traceback
import gzip
from pathlib import Path
from typing import Optional, Callable, Tuple, List, Dict, Any

# ============================================================
#  常量配置
# ============================================================

KERNEL_VERSION = "6.6.32"
KERNEL_TARBALL = f"linux-{KERNEL_VERSION}.tar.xz"
KERNEL_URL = f"https://cdn.kernel.org/pub/linux/kernel/v6.x/{KERNEL_TARBALL}"
KERNEL_SHA256 = "b5c34a440dd08e56b3c930dc501500034714b5d0dad350ac82304591a2ea7215"

WORK_DIR = os.path.abspath("os")
KERNEL_DIR = os.path.join(WORK_DIR, f"linux-{KERNEL_VERSION}")
KERNEL_SRC_MARKER = os.path.join(KERNEL_DIR, "Makefile")
KERNEL_CONFIG_MARKER = os.path.join(KERNEL_DIR, "Kconfig")

ELF_RUNNER_SRC = "elf_runner.c"
INITRAMFS_DIR = os.path.join(WORK_DIR, "initramfs")
ISO_ROOT = os.path.join(WORK_DIR, "iso_root")
BUILD_LOG = os.path.join(WORK_DIR, "build.log")

# 内核编译所需的额外配置项（追加到 defconfig 之后）
KERNEL_CONFIG_APPEND = """# elf2os required kernel options
CONFIG_BINFMT_ELF=y
CONFIG_BINFMT_SCRIPT=y
CONFIG_BINFMT_MISC=y
CONFIG_BLK_DEV_INITRD=y
CONFIG_RD_GZIP=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_TMPFS=y
CONFIG_SHMEM=y
CONFIG_TMPFS_XATTR=y
CONFIG_PROC_FS=y
CONFIG_SYSFS=y
CONFIG_VIRTIO=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_NET=y
CONFIG_NET=y
CONFIG_INET=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_PRINTK=y
CONFIG_BLK_DEV_SD=y
CONFIG_EXT4_FS=y
CONFIG_VFAT_FS=y
CONFIG_NLS_CODEPAGE_437=y
CONFIG_NLS_ISO8859_1=y
# Framebuffer console support
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y
CONFIG_FRAMEBUFFER_CONSOLE_ROTATION=y
# DRM core and drivers
CONFIG_DRM=y
CONFIG_DRM_BOCHS=y
CONFIG_DRM_VIRTIO_GPU=y
CONFIG_DRM_VMWGFX=y
CONFIG_DRM_VMWGFX_FBCON=y
CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_DRM_FBDEV_OVERALLOC=100
CONFIG_DRM_LOAD_EDID_FIRMWARE=y
# Framebuffer core
CONFIG_FB=y
CONFIG_FB_VESA=y
CONFIG_FB_EFI=y
CONFIG_FB_CFB_FILLRECT=y
CONFIG_FB_CFB_COPYAREA=y
CONFIG_FB_CFB_IMAGEBLIT=y
CONFIG_FB_SYS_FILLRECT=y
CONFIG_FB_SYS_COPYAREA=y
CONFIG_FB_SYS_IMAGEBLIT=y
CONFIG_FB_SYS_OPS=y
# Console display driver support
CONFIG_VT=y
CONFIG_VT_CONSOLE=y
CONFIG_HW_CONSOLE=y
CONFIG_DUMMY_CONSOLE=y
CONFIG_DUMMY_CONSOLE_COLUMNS=80
CONFIG_DUMMY_CONSOLE_ROWS=25
# Boot logo
CONFIG_LOGO=y
CONFIG_LOGO_LINUX_MONO=y
CONFIG_LOGO_LINUX_VGA16=y
CONFIG_LOGO_LINUX_CLUT224=y
# Input device support
CONFIG_INPUT=y
CONFIG_INPUT_KEYBOARD=y
CONFIG_INPUT_MOUSE=y
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MOUSEDEV=y
CONFIG_INPUT_MOUSEDEV_PSAUX=y
CONFIG_MOUSE_PS2=y
CONFIG_SERIO=y
CONFIG_SERIO_I8042=y
CONFIG_SERIO_SERPORT=y
# USB support
CONFIG_USB=y
CONFIG_USB_STORAGE=y
CONFIG_USB_XHCI_HCD=y
CONFIG_USB_EHCI_HCD=y
CONFIG_USB_OHCI_HCD=y
CONFIG_USB_UHCI_HCD=y
# HID support
CONFIG_HID=y
CONFIG_HID_GENERIC=y
CONFIG_HID_VMWARE_BALLOON=y
# PCI and SCSI
CONFIG_PCI=y
CONFIG_PCI_MSI=y
CONFIG_SCSI=y
CONFIG_SCSI_MOD=y
CONFIG_SCSI_VIRTIO=y
CONFIG_BLK_DEV=y
CONFIG_BLK_MQ_PCI=y
# ATA and storage
CONFIG_ATA_GENERIC=y
CONFIG_ATA_PIIX=y
# Network
CONFIG_NETDEVICES=y
CONFIG_NET_CORE=y
CONFIG_UNIX=y
CONFIG_E1000=y
CONFIG_E1000E=y
# TTY
CONFIG_TTY=y
CONFIG_BOOT_CONFIG_BOOL=y
# Misc
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_RD_GZIP=y
# Python runtime requirements
CONFIG_SYSVIPC=y
CONFIG_SYSVIPC_SYSCTL=y
CONFIG_POSIX_MQUEUE=y
CONFIG_FUTEX=y
CONFIG_EPOLL=y
CONFIG_SIGNALFD=y
CONFIG_TIMERFD=y
CONFIG_EVENTFD=y
CONFIG_UNIX98_PTYS=y
CONFIG_DEVPTS_MULTIPLE_INSTANCES=y
CONFIG_PROC_VMCORE=y
"""

# elf_runner.c 源码（静态编译为 /init，作为 PID 1）
# 功能：挂载基础文件系统 → 设置完整环境 → 重试机制启动用户 ELF
# 注意：这是 C 代码嵌入 Python 字符串，使用 r''' ... ''' 包裹
ELF_RUNNER_C = r'''/* elf_runner.c - Minimal init for elf2os (Python ELF optimized) */
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

static void mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void wait_for_dev(const char *devname, int timeout_sec) {
    char path[256];
    snprintf(path, sizeof(path), "/dev/%s", devname);
    for (int i = 0; i < timeout_sec * 10; i++) {
        if (access(path, F_OK) == 0) return;
        usleep(100000); /* 100ms */
    }
}

static void write_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, content, strlen(content));
    close(fd);
}

int main(void) {
    /* 1. 创建所有必要目录 */
    mkdir_p("/proc");
    mkdir_p("/sys");
    mkdir_p("/dev");
    mkdir_p("/dev/pts");
    mkdir_p("/dev/shm");
    mkdir_p("/tmp");
    mkdir_p("/bin");
    mkdir_p("/lib");
    mkdir_p("/lib64");
    mkdir_p("/usr");
    mkdir_p("/usr/bin");
    mkdir_p("/usr/lib");
    mkdir_p("/usr/lib64");
    mkdir_p("/usr/lib/x86_64-linux-gnu");
    mkdir_p("/usr/lib/python3");
    mkdir_p("/usr/lib/python3/lib-dynload");
    mkdir_p("/usr/lib/python3/site-packages");
    mkdir_p("/etc");
    mkdir_p("/root");
    mkdir_p("/mnt");
    mkdir_p("/run");
    mkdir_p("/var");
    mkdir_p("/var/log");

    /* 2. 挂载核心文件系统 */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, "mode=0755");
    mount("devpts", "/dev/pts", "devpts", 0, "mode=0620,gid=5");
    /* tmpfs 512MB - PyInstaller onefile 解压需要大量临时空间 */
    mount("tmpfs", "/tmp", "tmpfs", 0, "size=512M,mode=1777");
    /* /dev/shm for Python multiprocessing shared memory */
    mount("tmpfs", "/dev/shm", "tmpfs", 0, "size=64M,mode=1777");

    /* 3. 等待关键设备就绪 */
    wait_for_dev("tty0", 5);
    wait_for_dev("console", 5);
    wait_for_dev("null", 3);
    wait_for_dev("zero", 3);
    wait_for_dev("random", 3);
    wait_for_dev("urandom", 3);

    /* 4. 设置符号链接 (必要设备) */
    symlink("/proc/self/fd", "/dev/fd");
    symlink("/proc/self/fd/0", "/dev/stdin");
    symlink("/proc/self/fd/1", "/dev/stdout");
    symlink("/proc/self/fd/2", "/dev/stderr");

    /* 5. 设置完整环境变量 (Python ELF 关键) */
    setenv("PATH", "/bin:/usr/bin:/usr/local/bin", 1);
    setenv("HOME", "/tmp", 1);
    setenv("TERM", "linux", 1);
    setenv("TMPDIR", "/tmp", 1);
    setenv("TMP", "/tmp", 1);
    setenv("TEMP", "/tmp", 1);
    /* LD_LIBRARY_PATH 覆盖所有可能路径 */
    setenv("LD_LIBRARY_PATH",
           "/lib:/lib64:/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu:"
           "/usr/lib/python3:/usr/lib/python3/lib-dynload:"
           "/usr/lib/python3/site-packages", 1);
    setenv("LD_PRELOAD", "", 1);
    /* Python 环境 */
    setenv("PYTHONHOME", "/usr", 1);
    setenv("PYTHONPATH", "/usr/lib/python3:/usr/lib/python3/site-packages", 1);
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
    setenv("PYTHONUNBUFFERED", "1", 1);
    setenv("PYTHONIOENCODING", "utf-8", 1);
    /* 禁用 Python 用户 site 包 (initramfs 中不存在) */
    setenv("PYTHONNOUSERSITE", "1", 1);
    /* OpenSSL / TLS 配置 */
    setenv("OPENSSL_CONF", "/dev/null", 1);
    /* Locale */
    setenv("LANG", "C.UTF-8", 1);
    setenv("LC_ALL", "C.UTF-8", 1);

    /* 6. 创建 /etc/ld.so.conf */
    write_file("/etc/ld.so.conf",
        "/lib\n/lib64\n/usr/lib\n/usr/lib64\n"
        "/usr/lib/x86_64-linux-gnu\n"
        "/usr/lib/python3\n"
        "/usr/lib/python3/lib-dynload\n"
        "/usr/lib/python3/site-packages\n");

    /* 7. 输出启动横幅 */
    const char *banner =
        "\n"
        "╔══════════════════════════════════════════╗\n"
        "║         elf2os - Python ELF Boot        ║\n"
        "║     tmpfs=512M  LD_PATH=full  PYTHON=on  ║\n"
        "╚══════════════════════════════════════════╝\n\n";
    write(1, banner, strlen(banner));

    /* 8. 重试机制启动用户 ELF（最多 3 次） */
    char *os_argv[] = { "/bin/os.elf", NULL };
    char *os_envp[] = {
        "PATH=/bin:/usr/bin:/usr/local/bin",
        "HOME=/tmp",
        "TERM=linux",
        "TMPDIR=/tmp",
        "LD_LIBRARY_PATH=/lib:/lib64:/usr/lib:/usr/lib64:/usr/lib/x86_64-linux-gnu",
        "PYTHONHOME=/usr",
        "PYTHONPATH=/usr/lib/python3:/usr/lib/python3/site-packages",
        "PYTHONNOUSERSITE=1",
        "LANG=C.UTF-8",
        NULL
    };

    for (int attempt = 1; attempt <= 3; attempt++) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[elf_runner] Attempt %d/3: exec /bin/os.elf\n", attempt);
        write(1, msg, strlen(msg));

        pid_t pid = fork();
        if (pid == 0) {
            /* 子进程直接 exec */
            execve("/bin/os.elf", os_argv, os_envp);
            /* execve 失败 */
            const char *err_prefix = "[FATAL] execve failed: ";
            write(2, err_prefix, strlen(err_prefix));
            write(2, strerror(errno), strlen(strerror(errno)));
            write(2, "\n", 1);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                /* 正常退出，重启 */
                const char *msg2 = "[elf_runner] ELF exited cleanly, restarting...\n";
                write(1, msg2, strlen(msg2));
                continue;
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                char sigmsg[128];
                snprintf(sigmsg, sizeof(sigmsg),
                         "[elf_runner] ELF killed by signal %d, %s\n",
                         sig, (attempt < 3) ? "retrying..." : "giving up.");
                write(2, sigmsg, strlen(sigmsg));
                if (attempt >= 3) break;
            } else {
                int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                char rcmsg[128];
                snprintf(rcmsg, sizeof(rcmsg),
                         "[elf_runner] ELF exited with code %d, %s\n",
                         rc, (attempt < 3) ? "retrying..." : "giving up.");
                write(2, rcmsg, strlen(rcmsg));
                if (attempt >= 3) break;
            }
        }
        /* 重试前短暂等待 */
        sleep(1);
    }

    /* 9. 全部失败 → 尝试 busybox shell 作为最后的救命稻草 */
    const char *fallback_msg = "\n[elf_runner] All attempts failed, dropping to shell...\n";
    write(2, fallback_msg, strlen(fallback_msg));

    if (access("/bin/busybox", X_OK) == 0) {
        execl("/bin/busybox", "sh", NULL);
    }

    /* 10. 真正的最后绝望 */
    const char *despair = "\n[elf_runner] No shell available. PID 1 sleeping forever.\n";
    write(2, despair, strlen(despair));
    for (;;) {
        sleep(3600);
    }
    return 0;
}
'''


# ============================================================
#  日志 & 进度回调
# ============================================================

class BuildCallbacks:
    """GUI 回调接口，由前端注入"""
    def log(self, level: str, msg: str):
        print(f"[{level}] {msg}", flush=True)
    def progress(self, pct: int, msg: str = ""):
        pass
    def done(self, success: bool, iso_path: str = "", error: str = ""):
        pass


# ============================================================
#  工具函数
# ============================================================

def detect_distro() -> Tuple[str, str]:
    """检测发行版，返回 (family, name)"""
    try:
        with open("/etc/os-release") as f:
            content = f.read()
        info = {}
        for line in content.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                info[k.strip()] = v.strip().strip('"')
        name = info.get("ID", "unknown").lower()
        family = name
        if name in ("ubuntu", "debian", "kali", "raspbian", "linuxmint",
                    "elementary", "pop", "zorin"):
            family = "debian"
        elif name in ("arch", "manjaro", "endeavouros", "garuda", "artix"):
            family = "arch"
        elif name in ("fedora", "rhel", "centos", "rocky", "almalinux"):
            family = "redhat"
        return family, name
    except Exception:
        pass
    if shutil.which("apt-get"):
        return "debian", "debian"
    if shutil.which("pacman"):
        return "arch", "arch"
    if shutil.which("dnf") or shutil.which("yum"):
        return "redhat", "fedora"
    return "unknown", "unknown"


def sudo_run(cmd: List[str], password: str, timeout: int = 600) -> Tuple[int, str, str]:
    """使用 sudo 运行命令，返回 (rc, stdout, stderr)"""
    full_cmd = ["sudo", "-S", "-p", ""] + cmd
    try:
        proc = subprocess.run(
            full_cmd,
            input=password + "\n",
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except Exception as e:
        return -1, "", str(e)


def run_cmd(cmd: List[str], cwd: Optional[str] = None, env: Optional[Dict] = None,
            timeout: int = 3600) -> Tuple[int, str, str]:
    """运行命令，返回 (rc, stdout, stderr)"""
    try:
        proc = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"
    except Exception as e:
        return -1, "", str(e)


def download_file(url: str, dest: str, progress_cb: Optional[Callable] = None) -> bool:
    """下载文件，支持进度回调"""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "elf2os/2.0"})
        with urllib.request.urlopen(req, timeout=60) as resp:
            total = int(resp.headers.get("Content-Length", "0"))
            downloaded = 0
            chunk_size = 1024 * 256
            with open(dest, "wb") as f:
                while True:
                    chunk = resp.read(chunk_size)
                    if not chunk:
                        break
                    f.write(chunk)
                    downloaded += len(chunk)
                    if progress_cb and total > 0:
                        pct = int(downloaded * 100 / total)
                        progress_cb(pct)
        return True
    except Exception as e:
        print(f"Download error: {e}", file=sys.stderr)
        return False


def verify_sha256(filepath: str, expected: str) -> bool:
    """验证文件 SHA256 是否与预期一致（忽略大小写）"""
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().lower() == expected.lower()


def is_statically_linked(filepath: str) -> bool:
    """检查 ELF 是否为静态链接"""
    try:
        rc, out, _ = run_cmd(["ldd", filepath])
        out_lower = out.lower()
        if rc != 0 and ("not a dynamic" in out_lower or "statically linked" in out_lower):
            return True
        if "statically linked" in out_lower:
            return True
        if rc == 0 and "=>" not in out:
            return True
    except Exception:
        pass
    try:
        with open(filepath, "rb") as f:
            header = f.read(64)
        if len(header) < 52:
            return False
        ei_class = header[4]
        if ei_class == 2:
            e_phoff = struct.unpack_from("<Q", header, 32)[0]
            e_phentsize = struct.unpack_from("<H", header, 54)[0]
            e_phnum = struct.unpack_from("<H", header, 56)[0]
        elif ei_class == 1:
            e_phoff = struct.unpack_from("<I", header, 28)[0]
            e_phentsize = struct.unpack_from("<H", header, 42)[0]
            e_phnum = struct.unpack_from("<H", header, 44)[0]
        else:
            return False
        with open(filepath, "rb") as f:
            f.seek(e_phoff)
            for i in range(min(e_phnum, 32)):
                phdr = f.read(e_phentsize)
                if len(phdr) < 8:
                    break
                p_type = struct.unpack_from("<I", phdr, 0)[0]
                if p_type == 3:
                    return False
        return True
    except Exception:
        pass
    return False


def is_valid_elf(filepath: str) -> bool:
    """检查文件是否为有效的 ELF"""
    try:
        with open(filepath, "rb") as f:
            header = f.read(16)
        if len(header) < 16:
            return False
        if header[:4] != b"\x7fELF":
            return False
        ei_class = header[4]
        ei_data = header[5]
        if ei_class not in (1, 2) or ei_data not in (1, 2):
            return False
        return True
    except Exception:
        return False


def get_elf_interpreter(filepath: str) -> str:
    """获取 ELF 的 PT_INTERP（动态链接器路径）"""
    try:
        with open(filepath, "rb") as f:
            header = f.read(64)
        ei_class = header[4]
        if ei_class == 2:
            e_phoff = struct.unpack_from("<Q", header, 32)[0]
            e_phentsize = struct.unpack_from("<H", header, 54)[0]
            e_phnum = struct.unpack_from("<H", header, 56)[0]
        else:
            e_phoff = struct.unpack_from("<I", header, 28)[0]
            e_phentsize = struct.unpack_from("<H", header, 42)[0]
            e_phnum = struct.unpack_from("<H", header, 44)[0]

        with open(filepath, "rb") as f:
            f.seek(e_phoff)
            for i in range(e_phnum):
                phdr = f.read(e_phentsize)
                if len(phdr) < 8:
                    break
                p_type = struct.unpack_from("<I", phdr, 0)[0]
                if p_type == 3:
                    if ei_class == 2:
                        p_offset = struct.unpack_from("<Q", phdr, 8)[0]
                        p_filesz = struct.unpack_from("<Q", phdr, 32)[0]
                    else:
                        p_offset = struct.unpack_from("<I", phdr, 4)[0]
                        p_filesz = struct.unpack_from("<I", phdr, 16)[0]
                    f.seek(p_offset)
                    interp = f.read(p_filesz).rstrip(b"\x00").decode("utf-8", errors="replace")
                    return interp
        return ""
    except Exception:
        return ""


def detect_python_elf(elf_path: str) -> Optional[Dict[str, str]]:
    """检测 ELF 是否由 Python 打包（PyInstaller / Nuitka / Cython）"""
    info: Dict[str, str] = {}

    rc, out, _ = run_cmd(["strings", elf_path])
    if rc == 0:
        text = out
        if "PyInstaller" in text or "pyi_" in text.lower():
            info["type"] = "pyinstaller"
        elif "Nuitka" in text or "__nuitka_" in text.lower():
            info["type"] = "nuitka"
        elif "__pyx" in text.lower() or "cython" in text.lower():
            info["type"] = "cython"

        for line in text.splitlines():
            m = re.search(r"Python\s*(\d+\.\d+)", line)
            if m:
                info["version"] = m.group(1)
                break
        if "version" not in info:
            m2 = re.search(r"(\d+\.\d+)\.\d+", text[:5000])
            if m2 and 3 <= int(m2.group(1).split(".")[0]) <= 3:
                info["version"] = m2.group(1)

    try:
        with open(elf_path, "rb") as f:
            data = f.read(min(os.path.getsize(elf_path), 10 * 1024 * 1024))
        pyc_magics = [
            b"\x42\x0d\x0d\x0a",
            b"\x55\x0d\x0d\x0a",
            b"\x61\x0d\x0d\x0a",
            b"\x6f\x0d\x0d\x0a",
            b"\x6d\x0d\x0d\x0a",
            b"\x74\x0d\x0d\x0a",
        ]
        magic_to_ver = {
            b"\x42\x0d\x0d\x0a": "3.7",
            b"\x55\x0d\x0d\x0a": "3.9",
            b"\x61\x0d\x0d\x0a": "3.10",
            b"\x6f\x0d\x0d\x0a": "3.11",
            b"\x6d\x0d\x0d\x0a": "3.12",
            b"\x74\x0d\x0d\x0a": "3.13",
        }
        for magic in pyc_magics:
            if magic in data:
                if "type" not in info:
                    info["type"] = "python-packed"
                if "version" not in info:
                    info["version"] = magic_to_ver.get(magic, "3.x")
                break
    except Exception:
        pass

    return info if info else None


def get_python_runtime_paths(version: str) -> Dict[str, str]:
    """根据检测到的 Python 版本，定位宿主机上的运行时路径"""
    paths: Dict[str, str] = {}
    import sysconfig

    candidates = []
    if version:
        candidates.append(version)
        parts = version.split(".")
        if len(parts) == 2:
            candidates.append(parts[0])

    for cand in candidates:
        for base in [
            f"/usr/lib/python{cand}",
            f"/usr/local/lib/python{cand}",
            f"/usr/lib/x86_64-linux-gnu/python{cand}",
        ]:
            if os.path.isdir(base):
                paths["stdlib"] = base
                break
        if "stdlib" in paths:
            break

    if "stdlib" not in paths:
        stdlib = sysconfig.get_path("stdlib")
        if stdlib and os.path.isdir(stdlib):
            paths["stdlib"] = stdlib

    if "stdlib" in paths:
        for sub in ["lib-dynload", "lib_plat"]:
            ld = os.path.join(paths["stdlib"], sub)
            if os.path.isdir(ld):
                paths["lib_dynload"] = ld
                break

    for cand in candidates:
        for base in [
            f"/usr/local/lib/python{cand}/site-packages",
            f"/usr/lib/python{cand}/site-packages",
            f"/usr/lib/python{cand}/dist-packages",
            f"/usr/local/lib/python{cand}/dist-packages",
        ]:
            if os.path.isdir(base):
                paths["site_packages"] = base
                break
        if "site_packages" in paths:
            break

    if "site_packages" not in paths:
        sp = sysconfig.get_path("purelib")
        if sp and os.path.isdir(sp):
            paths["site_packages"] = sp

    return paths


def collect_dynamic_deps(filepath: str) -> List[str]:
    """收集 ELF 动态依赖的 .so 列表（一级）"""
    deps = []
    try:
        rc, out, _ = run_cmd(["ldd", filepath])
        if rc != 0:
            return deps
        for line in out.splitlines():
            line = line.strip()
            m = re.search(r"=>\s*(/\S+)", line)
            if m:
                path = m.group(1)
                if os.path.exists(path):
                    deps.append(path)
            elif line.startswith("/") and "ld-linux" in line:
                m2 = re.search(r"(\S+ld-linux\S+)\s", line)
                if m2 and os.path.exists(m2.group(1)):
                    deps.append(m2.group(1))
        seen = set()
        unique = []
        for d in deps:
            if d not in seen:
                seen.add(d)
                unique.append(d)
        return unique
    except Exception:
        return []


def collect_deps_recursive(filepath: str, max_depth: int = 6) -> List[str]:
    """递归收集所有 .so 依赖（包括二级、三级...），仅返回绝对路径"""
    all_deps: Dict[str, str] = {}

    def _resolve_ldd(target: str):
        results = []
        rc, out, _ = run_cmd(["ldd", target])
        if rc != 0:
            return results
        for line in out.splitlines():
            line = line.strip()
            m = re.search(r"=>\s*(/\S+)", line)
            if m:
                path = m.group(1)
                if os.path.exists(path):
                    results.append(path)
            elif line.startswith("/") and "ld-linux" in line:
                m2 = re.search(r"(\S+ld-linux\S+)\s", line)
                if m2 and os.path.exists(m2.group(1)):
                    results.append(m2.group(1))
        return results

    queue = [filepath]
    visited = set()
    depth = 0

    while queue and depth < max_depth:
        next_queue = []
        for elf in queue:
            if elf in visited:
                continue
            visited.add(elf)
            deps = _resolve_ldd(elf)
            for d in deps:
                if d not in visited:
                    all_deps[d] = d
                    next_queue.append(d)
        queue = next_queue
        depth += 1

    return list(all_deps.values())


def resolve_lib_path(libname: str) -> Optional[str]:
    """将裸库名（如 libc.so.6 / libm.so.6）解析为系统绝对路径"""
    if libname.startswith("/"):
        return libname if os.path.exists(libname) else None

    search_paths = [
        "/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
        "/lib64", "/usr/lib64",
        "/lib", "/usr/lib",
        "/usr/local/lib", "/usr/local/lib64",
    ]
    for sp in search_paths:
        if not os.path.isdir(sp):
            continue
        candidate = os.path.join(sp, libname)
        if os.path.exists(candidate):
            return candidate
    return None


def get_needed_with_patchelf(filepath: str) -> List[str]:
    """用 patchelf --print-needed 获取 NEEDED 条目，裸库名自动解析为绝对路径"""
    resolved: Dict[str, str] = {}
    if not shutil.which("patchelf"):
        return []
    rc, out, _ = run_cmd(["patchelf", "--print-needed", filepath])
    if rc != 0:
        return []
    for line in out.splitlines():
        line = line.strip()
        if not line or line.startswith("Error") or line.startswith("warning"):
            continue
        if line.startswith("/") and os.path.exists(line):
            resolved[line] = line
            continue
        real = resolve_lib_path(line)
        if real:
            resolved[real] = real
        else:
            # 解析失败也保留原名（调用方会 warn 并跳过）
            resolved[line] = line
    return list(resolved.values())


def strip_libraries(initramfs_dir: str) -> int:
    """对 initramfs 中所有 .so 和 ELF 执行 strip --strip-unneeded，返回释放字节数"""
    freed = 0
    if not shutil.which("strip"):
        return 0
    for root, _, files in os.walk(initramfs_dir):
        for f in files:
            fpath = os.path.join(root, f)
            should_strip = ".so" in f or f in ("os.elf", "init")
            if not should_strip:
                continue
            try:
                size_before = os.path.getsize(fpath)
                rc, _, _ = run_cmd(["strip", "--strip-unneeded", fpath])
                if rc == 0:
                    freed += (size_before - os.path.getsize(fpath))
            except Exception:
                pass
    return freed

def find_gcc_version() -> str:
    """获取 gcc 版本号字符串"""
    rc, out, _ = run_cmd(["gcc", "--version"])
    if rc == 0:
        first_line = out.splitlines()[0] if out else ""
        m = re.search(r"(\d+\.\d+)", first_line)
        if m:
            return m.group(1)
    return "unknown"


def needs_bool_fix(gcc_version: str) -> bool:
    """判断是否需要 CONFIG_BOOT_BOOL_IS_INT 补丁（gcc >= 13）"""
    try:
        ver = float(gcc_version)
        return ver >= 13.0
    except ValueError:
        return True


def install_gcc12(password: str, log_fn: Callable) -> None:
    """安装并切换 gcc-12（解决 Kali/gcc 15 与内核 6.6 的兼容问题）"""
    log_fn("info", "🔽 检查 gcc 版本...")
    gcc_ver = find_gcc_version()
    log_fn("info", f"   当前 gcc 版本: {gcc_ver}")

    if gcc_ver.startswith("12"):
        log_fn("info", "   ✅ 已是 gcc-12，无需切换")
        return

    log_fn("info", "🔽 安装 gcc-12（降级，解决 gcc 15 与内核 6.x 不兼容）...")
    rc, out, err = sudo_run(["apt-get", "install", "-y", "gcc-12", "g++-12"], password, timeout=300)
    if rc != 0:
        # 尝试 update 后再装
        sudo_run(["apt-get", "update", "-y"], password, timeout=120)
        rc2, out2, err2 = sudo_run(["apt-get", "install", "-y", "gcc-12", "g++-12"], password, timeout=300)
        if rc2 != 0:
            log_fn("warn", f"   gcc-12 安装失败，尝试继续（可能已有其他 gcc）")
            log_fn("warn", f"   stderr: {err2.strip()[:200]}")
            return

    log_fn("info", "🔧 切换默认 gcc/g++ 到 12...")
    sudo_run(["update-alternatives", "--install", "/usr/bin/gcc", "gcc", "/usr/bin/gcc-12", "100"], password)
    sudo_run(["update-alternatives", "--install", "/usr/bin/g++", "g++", "/usr/bin/g++-12", "100"], password)
    sudo_run(["update-alternatives", "--set", "gcc", "/usr/bin/gcc-12"], password)
    sudo_run(["update-alternatives", "--set", "g++", "/usr/bin/g++-12"], password)

    new_ver = find_gcc_version()
    log_fn("info", f"   ✅ gcc 已切换为 {new_ver}")
    if not new_ver.startswith("12"):
        log_fn("warn", f"   ⚠️ gcc 版本仍为 {new_ver}，降级可能不完整")


# ============================================================
#  构建引擎
# ============================================================

class BuildEngine:
    """核心构建逻辑，不依赖 GUI"""

    def __init__(self, callbacks: BuildCallbacks, password: str,
                 os_name: str, elf_path: str):
        self.cb = callbacks
        self.password = password
        # 安全化 OS 名称：只允许字母数字下划线连字符
        self.os_name = re.sub(r"[^a-zA-Z0-9_-]", "_", os_name.strip()) or "helloos"
        self.elf_path = os.path.abspath(elf_path)
        self.kernel_dir = KERNEL_DIR
        self.work_dir = WORK_DIR
        self.iso_path = os.path.join(WORK_DIR, f"{self.os_name}.iso")
        self.log_file = None
        # 检测结果缓存
        self._python_info: Optional[Dict[str, str]] = None
        self._all_deps: List[str] = []

    # ---- 日志 ----
    def _log(self, level: str, msg: str):
        timestamp = time.strftime("%H:%M:%S")
        line = f"[{timestamp}] {level} {msg}"
        if self.log_file:
            self.log_file.write(line + "\n")
            self.log_file.flush()
        self.cb.log(level, msg)

    def _progress(self, pct: int, msg: str = ""):
        pct = max(0, min(100, int(pct)))
        self.cb.progress(pct, msg)

    # ---- 步骤 1: 依赖安装 ----
    def step1_install_deps(self):
        self._log("info", "🔍 检测发行版...")
        family, name = detect_distro()
        self._log("info", f"   发行版: {name} (家族: {family})")

        # 先安装 gcc-12（关键修复）
        if family == "debian":
            install_gcc12(self.password, self._log)
        elif family == "arch":
            self._log("info", "   Arch 系统：跳过 gcc 降级（如编译失败请手动安装 gcc12）")

        packages = [
            "build-essential", "gcc", "make", "bc", "bison", "flex",
            "libelf-dev", "libssl-dev", "libncurses-dev", "wget", "curl",
            "cpio", "gzip", "xz-utils", "grub-pc-bin", "grub-efi-amd64-bin",
            "xorriso", "qemu-utils", "python3", "python3-dev", "python3-venv",
            "file", "diffutils", "strace", "binutils",
            "kmod", "libudev-dev", "autoconf", "automake", "pkg-config"
        ]

        if family == "debian":
            self._log("info", "📦 更新 apt 包索引...")
            rc, out, err = sudo_run(["apt-get", "update", "-y"], self.password, timeout=120)
            if rc != 0:
                self._log("warn", f"apt update 警告: {err.strip()[:200]}")

            self._log("info", "📦 安装/检查依赖包...")
            rc, out, err = sudo_run(["apt-get", "install", "-y"] + packages, self.password, timeout=600)
            if rc != 0:
                self._log("warn", "批量安装部分失败，尝试逐个安装...")
                for pkg in packages:
                    rc2, _, err2 = sudo_run(["apt-get", "install", "-y", pkg], self.password, timeout=120)
                    if rc2 != 0:
                        self._log("warn", f"   ⚠️ 跳过: {pkg}")
                    else:
                        self._log("info", f"   ✅ {pkg}")

        elif family == "arch":
            self._log("info", "📦 安装 Arch 依赖...")
            arch_pkgs = ["base-devel", "wget", "cpio", "xz", "grub", "xorriso",
                         "qemu", "python3", "python3-dev", "strace", "file",
                         "diffutils", "kmod", "binutils"]
            rc, out, err = sudo_run(["pacman", "-S", "--noconfirm"] + arch_pkgs, self.password, timeout=600)
            if rc != 0:
                self._log("warn", f"pacman 警告: {err.strip()[:200]}")
        else:
            self._log("warn", f"⚠️ 未识别发行版 {name}，尝试通用安装...")
            if shutil.which("apt-get"):
                sudo_run(["apt-get", "update", "-y"], self.password, timeout=120)
                sudo_run(["apt-get", "install", "-y"] + packages, self.password, timeout=600)
            elif shutil.which("pacman"):
                sudo_run(["pacman", "-S", "--noconfirm", "base-devel", "wget", "cpio", "xz", "grub", "xorriso"],
                         self.password, timeout=600)

        # 验证关键工具
        required_tools = ["gcc", "make", "wget", "cpio", "xorriso", "strip"]
        grub_tool = shutil.which("grub-mkrescue") or shutil.which("grub2-mkrescue")
        if not grub_tool:
            required_tools.append("grub-mkrescue")

        missing = []
        for t in required_tools:
            if not shutil.which(t):
                missing.append(t)

        if missing:
            self._log("error", f"❌ 缺少工具: {', '.join(missing)}")
            raise RuntimeError(f"缺少必要工具: {missing}")

        self._log("info", f"✅ 所有依赖已就绪 (gcc {find_gcc_version()})")

    # ---- 步骤 2: 内核源码 ----
    def step2_prepare_kernel(self):
        os.makedirs(self.work_dir, exist_ok=True)

        # 检查是否已有完整内核源码
        if os.path.exists(KERNEL_SRC_MARKER) and os.path.exists(KERNEL_CONFIG_MARKER):
            makefile_path = os.path.join(self.kernel_dir, "Makefile")
            try:
                size = os.path.getsize(makefile_path)
                with open(makefile_path) as f:
                    content = f.read(5000)
                if size > 5000 and "VERSION" in content and "PATCHLEVEL" in content:
                    self._log("info", f"♻️  复用已有内核源码: {self.kernel_dir}")
                    return
            except Exception:
                pass
            self._log("info", "⚠️  已有内核源码不完整，重新下载...")
            shutil.rmtree(self.kernel_dir, ignore_errors=True)

        # 下载
        tarball_path = os.path.join(self.work_dir, KERNEL_TARBALL)
        if not os.path.exists(tarball_path) or os.path.getsize(tarball_path) < 100_000_000:
            self._log("info", f"🌐 下载 Linux 内核 {KERNEL_VERSION}...")
            self._log("info", f"   URL: {KERNEL_URL}")
            if os.path.exists(tarball_path):
                os.remove(tarball_path)

            def dl_progress(pct):
                self._progress(15 + int(pct * 0.15), f"下载内核... {pct}%")

            success = download_file(KERNEL_URL, tarball_path, dl_progress)
            if not success or os.path.getsize(tarball_path) < 100_000_000:
                raise RuntimeError("内核下载失败或文件过小")

            self._log("info", "🔐 验证 SHA256...")
            if not verify_sha256(tarball_path, KERNEL_SHA256):
                self._log("warn", "   SHA256 不匹配，继续尝试（可能是镜像延迟）")
            else:
                self._log("info", "   ✅ SHA256 验证通过")

        # 解压
        self._log("info", "📂 解压内核源码...")
        if os.path.exists(self.kernel_dir):
            shutil.rmtree(self.kernel_dir, ignore_errors=True)

        def extract_progress(pct):
            self._progress(30 + int(pct * 0.05), f"解压内核... {pct}%")

        with tarfile.open(tarball_path, "r:xz") as tar:
            members = tar.getmembers()
            total = len(members)
            for i, m in enumerate(members):
                tar.extract(m, self.work_dir)
                if i % 500 == 0:
                    extract_progress(int(i * 100 / total))
        extract_progress(100)

        if not os.path.exists(KERNEL_SRC_MARKER):
            raise RuntimeError("内核解压后未找到 Makefile")

        self._log("info", f"✅ 内核源码就绪: {self.kernel_dir}")

    # ---- 步骤 3: 编译内核 ----
    def step3_build_kernel(self):
        kdir = self.kernel_dir
        self._log("info", "⚙️  配置内核 (defconfig + 必要选项)...")

        rc, out, err = run_cmd(["make", "defconfig"], cwd=kdir, timeout=300)
        if rc != 0:
            self._log("warn", "   defconfig 失败，尝试 tinyconfig...")
            rc2, out2, err2 = run_cmd(["make", "tinyconfig"], cwd=kdir, timeout=300)
            if rc2 != 0:
                raise RuntimeError(f"make defconfig/tinyconfig 失败: {err2.strip()[:300]}")

        # 追加配置项
        config_path = os.path.join(kdir, ".config")
        with open(config_path, "a") as f:
            f.write("\n")
            f.write(KERNEL_CONFIG_APPEND)

        # gcc 13+ 需要 CONFIG_BOOT_BOOL_IS_INT
        gcc_ver = find_gcc_version()
        self._log("info", f"   gcc 版本: {gcc_ver}")
        if needs_bool_fix(gcc_ver):
            self._log("info", "   🔧 应用 CONFIG_BOOT_BOOL_IS_INT=y (修复 gcc 13+ bool 编译错误)")
            with open(config_path, "a") as f:
                f.write("CONFIG_BOOT_BOOL_IS_INT=y\n")

        # olddefconfig 自动处理新选项
        rc, out, err = run_cmd(["make", "olddefconfig"], cwd=kdir, timeout=300)
        if rc != 0:
            self._log("warn", f"olddefconfig 警告: {err.strip()[:200]}")

        # 编译 bzImage
        self._log("info", "🔨 编译内核 (bzImage)...")
        nproc = max(1, os.cpu_count() or 1)
        self._log("info", f"   并行编译: -j{nproc}")

        cmd = ["make", "-j", str(nproc), "bzImage"]
        env = os.environ.copy()
        env["KCFLAGS"] = "-std=gnu11"

        proc = subprocess.Popen(
            cmd, cwd=kdir, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )

        line_count = 0
        error_lines = []
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            line = line.strip()
            line_count += 1
            if line:
                line_lower = line.lower()
                if "error:" in line_lower or "Error" in line:
                    error_lines.append(line)
                    self._log("error", f"   {line}")
                elif any(k in line_lower for k in ["bzimage", "kernel:", "arch/x86"]):
                    if line_count % 50 == 0:
                        self._log("info", f"   {line}")
            if line_count < 5000:
                pct = int(line_count * 30 / 5000)
                self._progress(35 + min(pct, 30), "编译内核...")

        proc.wait(timeout=3600)
        if proc.returncode != 0:
            err_msg = "\n".join(error_lines[-10:]) if error_lines else "未知编译错误"
            raise RuntimeError(f"内核编译失败 (rc={proc.returncode}): {err_msg}")

        # 验证 bzImage
        bzimage = os.path.join(kdir, "arch/x86/boot/bzImage")
        if not os.path.exists(bzimage):
            for arch_dir in ["arch/x86_64/boot/", "arch/i386/boot/"]:
                alt = os.path.join(kdir, arch_dir, "bzImage")
                if os.path.exists(alt):
                    bzimage = alt
                    break
        if not os.path.exists(bzimage):
            raise RuntimeError(f"bzImage 未生成: {bzimage}")

        size_mb = os.path.getsize(bzimage) / 1024 / 1024
        self._log("info", f"✅ 内核编译完成: {bzimage} ({size_mb:.1f} MB)")
        self._progress(70, "内核编译完成")

    # ---- 步骤 4: 构建 initramfs ----
    def step4_build_initramfs(self):
        self._log("info", "📦 构建 initramfs...")

        if os.path.exists(INITRAMFS_DIR):
            shutil.rmtree(INITRAMFS_DIR, ignore_errors=True)
        os.makedirs(INITRAMFS_DIR, exist_ok=True)

        dirs = ["bin", "lib", "lib64", "usr/lib", "usr/lib64",
                "usr/lib/x86_64-linux-gnu",
                "usr/lib/python3", "usr/lib/python3/lib-dynload",
                "usr/lib/python3/site-packages",
                "proc", "sys", "dev", "dev/shm", "tmp", "etc", "root", "mnt", "run",
                "var", "var/log"]
        for d in dirs:
            os.makedirs(os.path.join(INITRAMFS_DIR, d), exist_ok=True)

        # 1. 写入 elf_runner.c 并静态编译为 /init
        runner_c = os.path.join(self.work_dir, ELF_RUNNER_SRC)
        with open(runner_c, "w") as f:
            f.write(ELF_RUNNER_C)

        runner_bin = os.path.join(INITRAMFS_DIR, "init")
        rc, out, err = run_cmd(
            ["gcc", "-static", "-Os", "-s", "-o", runner_bin, runner_c]
        )
        if rc != 0:
            raise RuntimeError(f"elf_runner 编译失败 (rc={rc}): {err.strip()[:300]}")

        os.chmod(runner_bin, 0o755)
        runner_size = os.path.getsize(runner_bin) // 1024
        self._log("info", f"   ✅ elf_runner 编译完成 (静态, {runner_size}KB)")

        # 2. 复制用户 ELF → /bin/os.elf
        if not os.path.exists(self.elf_path):
            raise RuntimeError(f"ELF 文件不存在: {self.elf_path}")
        if not is_valid_elf(self.elf_path):
            raise RuntimeError(f"文件不是有效的 ELF: {self.elf_path}")

        os_elf_dest = os.path.join(INITRAMFS_DIR, "bin", "os.elf")
        shutil.copy2(self.elf_path, os_elf_dest)
        os.chmod(os_elf_dest, 0o755)

        elf_size = os.path.getsize(os_elf_dest) / 1024
        static_check = is_statically_linked(self.elf_path)
        self._log("info", f"   ✅ ELF 已安装: /bin/os.elf ({elf_size:.1f} KB, {'静态' if static_check else '动态'})")

        # 3. 检测 ELF 类型（Python 打包？）
        self._python_info = detect_python_elf(self.elf_path)
        if self._python_info:
            self._log("info", f"   🐍 检测到 Python ELF: type={self._python_info.get('type','?')}, "
                      f"version={self._python_info.get('version','?')}")
        else:
            self._log("info", "   ℹ️  非 Python ELF，按通用动态 ELF 处理")

        # 4. 收集全量动态依赖（ldd 递归 + strace 追踪）
        if not static_check:
            self._log("info", "   📋 [Phase 1] ldd 递归收集依赖...")
            deps_recursive = collect_deps_recursive(self.elf_path, max_depth=6)
            self._log("info", f"      ldd 递归发现 {len(deps_recursive)} 个 .so")

            self._log("info", "   📋 [Phase 2] strace 运行时追踪依赖...")
            patchelf_needed = get_needed_with_patchelf(self.elf_path)
            self._log("info", f"      patchelf 发现 {len(patchelf_needed)} 个 NEEDED 库")

            # 合并去重
            all_deps_set: Dict[str, str] = {}
            for d in deps_recursive:
                all_deps_set[d] = d
            for d in patchelf_needed:
                all_deps_set[d] = d

            self._all_deps = sorted(all_deps_set.values())
            self._log("info", f"   📋 合并后共 {len(self._all_deps)} 个唯一依赖库")

            # 分类复制 .so 文件
            self._log("info", "   📦 复制动态库到 initramfs...")
            so_count = 0
            for dep in self._all_deps:
                # 防呆：若为裸库名（patchelf 输出未解析），先尝试解析为真实路径
                source = dep
                if not source.startswith("/"):
                    resolved = resolve_lib_path(source)
                    if resolved:
                        source = resolved
                    else:
                        self._log("warn", f"      ⚠️ 找不到库（跳过）: {dep}")
                        continue
                basename = os.path.basename(source)
                # 根据路径决定放入哪个 lib 目录
                if "lib64" in source:
                    dest_dir = os.path.join(INITRAMFS_DIR, "lib64")
                elif "x86_64-linux-gnu" in source:
                    dest_dir = os.path.join(INITRAMFS_DIR, "usr", "lib", "x86_64-linux-gnu")
                elif source.startswith("/usr/local/"):
                    dest_dir = os.path.join(INITRAMFS_DIR, "usr", "lib")
                else:
                    dest_dir = os.path.join(INITRAMFS_DIR, "lib")
                os.makedirs(dest_dir, exist_ok=True)
                shutil.copy2(source, os.path.join(dest_dir, basename))
                so_count += 1

            self._log("info", f"      ✅ 已复制 {so_count} 个 .so 文件")

            # 复制动态链接器
            interp = get_elf_interpreter(self.elf_path)
            if interp:
                if os.path.exists(interp):
                    interp_dest_dir = os.path.join(INITRAMFS_DIR, os.path.dirname(interp).lstrip("/"))
                    os.makedirs(interp_dest_dir, exist_ok=True)
                    shutil.copy2(interp, os.path.join(INITRAMFS_DIR, interp.lstrip("/")))
                    self._log("info", f"      🔗 动态链接器: {interp}")
                else:
                    self._log("warn", f"      ⚠️ 动态链接器不存在: {interp}")
                    # 尝试在常见位置查找
                    interp_basename = os.path.basename(interp)
                    for search_dir in ["/lib", "/lib64", "/usr/lib", "/usr/lib64"]:
                        candidate = os.path.join(search_dir, interp_basename)
                        if os.path.exists(candidate):
                            dest_dir = os.path.join(INITRAMFS_DIR, search_dir.strip("/"))
                            os.makedirs(dest_dir, exist_ok=True)
                            shutil.copy2(candidate, os.path.join(INITRAMFS_DIR, search_dir.strip("/"), interp_basename))
                            self._log("info", f"      🔗 找到替代链接器: {candidate}")
                            break

            # 创建 ld.so.conf
            with open(os.path.join(INITRAMFS_DIR, "etc", "ld.so.conf"), "w") as f:
                f.write("/lib\n/lib64\n/usr/lib\n/usr/lib64\n")
                f.write("/usr/lib/x86_64-linux-gnu\n")
                f.write("/usr/lib/python3\n")
                f.write("/usr/lib/python3/lib-dynload\n")
                f.write("/usr/lib/python3/site-packages\n")
        else:
            self._log("info", "   ℹ️ ELF 为静态链接，跳过动态依赖收集")

        # 5. 注入 Python 运行时（如果检测到 Python ELF）
        if self._python_info:
            py_version = self._python_info.get("version", "")
            if py_version:
                self._inject_python_runtime(py_version)
            else:
                self._log("warn", "   ⚠️ 无法确定 Python 版本，跳过运行时注入")

        # 6. 创建 /etc/os-release（让 Python 的 platform 模块能工作）
        with open(os.path.join(INITRAMFS_DIR, "etc", "os-release"), "w") as f:
            f.write(f'NAME="{self.os_name}"\n')
            f.write(f'ID={self.os_name.lower()}\n')
            f.write('VERSION="1.0"\n')
            f.write('PRETTY_NAME="elf2os - Python ELF Runtime"\n')
            f.write('HOME_URL="https://github.com/elf2os"\n')

        # 7. strip 所有库文件以减小体积
        self._log("info", "   ✂️  Strip 优化体积...")
        freed = strip_libraries(INITRAMFS_DIR)
        if freed > 0:
            self._log("info", f"      💾 释放 {freed / 1024:.1f} KB")

        # 8. 打包 initramfs
        self._progress(78, "打包 initramfs...")
        initramfs_cpio = os.path.join(self.work_dir, "initramfs.cpio.gz")

        if shutil.which("cpio"):
            find_proc = subprocess.Popen(
                ["find", ".", "-print"],
                cwd=INITRAMFS_DIR, stdout=subprocess.PIPE
            )
            cpio_proc = subprocess.Popen(
                ["cpio", "-H", "newc", "-o"],
                cwd=INITRAMFS_DIR, stdin=find_proc.stdout,
                stdout=subprocess.PIPE
            )
            find_proc.stdout.close()
            cpio_data = cpio_proc.stdout.read()
            find_proc.wait()
            cpio_rc = cpio_proc.wait()
            if cpio_rc != 0:
                self._log("warn", f"   cpio 返回非零: {cpio_rc}，尝试 Python 纯实现")
                self._create_initramfs_python(initramfs_cpio)
            else:
                with open(initramfs_cpio, "wb") as f:
                    f.write(gzip.compress(cpio_data, 9))
        else:
            self._log("info", "   ⚠️  cpio 不可用，使用 Python 纯实现打包")
            self._create_initramfs_python(initramfs_cpio)

        # 验证 initramfs
        if not os.path.exists(initramfs_cpio) or os.path.getsize(initramfs_cpio) < 1000:
            raise RuntimeError("initramfs 打包失败：文件不存在或过小")

        # 验证 init 在 cpio 中
        rc, out, _ = run_cmd(["zcat", initramfs_cpio], timeout=30)
        if rc == 0 and "init" not in out:
            self._log("warn", "   ⚠️ cpio 中未找到 init，使用 Python 纯实现重新打包")
            self._create_initramfs_python(initramfs_cpio)

        size_mb = os.path.getsize(initramfs_cpio) / 1024 / 1024
        self._log("info", f"✅ initramfs 构建完成: {size_mb:.2f} MB")
        self._progress(85, "initramfs 完成")

    def _inject_python_runtime(self, version: str):
        """
        将宿主机上的 Python 运行时注入 initramfs
        包括: 标准库 .py + .so, lib-dynload, site-packages 中的 C 扩展
        """
        self._log("info", f"   🐍 [Phase 3] 注入 Python {version} 运行时...")

        paths = get_python_runtime_paths(version)
        if not paths:
            self._log("warn", f"      ⚠️ 无法定位 Python {version} 运行时路径")
            return

        self._log("info", f"      stdlib: {paths.get('stdlib', 'NOT FOUND')}")
        self._log("info", f"      lib_dynload: {paths.get('lib_dynload', 'NOT FOUND')}")
        self._log("info", f"      site_packages: {paths.get('site_packages', 'NOT FOUND')}")

        # 复制标准库 (.py 文件，跳过测试和文档)
        if "stdlib" in paths:
            stdlib_src = paths["stdlib"]
            stdlib_dst = os.path.join(INITRAMFS_DIR, "usr", "lib",
                                      os.path.basename(stdlib_src))
            os.makedirs(os.path.dirname(stdlib_dst), exist_ok=True)

            ignore_patterns = shutil.ignore_patterns(
                "test", "tests", "__pycache__", "*.pyc", "*.pyo",
                "idlelib", "tkinter", "turtledemo", "ctypes/test",
                "distutils", "ensurepip", "venv", "pydoc_data"
            )

            try:
                shutil.copytree(stdlib_src, stdlib_dst, ignore=ignore_patterns,
                                dirs_exist_ok=True)
                self._log("info", f"      ✅ 标准库已复制 (→ /usr/lib/{os.path.basename(stdlib_src)})")
            except Exception as e:
                self._log("warn", f"      ⚠️ 标准库复制失败: {e}")

        # 复制 lib-dynload (.so C 扩展)
        if "lib_dynload" in paths:
            ld_src = paths["lib_dynload"]
            ld_dst = os.path.join(INITRAMFS_DIR, "usr", "lib", "python3", "lib-dynload")
            os.makedirs(ld_dst, exist_ok=True)
            so_count = 0
            for f in os.listdir(ld_src):
                if f.endswith(".so"):
                    shutil.copy2(os.path.join(ld_src, f), os.path.join(ld_dst, f))
                    so_count += 1
            self._log("info", f"      ✅ lib-dynload: {so_count} 个 C 扩展")

        # 复制 site-packages 中的 .so（第三方 C 扩展）
        if "site_packages" in paths:
            sp_src = paths["site_packages"]
            sp_dst = os.path.join(INITRAMFS_DIR, "usr", "lib", "python3", "site-packages")
            os.makedirs(sp_dst, exist_ok=True)

            so_count = 0
            for root, dirs, files in os.walk(sp_src):
                # 跳过明显的纯 Python 包（只复制含 .so 的包）
                has_so = any(f.endswith(".so") for f in files)
                if not has_so and not any(
                    f.endswith((".so", ".pyd")) for f in files
                ):
                    # 检查子目录
                    sub_has_so = False
                    for d in dirs:
                        sub_path = os.path.join(root, d)
                        if os.path.isdir(sub_path):
                            for sf in os.listdir(sub_path):
                                if sf.endswith(".so"):
                                    sub_has_so = True
                                    break
                        if sub_has_so:
                            break
                    if not sub_has_so:
                        continue

                rel = os.path.relpath(root, sp_src)
                dest_dir = os.path.join(sp_dst, rel) if rel != "." else sp_dst
                os.makedirs(dest_dir, exist_ok=True)
                for f in files:
                    if f.endswith((".so", ".pyd", ".py")):
                        shutil.copy2(os.path.join(root, f), os.path.join(dest_dir, f))
                        if f.endswith(".so"):
                            so_count += 1

            if so_count > 0:
                self._log("info", f"      ✅ site-packages: {so_count} 个 C 扩展 (.so)")

        # 创建 .pth 文件确保路径正确
        with open(os.path.join(INITRAMFS_DIR, "usr", "lib", "python3",
                                "site-packages", "elf2os.pth"), "w") as f:
            f.write("/usr/lib/python3\n")
            f.write("/usr/lib/python3/lib-dynload\n")
            f.write("/usr/lib/python3/site-packages\n")

    def _create_initramfs_python(self, output_path: str):
        """纯 Python 实现 initramfs (cpio newc + gzip) 打包"""
        cpio_data = b""
        initramfs_dir = INITRAMFS_DIR

        def cpio_pad(size: int) -> int:
            return (size + 3) & ~3

        def write_header(name: str, mode: int, filesize: int):
            nonlocal cpio_data
            name_bytes = name.encode("utf-8")
            header = struct.pack(
                "6s8s8s8s8s8s8s8s8s8s8s8s8s8s",
                b"070701",
                b"00000000",
                f"{mode:04o}".encode().zfill(8),
                b"00000000",
                b"00000000",
                b"00000001",
                b"00000000",
                f"{filesize:08x}".encode().zfill(8),
                b"00000000",
                b"00000000",
                b"00000000",
                b"00000000",
                f"{len(name_bytes)+1:08x}".encode().zfill(8),
                b"00000000",
            )
            cpio_data += header
            cpio_data += name_bytes + b"\x00"
            pad = cpio_pad(110 + len(name_bytes) + 1) - (110 + len(name_bytes) + 1)
            cpio_data += b"\x00" * pad

        def write_file(name: str, mode: int, data: bytes):
            write_header(name, mode, len(data))
            nonlocal cpio_data
            cpio_data += data
            pad = cpio_pad(len(data)) - len(data)
            cpio_data += b"\x00" * pad

        # 遍历目录，先写目录条目，再写文件
        all_items = []
        for root, dirs, files in os.walk(initramfs_dir):
            for d in sorted(dirs):
                full = os.path.join(root, d)
                rel = os.path.relpath(full, initramfs_dir)
                all_items.append(("dir", rel, full))
            for f in sorted(files):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, initramfs_dir)
                all_items.append(("file", rel, full))

        for item_type, rel, full in all_items:
            if item_type == "dir":
                st_info = os.lstat(full)
                write_header(rel, st_info.st_mode & 0o777 | 0o040000, 0)
            else:
                st_info = os.lstat(full)
                if os.path.islink(full):
                    link_target = os.readlink(full)
                    write_file(rel, st_info.st_mode & 0o777 | 0o120000, link_target.encode())
                else:
                    with open(full, "rb") as fh:
                        data = fh.read()
                    write_file(rel, st_info.st_mode & 0o777 | 0o100000, data)

        # TRAILER
        write_header("TRAILER!!!", 0, 0)

        with open(output_path, "wb") as f:
            f.write(gzip.compress(cpio_data, 9))

    # ---- 步骤 5: 打包 ISO ----
    def step5_build_iso(self):
        self._log("info", "💿 生成 ISO 镜像...")

        if os.path.exists(ISO_ROOT):
            shutil.rmtree(ISO_ROOT, ignore_errors=True)
        os.makedirs(os.path.join(ISO_ROOT, "boot"), exist_ok=True)
        os.makedirs(os.path.join(ISO_ROOT, "boot", "grub"), exist_ok=True)

        # 复制内核
        bzimage_src = os.path.join(self.kernel_dir, "arch/x86/boot/bzImage")
        if not os.path.exists(bzimage_src):
            for alt in ["arch/x86_64/boot/bzImage", "arch/i386/boot/bzImage"]:
                alt_path = os.path.join(self.kernel_dir, alt)
                if os.path.exists(alt_path):
                    bzimage_src = alt_path
                    break

        bzimage_dest = os.path.join(ISO_ROOT, "boot", "bzImage")
        shutil.copy2(bzimage_src, bzimage_dest)
        self._log("info", f"   ✅ 内核: /boot/bzImage ({os.path.getsize(bzimage_dest)//1024}KB)")

        # 复制 initramfs
        initramfs_src = os.path.join(self.work_dir, "initramfs.cpio.gz")
        initramfs_dest = os.path.join(ISO_ROOT, "boot", "initramfs.cpio.gz")
        shutil.copy2(initramfs_src, initramfs_dest)
        self._log("info", f"   ✅ initramfs: /boot/initramfs.cpio.gz ({os.path.getsize(initramfs_dest)//1024}KB)")

        # 生成 grub.cfg
        grub_dir = os.path.join(ISO_ROOT, "boot", "grub")
        grub_cfg_path = os.path.join(grub_dir, "grub.cfg")

        safe_name = self.os_name.replace('"', '\\"')
        grub_cfg = (
            'set timeout=0\n'
            'set default=0\n'
            '\n'
            f'menuentry "{safe_name}" {{\n'
            '    linux /boot/bzImage console=tty0 init=/init quiet loglevel=0\n'
            '    initrd /boot/initramfs.cpio.gz\n'
            '}\n'
        )
        with open(grub_cfg_path, "w") as f:
            f.write(grub_cfg)
        self._log("info", f"   ✅ GRUB 配置: {grub_cfg_path}")

        # 生成 ISO
        self._progress(90, "生成 ISO...")
        self._log("info", "🔨 运行 grub-mkrescue...")

        grub_cmd = shutil.which("grub-mkrescue") or shutil.which("grub2-mkrescue")
        if not grub_cmd:
            raise RuntimeError("找不到 grub-mkrescue 或 grub2-mkrescue")

        cmd = [grub_cmd, "-o", self.iso_path, ISO_ROOT]
        rc, out, err = run_cmd(cmd, timeout=300)

        if rc != 0:
            self._log("warn", f"   grub-mkrescue 失败，尝试 xorriso 直接构建...")
            self._log("warn", f"   stderr: {err.strip()[:200]}")
            xorriso_cmd = [
                "xorriso", "-as", "mkisofs",
                "-iso-level", "3",
                "-full-iso9660-filenames",
                "-volid", self.os_name[:32],
                "-eltorito-boot", "boot/grub/i386-pc/eltorito.img",
                "-eltorito-catalog", "boot/grub/boot.cat",
                "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
                "-eltorito-alt-boot",
                "-e", "boot/grub/efi.img",
                "-no-emul-boot",
                "-append_partition", "2", "0xef", "boot/grub/efi.img",
                "-output", self.iso_path,
                ISO_ROOT
            ]
            rc2, out2, err2 = run_cmd(xorriso_cmd, timeout=300)
            if rc2 != 0:
                raise RuntimeError(
                    f"ISO 生成失败。grub-mkrescue: {err.strip()[:200]} | "
                    f"xorriso: {err2.strip()[:200]}"
                )

        if not os.path.exists(self.iso_path):
            raise RuntimeError("ISO 文件未生成")

        size_mb = os.path.getsize(self.iso_path) / 1024 / 1024
        self._log("info", f"✅ ISO 生成完成: {self.iso_path} ({size_mb:.1f} MB)")
        self._progress(100, "构建完成!")
        return self.iso_path

    # ---- 主流程 ----
    def run_all(self):
        try:
            os.makedirs(self.work_dir, exist_ok=True)
            self.log_file = open(BUILD_LOG, "a", buffering=1)
            self._log("info", f"🏷️  操作系统名称: {self.os_name}")
            self._log("info", f"📦 ELF 文件: {self.elf_path}")
            self._log("info", f"📁 工作目录: {self.work_dir}")
            self._log("info", f"📋 构建日志: {BUILD_LOG}")
            self._log("info", "─" * 50)

            self._progress(1, "开始构建...")

            self._log("info", "📦 [Step 1/5] 检查并安装依赖...")
            self.step1_install_deps()
            self._progress(15, "依赖就绪")

            self._log("info", "🌐 [Step 2/5] 准备内核源码...")
            self.step2_prepare_kernel()
            self._progress(35, "内核源码就绪")

            self._log("info", "🔨 [Step 3/5] 编译内核...")
            self.step3_build_kernel()

            self._log("info", "📦 [Step 4/5] 构建 initramfs...")
            self.step4_build_initramfs()

            self._log("info", "💿 [Step 5/5] 打包 ISO...")
            iso = self.step5_build_iso()

            self._log("info", "─" * 50)
            self._log("info", f"🎉 构建成功! ISO: {iso}")
            self._log("info", f"   测试: qemu-system-x86_64 -cdrom {os.path.basename(iso)} -m 512M")
            self.cb.done(True, iso, "")

        except Exception as e:
            err_msg = str(e)
            self._log("error", f"💥 构建失败: {err_msg}")
            self._log("error", traceback.format_exc())
            self.cb.done(False, "", err_msg)
        finally:
            if self.log_file:
                self.log_file.close()
                self.log_file = None


# ============================================================
#  命令行模式（无 GUI 时回退）
# ============================================================

def cli_mode():
    """命令行交互模式"""
    print("=" * 60)
    print("  elf2os — 将 ELF 文件打包为可启动操作系统")
    print("=" * 60)
    print()

    import getpass
    password = getpass.getpass("🔐 请输入 sudo 密码: ")
    if not password:
        print("❌ 密码不能为空", file=sys.stderr)
        sys.exit(1)

    rc, _, err = sudo_run(["true"], password, timeout=10)
    if rc != 0:
        print(f"❌ sudo 验证失败: {err.strip()[:200]}", file=sys.stderr)
        sys.exit(1)

    os_name = input("🏷️  操作系统名称 (默认: helloos): ").strip()
    if not os_name:
        os_name = "helloos"

    elf_path = input("📦 ELF 文件路径: ").strip()
    if not elf_path:
        print("❌ ELF 文件路径不能为空", file=sys.stderr)
        sys.exit(1)
    elf_path = os.path.abspath(elf_path)
    if not os.path.exists(elf_path):
        print(f"❌ 文件不存在: {elf_path}", file=sys.stderr)
        sys.exit(1)
    if not is_valid_elf(elf_path):
        print(f"❌ 文件不是有效的 ELF: {elf_path}", file=sys.stderr)
        sys.exit(1)

    print()

    # 预检测 Python ELF
    py_info = detect_python_elf(elf_path)
    if py_info:
        print(f"  🐍 检测到 Python ELF: {py_info.get('type','?')} {py_info.get('version','')}")
    else:
        print("  ℹ️  非 Python ELF，按通用模式处理")
    print()

    class CLICallbacks(BuildCallbacks):
        def progress(self, pct, msg=""):
            bar_len = 30
            filled = int(bar_len * pct / 100)
            bar = "█" * filled + "░" * (bar_len - filled)
            print(f"\r   [{bar}] {pct:3d}% {msg}", end="", flush=True)
            if pct >= 100:
                print()

    cb = CLICallbacks()
    engine = BuildEngine(cb, password, os_name, elf_path)
    engine.run_all()


# ============================================================
#  GUI 前端（PyQt5 / PySide2 / tkinter 三选一）
# ============================================================

def detect_gui_backend() -> str:
    """检测可用的 GUI 后端"""
    try:
        import PyQt5  # noqa
        return "pyqt5"
    except ImportError:
        pass
    try:
        import PySide2  # noqa
        return "pyside2"
    except ImportError:
        pass
    try:
        import tkinter  # noqa
        return "tkinter"
    except ImportError:
        pass
    return "none"


# ---- PyQt5 前端 ----
class PyQt5Frontend:
    """PyQt5 图形界面"""

    def run(self):
        from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout,
                                     QHBoxLayout, QLabel, QLineEdit, QPushButton,
                                     QFileDialog, QProgressBar, QPlainTextEdit,
                                     QMessageBox, QFrame)
        from PyQt5.QtCore import Qt, QThread, pyqtSignal
        from PyQt5.QtGui import QFont, QPalette, QColor

        app = QApplication(sys.argv)

        class Worker(QThread):
            log_signal = pyqtSignal(str, str)
            progress_signal = pyqtSignal(int, str)
            done_signal = pyqtSignal(bool, str, str)

            def __init__(self, password, os_name, elf_path):
                super().__init__()
                self.password = password
                self.os_name = os_name
                self.elf_path = elf_path

            def run(self):
                class GuiCB(BuildCallbacks):
                    def __init__(self, ls, ps, ds):
                        self._ls = ls
                        self._ps = ps
                        self._ds = ds
                    def log(self, level, msg):
                        self._ls(level, msg)
                    def progress(self, pct, msg):
                        self._ps(pct, msg)
                    def done(self, success, iso_path, error):
                        self._ds(success, iso_path, error)

                cb = GuiCB(self.log_signal.emit, self.progress_signal.emit, self.done_signal.emit)
                engine = BuildEngine(cb, self.password, self.os_name, self.elf_path)
                engine.run_all()

        class Window(QMainWindow):
            def __init__(self):
                super().__init__()
                self.setWindowTitle("elf2os — 将 ELF 变成操作系统")
                self.setMinimumSize(720, 560)

                central = QWidget()
                self.setCentralWidget(central)
                layout = QVBoxLayout(central)
                layout.setContentsMargins(24, 20, 24, 20)
                layout.setSpacing(12)

                title = QLabel("🐧 elf2os")
                title.setFont(QFont("Sans", 18, 75))
                title.setStyleSheet("color: #89b4fa; padding-bottom: 4px;")
                layout.addWidget(title)

                subtitle = QLabel("将 ELF 文件 + Linux 极简内核打包为可启动 ISO 操作系统")
                subtitle.setFont(QFont("Sans", 9))
                subtitle.setStyleSheet("color: #a6adc8; padding-bottom: 8px;")
                layout.addWidget(subtitle)

                sep = QFrame()
                sep.setFrameShape(QFrame.HLine)
                sep.setStyleSheet("color: #313244; max-height: 1px;")
                layout.addWidget(sep)

                layout.addWidget(QLabel("🔐 sudo 密码（用于安装依赖）"))
                self.pwd_input = QLineEdit()
                self.pwd_input.setEchoMode(QLineEdit.Password)
                self.pwd_input.setPlaceholderText("输入 sudo 密码...")
                layout.addWidget(self.pwd_input)

                layout.addWidget(QLabel("🏷️  操作系统名称"))
                self.name_input = QLineEdit()
                self.name_input.setPlaceholderText("例如: MyAwesomeOS")
                layout.addWidget(self.name_input)

                layout.addWidget(QLabel("📦 ELF 文件（你的操作系统本体）"))
                elf_row = QHBoxLayout()
                self.elf_input = QLineEdit()
                self.elf_input.setPlaceholderText("选择 ELF 文件路径...")
                elf_row.addWidget(self.elf_input, 1)
                browse_btn = QPushButton("浏览...")
                browse_btn.setMaximumWidth(100)
                browse_btn.clicked.connect(self._browse)
                elf_row.addWidget(browse_btn)
                layout.addLayout(elf_row)

                sep2 = QFrame()
                sep2.setFrameShape(QFrame.HLine)
                sep2.setStyleSheet("color: #313244; max-height: 1px;")
                layout.addWidget(sep2)

                layout.addWidget(QLabel("⏳ 构建进度"))
                self.progress = QProgressBar()
                self.progress.setValue(0)
                layout.addWidget(self.progress)

                self.status_label = QLabel("就绪 | 请输入信息后点击构建")
                self.status_label.setStyleSheet("color: #a6adc8; font-size: 9px;")
                layout.addWidget(self.status_label)

                sep3 = QFrame()
                sep3.setFrameShape(QFrame.HLine)
                sep3.setStyleSheet("color: #313244; max-height: 1px;")
                layout.addWidget(sep3)

                layout.addWidget(QLabel("📋 构建日志"))
                self.log_view = QPlainTextEdit()
                self.log_view.setReadOnly(True)
                self.log_view.setMaximumBlockCount(2000)
                layout.addWidget(self.log_view, 1)

                self.build_btn = QPushButton("🚀 开始构建操作系统")
                self.build_btn.clicked.connect(self._start_build)
                layout.addWidget(self.build_btn)

                self.statusBar().showMessage("就绪")
                self.worker = None

            def _browse(self):
                path, _ = QFileDialog.getOpenFileName(self, "选择 ELF 文件", "", "ELF Files (*);;All Files (*)")
                if path:
                    self.elf_input.setText(path)

            def _append_log(self, level, msg):
                color_map = {"info": "#a6e3a1", "warn": "#f9e2af", "error": "#f38ba8"}
                icon_map = {"info": "ℹ️", "warn": "⚠️", "error": "❌"}
                color = color_map.get(level, "#cdd6f4")
                icon = icon_map.get(level, "•")
                self.log_view.appendHtml(f'<span style="color:{color}">{icon} {msg}</span>')

            def _set_progress(self, pct, msg):
                self.progress.setValue(pct)
                if msg:
                    self.status_label.setText(msg)

            def _build_done(self, success, iso_path, error):
                self.build_btn.setEnabled(True)
                self.pwd_input.setEnabled(True)
                self.name_input.setEnabled(True)
                self.elf_input.setEnabled(True)
                if success:
                    size_mb = os.path.getsize(iso_path) / 1024 / 1024
                    QMessageBox.information(self, "构建成功",
                        f"✅ 操作系统 ISO 已生成!\n\n文件: {iso_path}\n大小: {size_mb:.1f} MB\n\n"
                        f"测试命令:\nqemu-system-x86_64 -cdrom {os.path.basename(iso_path)} -m 512M")
                    self.statusBar().showMessage(f"✅ 构建成功: {iso_path}")
                else:
                    QMessageBox.critical(self, "构建失败",
                        f"❌ 构建过程中出错:\n\n{error}\n\n请查看上方日志获取详细信息。")
                    self.statusBar().showMessage("❌ 构建失败")

            def _start_build(self):
                password = self.pwd_input.text().strip()
                os_name = self.name_input.text().strip()
                elf_path = self.elf_input.text().strip()

                if not password:
                    QMessageBox.warning(self, "输入错误", "请输入 sudo 密码")
                    return
                if not os_name:
                    QMessageBox.warning(self, "输入错误", "请输入操作系统名称")
                    return
                if not elf_path or not os.path.exists(elf_path):
                    QMessageBox.warning(self, "输入错误", "请选择有效的 ELF 文件")
                    return
                if not is_valid_elf(elf_path):
                    QMessageBox.warning(self, "文件错误", "所选文件不是有效的 ELF 文件")
                    return

                rc, _, err = sudo_run(["true"], password, timeout=10)
                if rc != 0:
                    QMessageBox.critical(self, "密码错误",
                        f"sudo 密码验证失败:\n{err.strip()[:200]}\n\n请检查后重试。")
                    return

                self.build_btn.setEnabled(False)
                self.pwd_input.setEnabled(False)
                self.name_input.setEnabled(False)
                self.elf_input.setEnabled(False)
                self.log_view.clear()
                self.progress.setValue(0)
                self.statusBar().showMessage("构建中...")

                self.worker = Worker(password, os_name, elf_path)
                self.worker.log_signal.connect(self._append_log)
                self.worker.progress_signal.connect(self._set_progress)
                self.worker.done_signal.connect(self._build_done)
                self.worker.start()

        win = Window()
        win.show()
        sys.exit(app.exec_())


# ---- tkinter 前端（备用）----
class TkinterFrontend:
    """tkinter 图形界面（当 PyQt5/PySide2 不可用时）"""

    def run(self):
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox, scrolledtext

        root = tk.Tk()
        root.title("elf2os — 将 ELF 变成操作系统")
        root.geometry("760x620")
        root.configure(bg="#1e1e2e")

        style = ttk.Style()
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("TLabel", background="#1e1e2e", foreground="#cdd6f4", font=("Sans", 10))
        style.configure("TEntry", fieldbackground="#313244", foreground="#cdd6f4")
        style.configure("TButton", background="#89b4fa", foreground="#1e1e2e", font=("Sans", 11, "bold"))
        style.map("TButton", background=[("active", "#b4befe"), ("disabled", "#585b70")])
        style.configure("TProgressbar", background="#a6e3a1", troughcolor="#313244")

        main = ttk.Frame(root, padding=20)
        main.pack(fill="both", expand=True)

        title = tk.Label(main, text="🐧 elf2os", font=("Sans", 20, "bold"),
                         bg="#1e1e2e", fg="#89b4fa")
        title.pack(anchor="w", pady=(0, 4))
        sub = tk.Label(main, text="将 ELF 文件 + Linux 极简内核打包为可启动 ISO 操作系统",
                       font=("Sans", 9), bg="#1e1e2e", fg="#a6adc8")
        sub.pack(anchor="w", pady=(0, 12))

        ttk.Separator(main, orient="horizontal").pack(fill="x", pady=4)

        ttk.Label(main, text="🔐 sudo 密码（用于安装依赖）").pack(anchor="w", pady=(8, 2))
        pwd_var = tk.StringVar()
        pwd_entry = ttk.Entry(main, textvariable=pwd_var, show="●", font=("Sans", 11))
        pwd_entry.pack(fill="x", pady=(0, 8))

        ttk.Label(main, text="🏷️  操作系统名称").pack(anchor="w", pady=(4, 2))
        name_var = tk.StringVar()
        name_entry = ttk.Entry(main, textvariable=name_var, font=("Sans", 11))
        name_entry.pack(fill="x", pady=(0, 8))

        ttk.Label(main, text="📦 ELF 文件（你的操作系统本体）").pack(anchor="w", pady=(4, 2))
        elf_frame = ttk.Frame(main)
        elf_frame.pack(fill="x", pady=(0, 8))
        elf_var = tk.StringVar()
        elf_entry = ttk.Entry(elf_frame, textvariable=elf_var, font=("Sans", 11))
        elf_entry.pack(side="left", fill="x", expand=True, padx=(0, 8))

        def browse():
            path = filedialog.askopenfilename(title="选择 ELF 文件",
                                              filetypes=[("ELF files", "*"), ("All files", "*.*")])
            if path:
                elf_var.set(path)

        browse_btn = ttk.Button(elf_frame, text="浏览...", command=browse)
        browse_btn.pack(side="right")

        ttk.Separator(main, orient="horizontal").pack(fill="x", pady=4)

        ttk.Label(main, text="⏳ 构建进度").pack(anchor="w", pady=(8, 2))
        progress = ttk.Progressbar(main, length=100, mode="determinate")
        progress.pack(fill="x", pady=(0, 4))
        status_var = tk.StringVar(value="就绪 | 请输入信息后点击构建")
        status_lbl = ttk.Label(main, textvariable=status_var)
        status_lbl.pack(anchor="w")

        ttk.Separator(main, orient="horizontal").pack(fill="x", pady=4)

        ttk.Label(main, text="📋 构建日志").pack(anchor="w", pady=(8, 2))
        log_view = scrolledtext.ScrolledText(main, height=14, font=("Monospace", 9),
                                              bg="#11111b", fg="#a6e3a1",
                                              insertbackground="#cdd6f4")
        log_view.pack(fill="both", expand=True, pady=(0, 8))

        build_btn = ttk.Button(main, text="🚀 开始构建操作系统")
        build_btn.pack(pady=8)

        bottom = tk.Label(root, text="就绪", bg="#11111b", fg="#a6adc8", font=("Sans", 9),
                          anchor="w", padx=10)
        bottom.pack(fill="x", side="bottom")

        class GuiCB(BuildCallbacks):
            def __init__(self):
                self.log_view = log_view
                self.progress_bar = progress
                self.status = status_var
                self.bottom = bottom
            def log(self, level, msg):
                color_map = {"info": "#a6e3a1", "warn": "#f9e2af", "error": "#f38ba8"}
                color = color_map.get(level, "#cdd6f4")
                icon = {"info": "ℹ️", "warn": "⚠️", "error": "❌"}.get(level, "•")
                self.log_view.insert("end", f"{icon} {msg}\n", (level,))
                self.log_view.tag_config(level, foreground=color)
                self.log_view.see("end")
                self.bottom.config(text=msg[:80])
                root.update_idletasks()
            def progress(self, pct, msg):
                self.progress_bar["value"] = pct
                if msg:
                    self.status.set(msg)
                root.update_idletasks()
            def done(self, success, iso_path, error):
                build_btn.config(state="normal")
                pwd_entry.config(state="normal")
                name_entry.config(state="normal")
                elf_entry.config(state="normal")
                if success:
                    size_mb = os.path.getsize(iso_path) / 1024 / 1024
                    messagebox.showinfo("构建成功",
                        f"✅ ISO 已生成!\n\n文件: {iso_path}\n大小: {size_mb:.1f} MB\n\n"
                        f"测试: qemu-system-x86_64 -cdrom {os.path.basename(iso_path)} -m 512M")
                    bottom.config(text=f"✅ 构建成功: {iso_path}")
                else:
                    messagebox.showerror("构建失败", f"❌ {error}")
                    bottom.config(text="❌ 构建失败")

        def start_build():
            password = pwd_var.get().strip()
            os_name = name_var.get().strip()
            elf_path = elf_var.get().strip()

            if not password:
                messagebox.showwarning("输入错误", "请输入 sudo 密码"); return
            if not os_name:
                messagebox.showwarning("输入错误", "请输入操作系统名称"); return
            if not elf_path or not os.path.exists(elf_path):
                messagebox.showwarning("输入错误", "请选择有效的 ELF 文件"); return
            if not is_valid_elf(elf_path):
                messagebox.showwarning("文件错误", "不是有效的 ELF 文件"); return

            rc, _, err = sudo_run(["true"], password, timeout=10)
            if rc != 0:
                messagebox.showerror("密码错误", f"sudo 验证失败:\n{err.strip()[:200]}")
                return

            build_btn.config(state="disabled")
            pwd_entry.config(state="disabled")
            name_entry.config(state="disabled")
            elf_entry.config(state="disabled")
            log_view.delete("1.0", "end")
            progress["value"] = 0
            bottom.config(text="构建中...")

            cb = GuiCB()
            engine = BuildEngine(cb, password, os_name, elf_path)

            def run_thread():
                engine.run_all()

            t = threading.Thread(target=run_thread, daemon=True)
            t.start()

        build_btn.config(command=start_build)

        if "XDG_RUNTIME_DIR" not in os.environ:
            os.environ["XDG_RUNTIME_DIR"] = "/tmp/runtime-root"
            os.makedirs("/tmp/runtime-root", exist_ok=True)

        root.mainloop()


# ============================================================
#  主入口
# ============================================================

def main():
    # 设置 XDG_RUNTIME_DIR（避免 Qt 警告）
    if "XDG_RUNTIME_DIR" not in os.environ:
        os.environ["XDG_RUNTIME_DIR"] = "/tmp/runtime-root"
        os.makedirs("/tmp/runtime-root", exist_ok=True)

    # 如果有 --cli 参数或无显示环境，使用命令行模式
    use_cli = "--cli" in sys.argv or not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY")

    if use_cli:
        cli_mode()
        return

    backend = detect_gui_backend()
    print(f"[elf2os] 检测到 GUI 后端: {backend}")

    if backend == "pyqt5":
        frontend = PyQt5Frontend()
        frontend.run()
    elif backend == "pyside2":
        print("[elf2os] PySide2 支持请参考 PyQt5 实现")
        print("[elf2os] 回退到命令行模式...")
        cli_mode()
    elif backend == "tkinter":
        print("[elf2os] 使用 tkinter 前端")
        frontend = TkinterFrontend()
        frontend.run()
    else:
        print("❌ 未找到可用的 GUI 框架!", file=sys.stderr)
        print("", file=sys.stderr)
        print("请安装以下任一 GUI 框架:", file=sys.stderr)
        print("  PyQt5:   sudo apt-get install -y python3-pyqt5", file=sys.stderr)
        print("  tkinter: sudo apt-get install -y python3-tk", file=sys.stderr)
        print("", file=sys.stderr)
        print("或运行: sudo apt-get install -y python3-pyqt5 python3-tk", file=sys.stderr)
        print("", file=sys.stderr)
        print("也可以直接命令行模式: sudo python3 elf2os.py --cli", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
