# HBOS vs GNOS 全方位激烈吐槽（2026-10 源码复核版）

> 上一版把版本、行数、SMP、USB/音频、桌面状态全写错了。这一版只认源码、构建文件和测试脚本：
> **HBOS** `1e67acb`（2026-10-02，315 commits）+ **HIVE** `d98cd3f`（2026-10-18）+ **HPT**；
> **GNOS** 本仓库 `1800cd91`（2026-10-18，91 commits）。
> 本机（Termux/Android）没有 qemu-system-x86_64，所有"能跑"结论来自源码、QEMU 测试脚本与 CI 配置，不等于我亲手跑过。

## 一句话结论

| 你想干什么 | 选谁 |
|---|---|
| 开机直接进一个自己写的桌面 | **HBOS** |
| 跑 Alpine/apk/bash/coreutils/Python/curl | **GNOS** |
| 多核调度、内核模块、DRM、NVMe | **GNOS** |
| 项目自带桌面/编译器/包管理器（子模块） | **HBOS** |
| 图形驱动底座（DRM/KMS） | **GNOS** |
| 图形"今天就能用" | **HBOS** |

## 基本盘

| | HBOS | GNOS |
|---|---|---|
| 仓库 | `HeBitOS/HBOS` @ `1e67acb`，315 提交（2026-05 → 10），另有 HIVE/HPT 子模块 | 本仓库 @ `1800cd91`，91 提交（2026-08 → 10） |
| 许可证 | GPLv3 | GPLv2 |
| 版本 | `version.mk` = 0.1-beta5-**pre18**；README 还写着 pre6 | 无版本号，commit 即版本 |
| 第一方代码 | `src` 73,440 行（其中内核向 39,600）+ HIVE 19,103 + HPT 1,552 | 84,492 行（包含 13,776 行 Linux DRM 移植） |
| 第三方 | quickjs 79,982 / tcc 40,247 / busybox 4,046 / musl-math 5,156，另有 mbedtls/lwip 子模块 | 447,703 行：ACPICA 263,642 / mbedtls 159,039 / fatfs 24,838 |
| 系统调用 | native 139 个（`int 0x80`）+ Linux 号 107 个（`syscall`） | 222 个 `SYS_` 名 / 218 个唯一编号（README 写 219） |
| 上限 | `MAX_TASKS 64`、`POSIX_MAX_FDS 128` | `MAX_PROCS 64`、`PROC_MAX_FD 64`、`MAX_CPUS 16` |
| 启动 | BIOS：GRUB2 multiboot2；UEFI：Limine | Limine，BIOS+UEFI 双模式 |
| 桌面 | HIVE 桌面，默认镜像包含，`make nogui` 可关 | `startxfce` 脚本在；桌面栈是 `build/desk` 手工产物，镜像不带，Makefile 明确禁用 |
| 包管理 | HPT（自有格式/仓库） | apk（Alpine v3.20） |
| CI | 主仓库没有；HIVE/HPT 各有 GitHub Actions | 主仓库有：format + headers + 内核构建 + QEMU 启动 |

## 先把旧版的谣言枪毙

- **"HBOS SMP 8 核 ✅"** —— 假的。`src/smp.c:184-192` 原话：跳板在实模式→保护模式→长模式切换中三重故障，QEMU `-smp 4` 实测 BSP 发出第一个 STARTUP IPI 后整机复位，所以 `smp_init()` 最后只做了 `(void)start_ap;`。**代码在，核没起来**。
- **"HBOS int 0x80 + 137 个调用，形似神不似"** —— 只说对一半。native ABI 确实是 `int $0x80`（`src/user/syscall.c:31`），但 Linux 兼容走的是 `MSR_LSTAR` + `syscall`（`src/core/interrupt_asm.asm` 的 `linux_syscall_entry`），Linux 号 107 个，native 139 个。它有两套入口，不是一套。
- **"HBOS 70k 行全自研"** —— 少算了。`src` 73.4k + HIVE 19.1k + HPT 1.6k，另外还塞了 quickjs/tcc/busybox 共 129k 行第三方移植。
- **"GNOS 215k 行，159k 是 Linux DRM"** —— 数字全错。`src` 总共 532,195 行：vendor 447,703（ACPICA 263,642、mbedtls 159,039、fatfs 24,838），第一方 84,492，其中 DRM 移植 13,776。**DRM 不是 159k，ACPICA 才是大头。**
- **"GNOS 没 USB、音频只有头文件、没 TLS、桌面还在交叉编译"** —— 2026-08 的老黄历。现在 USB xHCI（HID + MSC）有了，HDA/AC97 有了，curl 8.9.1 + mbedtls 2.28.9 有了，wlroots/labwc/XFCE 也在作者机器上构建过。但——**桌面没进镜像，默认禁用了**，见下面。
- **"GNOS 158 个系统调用、11 个提交"** —— 头文件 222 个名字（218 个唯一编号，最大 449），提交 91 个。

## 内核：EEVDF+SMP vs 轮转+单核

| | HBOS | GNOS |
|---|---|---|
| 调度 | 全局循环链表轮转，PIT 抢占，`MAX_TASKS 64` | EEVDF，每核独立运行队列，虚拟时间 + 虚拟截止时间 |
| SMP | **实际单核**（AP 跳板三重故障，启动被注释掉） | Limine SMP，最多 16 核，AP 真实上线 |
| 地址空间 | 每任务独立页表，COW fork | 每进程独立页表，COW，KASLR（每次启动随机偏移） |
| 内核模块 | 无，全静态 | `insmod`/`rmmod`，ELF 模块 + `moddemo` |
| 隔离特性 | 无 seccomp/namespace | seccomp、landlock、cgroup、pidfd、ptrace |

HBOS 的调度器是教科书式"绕一圈找第一个 READY"，老实、能跑、一眼看懂；GNOS 直接把 Linux 6.6 的 EEVDF 搬了进来。差距不在 64 个任务槽，而在**HBOS 的第二颗核根本还没醒**。`src/smp.c` 那段注释倒是非常诚实——比 README 里任何一句都诚实。

GNOS 也别吹成数据中心内核：共享状态还有大锁，cgroup/landlock 是教学级实现，16 核是 Limine 给的上限不是实测上限。但至少 `make test` 是用 `-smp 4` 启动的，AP 真的在跑。

## 系统调用与 Linux ABI：两条腿 vs 一条腿

- **HBOS**：native 139 个 `HBOS_SYS_*`（`int 0x80`）+ Linux 号 107 个（`syscall`）。`tests/` 里 34 个用例，覆盖真实 musl 1.2.5 动态程序、未修改的 glibc 2.43 loader/libc、`dlopen` 依赖链、`clone`/`clone3`/TLS、futex、epoll ET、inotify、signalfd、timerfd、AF_UNIX/SCM_RIGHTS。`docs/LINUX_KDE_COMPAT.md` 自己承认：完整 TLS ABI、pthread 取消、复杂 glibc 依赖还没完成，大型 Linux 程序仍跑不动。
- **GNOS**：只有一套 ABI——Linux x86-64。222 个名字、218 个唯一编号，Alpine 的 apk/bash/coreutils 直接跑。

把 native ABI 建在 `int 0x80` 上，等于每次系统调用都走一遍中断门；HBOS 的 Linux 兼容又另开 `syscall` 快车道。两套入口、两套寄存器约定、两套 errno 路径，维护成本是实打实的。但反过来说，HBOS 用 107 个 Linux 号把 glibc 2.43 的 loader 和 musl 的 pthread 喂到 QEMU 里跑通，这已经不是"形似神不似"，而是**在认真啃硬骨头**——只是还没啃完。GNOS 则是直接把 Linux 的契约当唯一真理，省掉了"自洽"的浪漫，也省掉了双 ABI 的地狱。

## 桌面：能用的 vs 没发货的

**HBOS：真发货，但发在 ring0。**

- `Makefile:258-290` 把 `gpu.c`、`graphics/gui_*`、`gui/service.c`、`gui/wm.c`、`gui/winsrv.c`、`gui/compositor.c`、`gui/effects.c` 和 13 个内置应用（calc/clock/settings/files/taskmgr/shortcuts/imgview/hexview/snake/uwc/diag/notes/code）全部编进**内核**，合计约 15k 行（含字体/图标/壁纸数据）。
- HIVE 子模块另有 11 个 `app/*.c` 应用和 Toolkit API 1.4；README 说 runtime 正在从"内核链接"迁移到独立用户态服务——**现在还没迁完**。
- 浏览器后端只有 192 行（`src/gui/browser_backend.c` 137 + `browser_layout.c` 55），HTML-lite + QuickJS 脚本；Chromium 明确没移植，`browser/chromium/` 只有构建参数和路线图。
- 好处是：开机就有桌面、有窗口、有应用、有壁纸，QEMU 截图不是 P 的。坏处是：**任何一个内置应用空指针，整个内核陪葬**。

**GNOS：底座是真的，桌面是"作者机器限定"。**

- DRM 移植 13,776 行，atomic/GEM/connector/vblank/dumb 都有，`/dev/dri/card0` + `renderD128` 是真的；fbcon/fbdev 也能用。
- 但 `Makefile:1685` 白纸黑字：`# ---- desktop stack (labwc/xfce) DISABLED for headless ISO ----`；`1481-1484` 把 `libgtk-3*`、`xfce4-*`、`startxfce4`、icons/themes 全删了；`src/usr/rc:140-144` 的 Xorg/twm/xterm 启动行全是注释。
- `src/usr/startxfce` 自己交代：没有 seatd、没有 logind、没有 system bus、没有 machine-id；`libinput cannot initialise the evdev stub yet`，于是 `WLR_LIBINPUT_NO_DEVICES=1`（`startxfce:44-50`）——**合成器可以起，输入设备是零**。`dbus-uuidgen` 还会段错误，只能写死 machine-id。
- README:132 说"已有 DRM 驱动与 Wayland 桌面栈（startxfce 拉起 labwc + Xfce）"。技术上没撒谎：脚本在、二进制在作者机器的 `build/desk/stage` 里。但仓库里**没有任何构建 desktop 的规则或脚本**（Makefile 只有 `DESK_STAGE` 定义和"手工构建"注释，唯一的实际构建记录是 `AGENTS.md` 里上一轮会话的日志），`build/` 又是 gitignore。换一台机器 clone 下来，`startxfce` 只会对着一堆被裁掉的 XFCE 依赖发呆。

一句话：HBOS 的桌面是"能用但住内核里"，GNOS 的桌面是"驱动很正规但还没交房"。

## 用户态生态：花房 vs 真货

| | HBOS | GNOS |
|---|---|---|
| 应用格式 | HAX（ELF + 元数据，ring3） | 标准 ELF |
| libc | 自研 libc + 自研 ldso | Alpine musl（默认 minirootfs） |
| 动态链接 | 自己的 ldso，glibc/musl 兼容测试 | musl 动态/静态，真实 `.so` |
| 编译器 | **内嵌 TinyCC**（`cc` 即编译器）+ QuickJS | binutils；clang 24 在 `build/musl-clang/` 手工工具链里 |
| 包管理 | HPT：本地/HTTP/HTTPS 仓库、SHA-256、依赖、回滚 | apk：Alpine 仓库，`apk update/fetch` 在 rc 里跑 |
| 登录 | 无 login/getty，单用户 root | BusyBox getty/login；`USERLAND=alpine` 变体带 util-linux-login + PAM |
| 实软件 | BusyBox applet、TCC、js.hax、自有应用 | Bash/coreutils/binutils/curl/nano/python3/fastfetch/OpenRC |

HBOS 是"你没生态，那就自己造一个"：格式、libc、链接器、编译器、包管理器一条龙，连浏览器都自己写。浪漫，闭环，但外面没人给 HAX 编译软件。GNOS 是"生态已经有了，把内核做对就行"：Alpine 的 apk 能拉包，bash/coreutils 是真货，Python 3.12 能跑。代价是 GNOS 的用户态不是自己写的——它赌的是"Linux ABI 比自己的造轮子能力更值钱"。

## 网络与加密：HBOS 塞进内核，GNOS 交给用户态

- **HBOS**：`src/net.c` 自研 IPv4/TCP/HTTP/HTTPS，网卡支持 e1000/RTL8139/PCnet/VirtIO-net 四种；`src/tls.c` 是内核内 TLS 1.3（1,215 行），crypto 3,025 行，ChaCha20-Poly1305/SHA-256/X25519/P-256/AES-GCM 全在内核。HPT 用 mbedtls 走 TLS 1.2。
- **GNOS**：只有 e1000，TCP/UDP/AF_UNIX 自研；TLS 在用户态（curl + mbedtls、apk + OpenSSL）。`rc` 里跑 `apk update/fetch` 证明网络通，但这两条命令**不参与 make test 的硬断言**。

HBOS 把 TLS 放进内核，省了用户态依赖，也把攻击面放进 ring0；GNOS 把 TLS 交给成熟库，安全账更好算，但网络驱动只有一块 e1000，换硬件就抓瞎。

## 存储与驱动：互相都有洞

| | HBOS | GNOS |
|---|---|---|
| SATA | AHCI + ATA PIO | ATA（**无 AHCI**） |
| NVMe | ❌ | ✅ |
| USB | xHCI：HID 键鼠 + MSC 存储 | xHCI：HID 键鼠 + MSC 存储 |
| 输入 | PS/2 + USB HID | PS/2 + USB HID（evdev event0/1） |
| GPU | framebuffer（`/dev/fb0`），无 DRM | DRM（QEMU bochs/stdvga VBE）+ fbcon/fbdev |
| 音频 | AC97 | HDA + AC97 + audiodev/ALSA 兼容层 |
| 网卡 | e1000/RTL8139/PCnet/VirtIO-net | e1000 |

两边的洞刚好互补：HBOS 有 AHCI、四种网卡，没 NVMe/HDA/DRM；GNOS 有 NVMe/HDA/DRM，没 AHCI，网卡只有 e1000。真机上谁也别笑谁。

## 安全与多用户：GNOS 像 OS，HBOS 像 demo

- **HBOS**：单用户 root，没有 login/getty，没有内核模块，没有 seccomp/namespace。HAX 应用在 ring3，内核有用户指针范围检查，但没有认证、没有 uid 隔离。README 自己也写"无权限隔离"。
- **GNOS**：BusyBox getty/login（Alpine 的 `/etc/passwd`/`shadow`），VFS 有 euid/mode 检查；seccomp 412 行、landlock 80 行、cgroup 1,419 行、pidfd 121 行、ptrace 451 行；内核模块可加载。但 README 同样承认"无完整安全边界、无多用户隔离"——教学级，别拿去当生产沙箱。

在"多用户"这一项上 GNOS 是完整度更高的那个；在"ring3 应用 + 内核检查"上 HBOS 也不是裸奔。但两家都离"安全操作系统"很远。

## 测试、CI 与可复现：一个靠自觉，一个靠 CI，但都有水分

**HBOS**

- `tests/` 34 个文件，Linux ABI 用例非常扎实：真实 musl/glibc 动态、`dlopen`、`clone3`、futex、epoll、inotify、signalfd、timerfd、socketpair/poll。`scripts/test_linux_compat_smoke.sh` 会启动 QEMU 逐项 grep PASS。
- 但主仓库**没有 CI**，这些脚本要手工跑；HIVE/HPT 各自有 `test.yml`。子模块（HIVE/HPT/mbedtls/lwip/limine-bin）不初始化就构建不了，`limine-bin` 还直接决定 UEFI 镜像。

**GNOS**

- `.github/workflows/ci.yml`：clang-format 18.1.7、头文件名唯一性、gcc-13 内核构建、QEMU 启动。`make test` 不只是 grep PASS/FAIL，还硬断言"真 Alpine init 是 pid 1、读了 inittab、fork 了第一个子进程"以及"OpenRC 三个 runlevel + agetty 至少一次"（`Makefile:1887-1923`），并且用 `-smp 4` 启动。
- 水分在别处：PASS 大多来自内核功能自测（crypto/DRM/VMM COW/SLAB/HDA/AC97 等 31 处 `report()`，测试程序在 `src/usr`），apk/clang 这些用户态测试只看日志不判失败；README 说 `/etc/rc` 跑 `bashtest.sh`/`coreutilstest.sh`，**仓库里根本没有这两个文件**；`/usr/bin/clang` 依赖 `build/musl-clang/` 的手工工具链，没有 tracked 的获取/构建规则。
- 更实在的一条：`Makefile:1939` 自己写"musl was fetched and built by hand and there is no rule to get it back"——`USERLAND=src` 那套 musl/BusyBox/Bash/coreutils 树是手工产物，`make clean` 都得绕开它们。默认的 Alpine 路径能重建，但桌面栈不能。

## HBOS 的亮点与暴击点

**亮点**

- 315 个提交、5 个月，节奏是真的快；桌面/应用/包管理器/编译器全在一个生态里，成品感强。
- Linux ABI 兼容不是嘴炮：107 个 Linux 号 + 34 个测试文件，glibc 2.43 loader、musl pthread、futex、epoll、inotify 都有 QEMU 用例。
- xHCI（HID + MSC）、AHCI、四种网卡、内核 TLS 1.3—— hobby OS 里少见的硬骨头。
- 文档工程化一流：每个子系统一份 `*.agent.md`、HAX API 文档、`LINUX_KDE_COMPAT.md`、Chromium 基线，连"哪里不行"都写清楚。
- `make ver`/`make nogui`/多构建形态，工程洁癖。

**暴击点**

- SMP 是"代码已写，核没起来"，README 却把它当卖点；这是最需要修的一块。
- 桌面合成器 + 13 个应用 + 浏览器后端全在内核态，任何一个数组越界都是全机陪葬。
- native ABI 用 `int 0x80`，Linux 兼容另开 `syscall`，双入口的复杂度会长期咬人。
- 64 任务 / 128 fd 的上限，配上"桌面 + 浏览器 + 编译器"的野心，迟早撞墙。
- 浏览器 192 行，Chromium 路线图离"能跑"还有几光年；README 倒是诚实，没吹。
- 版本号 pre18，README 还写 pre6；文档漂移说明发布流程还没闭环。

## GNOS 的亮点与暴击点

**亮点**

- 真 Alpine 用户态：apk、bash、coreutils、Python、curl、OpenRC、gettys，不是"兼容层演示"。
- EEVDF + 16 核 SMP + KASLR + 内核模块 + seccomp/landlock/cgroup/pidfd/ptrace，内核工程深度在线。
- CI 是硬门槛：格式、头文件、构建、启动、真 init/OpenRC/agetty 断言，`-smp 4` 跑。
- DRM 移植 13.8k 行，fbcon/fbdev 之外真有 KMS/GEM/atomic 底座。
- 默认镜像 256 MiB ext2 + 可安装到磁盘，路线是"能当系统用"。

**暴击点**

- **桌面没发货**：Makefile 禁用、GTK/XFCE 裁剪、Xorg 注释、libinput 零设备、`build/desk` 不在仓库。README 的"Wayland 桌面栈"目前是作者机器上的演示，不是用户能复现的功能。
- README 的测试叙事过时：`bashtest.sh`/`coreutilstest.sh` 不存在；clang 测试依赖手工工具链；apk 网络测试不参与断言。
- 用户态测试的 PASS 主要来自内核 self-test，`make test` 对"真软件能不能用"的覆盖没有听起来那么强。
- 没有 AHCI、网卡只有 e1000；换一台真机，存储/网络两连可能直接卡住。
- 64 进程 / 64 fd 的上限，比 HBOS 还紧；桌面 + 浏览器 + 包管理一起上会很难看。
- `USERLAND=src` 的 musl/BusyBox/Bash 树是手工产物，Makefile 自己承认没有重建规则——"一切可复现"要打个折。

## 总结陈词

- **HBOS** 是"把整辆车造出来给你开"：从引导、内核、桌面、应用、编译器到包管理器全自己来，今天就能开机进图形界面，Linux 兼容也在认真推进。代价是单核、单用户、应用住内核、上限很小，生态是自家的花房。
- **GNOS** 是"把 Linux 的底盘和发动机调好"：真用户态、真 SMP、真 CI、真 DRM，能跑 Alpine 软件。代价是车身（桌面）还在仓库外面，输入设备没通，文档比实现跑得快。
- 一句话：**HBOS 赢在"交付"，GNOS 赢在"地基"。** 要桌面演示选 HBOS，要 Linux 软件/内核深水区选 GNOS；两家都还没到"成熟"，但都值得被认真对待。
