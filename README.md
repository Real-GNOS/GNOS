# GNOS

<p align="center">
  <img src="assets/gnos-icon.png" alt="GNOS" width="200">
</p>

<p align="center">
  <a href="https://github.com/Real-GNOS/GNOS">GitHub: Real-GNOS/GNOS</a>
</p>

> 🚀 **这是你见过迄今为止最强的小学生开发的操作系统** —— 由一位**六年级学生**独立开发。
> x86_64 真内核：SMP 多核 + EEVDF 现代调度器 + Linux ABI + 网络栈 + Wayland 桌面。

GNOS 是一个面向 x86_64 的教学型操作系统。它用约 5.9 万行自研代码实现了一个
尽可能贴近 Linux 的用户态 ABI，使得 **musl、BusyBox 1.38、GNU Bash 5.3 和
GNU coreutils 9.9 这些真实的第三方用户软件可以直接在其上运行**——不靠兼容层、
不靠模拟，靠的是内核本身。

> 代码中的命名前缀多为 GNU/gnucos；内核镜像为 `GNOSKr.elf`，项目文档见
> `ANALYSIS.md`。许可（GPL-2.0）见 `LICENSE`，第三方致谢汇总见 `NOTICE`，
> 贡献约定和架构细则见 `CONTRIBUTING.md`（英文版 `CONTRIBUTING.md.en`）。

（English README: `README.en.md`）

## 设计哲学

- **Linux/x86-64 syscall ABI 是唯一契约**：系统调用编号、寄存器约定、错误码
  （负 errno）均对齐 Linux（见 `src/include/uapi/linux/sysnum.h`，当前 219 个调用）。内核与
  用户态共享这一份头文件，二者不会漂移。
- **真货优先**：与其自造玩具用户态，不如把内核做对，让成熟的第三方用户栈直接
  跑起来。Bash 的 `bash_cv_termcap_lib=gnutermcap`、coreutils 的
  `linux-stub` 都是出于这一原则的手工配置。
- **SMP + EEVDF**：多核支持（Limine SMP 协议，最多 16 核；每核独立 GS 基址/私有 GDT+TSS，
  syscall 入口按每核内核栈切换）。调度器为 **EEVDF**（与 Linux 6.6+ 同款：全局虚拟时钟、
  进程虚拟运行时间、虚拟截止时间 = vtime + slice、二叉最小堆运行队列、timer 中断里的
  截止时间抢占判定）。运行队列由自旋锁保护，其余共享状态走大内核锁（BKL）。
- **一切可复现、可断言**：开机由 `/etc/rc` 自动执行 `bashtest.sh` 与
  `coreutilstest.sh` 的逐条断言，`make test` 无头跑一遍 QEMU 并打印 debugcon
  日志，任何内核回归在出现的那一刻就被抓到。

## 里程碑（按提交顺序）

| 提交 | 内容 |
|------|------|
| `2b034a0` | 初始版本：引导、PMM/VMM、进程、VFS、ext2/fat、ulib 用户态 |
| `abdf1a1` | 网络栈随内核启动；e1000 + 自研 tcp/sock/net |
| `c79795d` | tmpfs、procfs、ext2 符号链接/rename、信号、OpenRC 脚手架 |
| `860a584` | gfx/fbdev 帧缓冲（/dev/fb0 可 mmap）、子系统注册表、内核堆、ACPI、coldplug、fstab、pread/pwrite |
| `53a2495` | coreutils 支持 + 系统调用补齐（约 660 行） |
| `4f801ea` | tty 增加 ioctl（TCGETS/TCSETS*/TIOCGWINSZ/…），init 登录 shell 切换为交互式 bash |

## 架构

**引导**（`src/bootloader/`）：Limine 协议（BIOS 与 UEFI 双模式）；内核为
`-static-pie` ELF，被 Limine 重定位进高半区（`0xFFFFFFFF80000000` 附近）；
根文件系统是 256 MiB ext2 内存盘（initrd），内核直接在内存中挂载读写。

**内核**（`src/kernel/`）：

| 子系统 | 说明 |
|--------|------|
| 内存 | PMM 物理帧、VMM 4 级页表、HHDM 直接映射、kheap（边界标签分配，启动时定长） |
| 架构 | GDT/IDT/ISR/中断、syscall 入口（Linux ABI）、TSC/定时器 |
| 进程 | fork/exec/wait、每进程独立页表、抢占式用户态调度、信号、线程（CLONE_VM/futex） |
| TTY | termios 行规程（`src/kernel/core/tty.c`）、ioctl（TCGETS/TCSETS*/TIOCGWINSZ/…） |
| 文件系统 | VFS（挂载表 + fstab）+ tmpfs + procfs + ext2（符号链接/rename）+ fat |
| 图形 | fbcon 文本控制台、gfx、fbdev（Linux 风格 `/dev/fb0`，支持 mmap）、DRM 驱动目录（`driver/drm/`，legacy + ported） |
| 网络 | e1000 驱动 + 自研 tcp/sock 协议栈（`core/tcp.c`/`core/sock.c`/`core/net.c`）+ AF_UNIX 套接字（`unix.c`） |
| 声音 | HDA 与 AC97 |
| 其他 | ACPI、PCI、子系统注册表（`core/subsys.c`）、coldplug、ptrace、epoll、timerfd/signalfd/anonfd |

**用户态**（`src/user/` + 镜像）：

- `ulib`：自研最小库 + 13 个工具（init、shell、count、ls、cat、tail、tac、rm、mkdir、touch、scan、dbgcat、envtest），固定加载地址 `0x400000`
- musl 1.2.5 静态程序 16 个（hello、mount、coldplug、chvt、getty、login、installer、ttytest、thrtest、drmtest、ptracetest、insmod、rmmod、evtest、eventest、socktest）+ 动态链接 `dynhello`（ET_DYN + ld-musl，验证 loader 的 PIE/AT_* 链路）
- BusyBox 1.38（含 sh/ash 多调用调度）
- **GNU Bash 5.3**（musl 静态、`-no-pie`、禁用 bash-malloc，登录 shell 交互模式）
- **GNU coreutils 9.9**（配 `linux-stub` 头 + musl-gcc，全量装进 `/usr/bin`）
- OpenRC 体系（`/etc/rc`、启停脚本接线），`/etc` 有 passwd、group、hosts、nsswitch.conf、resolv.conf、fstab、services 等一整套

## 目录结构

```
GNOS/
├── src/
│   ├── arch/            # GDT/IDT/ISR、SMP 入口、页表
│   ├── bootloader/      # Limine 引导相关
│   ├── drivers/         # PCI、e1000、ATA/NVMe、HDA/AC97、输入、DRM……
│   ├── fs/              # VFS、tmpfs、procfs、ext2、fat、iso9660
│   ├── include/uapi/linux/sysnum.h  # 内核/用户共享契约（syscall 编号）
│   ├── init/            # 内核入口、kaslr、链接脚本
│   ├── kernel/          # 进程/调度/syscall/原语内核本体（约 4.8 万行，含 fs/mm/arch……）
│   ├── mm/              # PMM、VMM、COW、kheap
│   ├── net/             # e1000 栈、tcp、sock、AF_UNIX
│   ├── usr/             # init、getty、login、shell、测试程序
│   ├── rootfs/          # 覆盖进 initrd 的 /etc（fstab、bashrc…）与起动脚本
│   └── vendor/          # acpica、mbedtls、fatfs 等第三方（见 NOTICE）
├── lib/                 # termcap 等
├── limine/              # Limine 引导文件（limine-bios-cd.bin 等）
├── tools/               # 构建辅助脚本
├── Makefile             # 全量构建
├── linker.ld            # 高半区内核链接脚本
├── limine.conf
└── ANALYSIS.md          # 深度分析报告
```

第三方源码树（musl、busybox、bash、coreutils、openrc）放在 `build/` 下，由手工
fetch 并配置；`make clean` **刻意不清除它们**，避免毁掉手工工具链。

## 构建与运行

依赖：`gcc`（支持 `-static-pie`）、`ld`、`mke2fs`、`qemu-system-x86_64`。

```bash
make all        # 构建内核、用户程序、initrd 与 gnos.iso
make run        # QEMU（BIOS 引导，GTK 窗口）
make run-uefi   # QEMU（OVMF/UEFI 引导）
make guistart   # 演示模式：真实音频后端 + debugcon 落盘 + 不无限重启
make test       # 无头自检：跑 20s QEMU，断言测试随开机执行，打印 debugcon 日志
make clean      # 清理构建产物（保留第三方源码树）
```

QEMU 配置：512 MiB 内存、e1000 网卡（user 网络）、音频设备（HDA + AC97）。

## 现状与限制

- SMP 多核：Limine SMP 协议，最多 16 核；运行队列由自旋锁保护，其余共享状态
  走大内核锁（BKL）。
- 用户程序固定加载地址 `0x400000`；loader 同时接受 ET_EXEC 与 ET_DYN（PIE +
  ld-musl 动态链接，`dynhello` 佐证）；每进程独立页表，地址永不冲突。
- 根文件系统默认是内存盘（ext2，256 MiB）；`installer` 可将系统安装到真实块设备
  （ata 磁盘引导）。
- 图形：fbcon/fbdev 之外已有 DRM 驱动与 Wayland 桌面栈（`startxfce` 拉起 labwc
  合成器 + Xfce），`/dev/fb0` 仍为最后写入者胜。
- 219 个系统调用已覆盖 musl（含 pthread）/Bash/coreutils/BusyBox 的实际使用路径，
  但不是完整的 Linux 面（如部分网络 syscall 深度等）。
- 教学定位：无完整安全边界、无多用户隔离。

## 许可

GPL-2.0（见 LICENSE）。代码头与文档中的分析报告同遵循该项目精神：自研部分
GPLv2，第三方组件各自遵循上游许可。

> **第三方组件来源**：`src/include/limine.h` 为净室重写——仅依据 Limine 引导
> 协议的公开行为规范重新实现结构布局，未复制任何 Apache-2.0 源文件，故整体为
> GPLv2。其余第三方源码树（musl、busybox、bash、coreutils、openrc 等）各自遵循
> 其上游许可，见上文「构建与运行」。

## KASLR

内核每次开机的虚拟基址都不同。Limine 把内核固定映射在 0xFFFFFFFF80000000
且协议不接受运行时改基址，所以内核在入口处**自己搬自己**（src/init/kaslr.c）：

1. 用同一份物理页在新窗口建 2 MiB 别名映射（ Limine 的页表经 hhdm 原地修改）；
2. 经 hhdm 重写全部 724 条 R_X86_64_RELATIVE 重定位（不能走旧视图——
   .limine_requests 段是只读映射，写它必 page fault）；
3. 间接跳转到新基址，永不回头。熵源 RDTSC ^ RDRAND，约 460 个槽位。

配套改动：vmm.c 的 walk() 遇到大页自动拆分成 4K 页表（此前是 panic）；
kernel_entry 拆成薄入口 + kernel_main（ relocated 后执行流不得再回旧基址）。

验证（headless 三连启动）：kvirt = 0xFFFFFFFFA9BAD000 / B73B1000 / 99B9D000，
各不相同；5 项自检全 PASS；启动一路走到 SMP 调度器。物理基址由 Limine
随机化（kphys 每次微变，如 0xBA99D000/0xBA9A3000/0xBA9AD000）。

