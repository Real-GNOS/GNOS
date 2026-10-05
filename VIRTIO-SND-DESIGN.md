# GNOS virtio-snd 声卡驱动设计（v1）

> 状态：**设计稿，未实现**　生成日期：2026-10-05
> 范围：`-device virtio-sound-pci`（VirtIO 1.2 sound device，PCI id `1af4:1059`）
> 依据：`CONTRIBUTING.md.en §5`「Big changes: open an issue stating the problem
> and the chosen approach first」—— 本文件即该 issue 的底稿，动手前先贴。
> 版权：本文件随仓库以 GPL-2.0 发布。文中标注为 Uinxed-Kernel 的代码**只作行为
> 对照，不复制**（见 §3 许可证一节）。

## 目录

1. [目标与非目标](#1-目标与非目标)
2. [现状盘点](#2-现状盘点)
3. [为什么不能"反向移植"](#3-为什么不能反向移植)
4. [目标架构](#4-目标架构)
5. [详细设计](#5-详细设计)
   - [L1 virtio 传输层](#l1-virtio-传输层srcdriversvirtio)
   - [L2 virtio-snd 协议驱动](#l2-virtio-snd-协议驱动srcsoundvirtio_sndc)
   - [L3 声卡抽象](#l3-声卡抽象srcsoundcardh)
   - [L4 capture 补完](#l4-capture-补完)
6. [分阶段实施与验收](#6-分阶段实施与验收)
7. [验证方法](#7-验证方法)
8. [风险与未决问题](#8-风险与未决问题)
9. [参考](#9-参考)

---

## 1. 目标与非目标

**目标**：让 GNOS 能在 QEMU `-device virtio-sound-pci` 下播放**和录音** PCM，
走已有的 ALSA UAPI（`/dev/snd/pcmC0D0p`、`pcmC0D0c`），使真实 ALSA 用户态程序
可直接使用。

**非目标**：

- 不做 jack / chmap / control 事件（virtio-snd 的 jack、chmap、control 三个
  子设备本期跳过，只做 PCM 流）。
- 不做 packed vring，只支持 split vring（不协商 `VIRTIO_F_RING_PACKED`，位 34）。
- 不做 OSS `/dev/dsp` 在 virtio-snd 上的兼容（`/dev/dsp` 仍是 AC97 专属）。
- 不引入可加载模块机制（GNOS 无模块，见 `CONTRIBUTING.md.en §3`）。

---

## 2. 现状盘点

### 2.1 GNOS 已有的底子

| 能力 | 位置 | 备注 |
|---|---|---|
| PCI 枚举 | `src/drivers/pci/pci.c`（426 行） | `pci_find` / `pci_find_class` / `pci_map_bar` / `pci_enable` / `pci_bar_io` / `pci_has_cap`（`pci.h:40-87`） |
| **MSI / MSI-X** | `pci.h:83-84` | `pci_enable_msix(d, handler, &vec)`，`handler` 类型 `pci_msi_handler_t` |
| IRQ 注册 | `src/arch/x86/kernel/idt.h:42` | `irq_install(irq, fn, name)` |
| **物理连续分配** | `src/mm/pmm.h:34` | `pmm_alloc_contiguous(n)` + `pmm_virt(phys)`（HHDM 直映射）—— virtqueue 与音频 DMA 正需要 |
| ALSA UAPI | `src/include/sound/asound.h` | 真 Linux ALSA 头，632 个 `SNDRV_`/`snd_` 符号 |
| ALSA 内核层 | `src/sound/alsa.c`（513 行） | 已实现 HW_REFINE / HW_PARAMS / SW_PARAMS / PREPARE / START / STOP |
| 声卡注册 | `src/drivers/base/subsys.h:63` | `subsys_register(name, dev, cls, major, minor)` + `subsys_set_state()` |
| 内核线程 | `src/kernel/proc.h:476` | `kthread_create(name, entry, arg)`；**无通用 workqueue** |
| 延迟处理的既有范式 | `src/drivers/net/ethernet/intel/e1000.c:240` | `e1000_irq()` 直接在 IRQ 上下文干活，`e1000.c:423` 用 `irq_install` 挂上 |

### 2.2 GNOS 缺的

| 缺口 | 证据 | 影响 |
|---|---|---|
| **virtio 传输层 0 行** | 全仓 `grep -i "virtio\|virtqueue"` 只命中 `src/vendor/acpica/**` | **最大的一块**（L1） |
| **内存屏障 helper 0 个** | `grep -E "\b(wmb\|rmb\|mb\|smp_mb)\s*\(" src/include` 为空；`src/include/asm/` 只有 `byteorder.h`、`io.h` | vring 的 avail/used/desc 同步**无法正确表达**（见 §8.1） |
| 通用 workqueue | 只有 `kthread_create` | 泵送策略要选型（见 §8.4） |
| 可插拔声卡抽象 | `alsa.c:363` 直接调 `audio_write()` | AC97 写死（见 L3） |
| capture | `alsa.c:414` `pcm_read()` 返回 `0 /* capture not implemented */`；`alsa.c:397` `READI_FRAMES` → `-E_NOTTY` | 录音通路不存在（见 L4） |

### 2.3 音频通路现状（关键）

```
userspace ──/dev/snd/pcmC0D0p──▶ src/sound/alsa.c
                                     │ 直接调用 ↓（绑死 AC97）
                                  src/sound/audio.c   AC97 引擎
                                     │ BAR1 bus master / BDL / LVI / PICB
                                     ▼
                                  ICH AC97
```

- `audio.c` 导出 `audio_write(const int16_t*, frames)`、`audio_begin/stop`、
  `audio_reset_ring`、`audio_frames_total/in_flight_frames/free_frames`
  （声明见 `src/sound/audio.h`）—— 全是 **AC97 专属**语义。
- **`hda.c` 是断的**：`src/init/kernel.c:314` 调 `hda_init()`
  （`src/sound/hda.c:305`，`pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_HDA)`
  probe → 复位 → codec 发现 → pin 配置 → `hda_selftest`），
  但 `src/sound/alsa.c` 和 `src/sound/audio.c` 里 **一个 `hda_` 符号都不引用**——
  它没有接进 PCM 通路，播不了音。
- 设备节点已注册（`alsa_vfs_register()`，`alsa.c:493`）：

  | 节点 | major,minor | 行 |
  |---|---|---|
  | `snd/controlC0` | 116,0 | 503 |
  | `snd/pcmC0D0p` | 116,16 | 504 |
  | `snd/pcmC0D0c` | 116,24 | 505 |
  | `snd/timer` | 116,33 | 506 |

  **capture 节点已存在**，只是后面是空壳。
- 但 `pcm_stream_of()`（`alsa.c:408`）**无条件返回 `&g_play`**——全进程只有一个
  流对象，且只服务 playback。
- 启动序列：`kernel.c:305-319` 给 ac97、hda 各 `subsys_register(..., SUBSYS_CLASS_SOUND, ...)`
  并置 LIVE/FAILED；`kernel.c:372-373` 调 `audio_vfs_register()` + `alsa_vfs_register()`。

### 2.4 对照：Uinxed-Kernel 侧的实现规模

| 组成 | 文件 | 行数 |
|---|---|---|
| virtio-snd 协议驱动 | `drivers/audio/virtio/virtio_snd.c` + `include/.../virtio_snd.h` | 1255 + 139 = **1394** |
| virtio PCI 传输层 | `drivers/bus/virtpci.c` + `include/drivers/bus/virtpci.h` | 609 + 228 = **837** |
| 可插拔音频核心（依赖） | `drivers/audio/core/audio.c` + `include/.../audio.h` | 1031 + 334 = **1365** |

---

## 3. 为什么不能"反向移植"

### 3.1 许可证不兼容（硬阻碍）

| 项目 | 许可 |
|---|---|
| Uinxed-Kernel | **Apache License 2.0**（`LICENSE` 文件首行确认） |
| GNOS | **GPL-2.0**（`CONTRIBUTING.md.en:69`：「By submitting code you agree it is published under the project's GPL-2.0 license」） |

Apache-2.0 只**单向兼容到 GPLv3**，与 GPLv2 不兼容。把 Apache-2.0 代码并入
GPL-2.0-only 的主树会造成许可证冲突。另外 `virtio_snd.c` 文件头写的是
`Copyright (C) 2020 ViudiraTech`，版权也不完全在个人手里。

**但这不是死路**，两条出路：

1. **按规范重写，不抄代码。** virtio-snd 协议本身是公开的 VirtIO 1.2 规范
   （§5.14），`CONTRIBUTING.md.en §2` 也明写
   「No copy-paste from third parties into the main tree」。照规范实现，
   代码是原创的，无许可问题。
2. 行为基线可参考 Uinxed PR #87 的日志与现象（见 §7），那是**观察结果**，
   不是代码。

> 结论：本文档剩下的部分都是「按规范重写到 GNOS 自己的 API 上」，
> 而非 cherry-pick。

### 3.2 架构不匹配

Uinxed 的音频核心是**可插拔的**：`audio_card_ops_t` + `audio_register_card()`，
`audio_card_t` 持有 `pcm_files` 链表和 `pcm_lock`，HDA / SB16 / virtio-snd 各注册
一张卡，由驱动自己去 pump ring。

GNOS 则是**固定引擎**：`alsa.c` 直接调 `audio_*`，没有「卡」的概念。
搬过来没有落脚点，必须先有 L3。

### 3.3 传输层不存在

GNOS 全仓 0 行 virtio。现代 virtio 的 PCI transport（common cfg / notify /
ISR / device cfg 四个能力结构）、split vring、feature 协商、used ring 收包——
全部要从零写（L1）。

---

## 4. 目标架构

```
                userspace（ALSA 应用、aplay、BusyBox）
                          │  /dev/snd/pcmC0D0p|pcmC0D0c、controlC0
                          ▼
        ┌──────────────────────────────────────────┐
        │  src/sound/alsa.c     ALSA UAPI 层（已有）│
        └──────────────────────────────────────────┘
                          │  snd_card_ops_t  ←── L3 新增的唯一抽象
          ┌───────────────┼─────────────────┐
          ▼               ▼                 ▼
   ┌────────────┐  ┌────────────┐  ┌────────────────────┐
   │ AC97 card  │  │  HDA card  │  │  virtio-snd card   │
   │ audio.c    │  │ hda.c      │  │  L2 新增            │
   │ 包装成 ops │  │ 接上（修断）│  │  src/sound/         │
   └────────────┘  └────────────┘  │  virtio_snd.c       │
                                   └─────────┬──────────┘
                                             ▼
                        ┌──────────────────────────────────┐
                        │ L1 src/drivers/virtio/           │
                        │  virtio_pci.c  PCI transport      │
                        │  virtqueue.c   split vring        │
                        └──────────────────────────────────┘
                                             ▼
                        ┌──────────────────────────────────┐
                        │ src/drivers/pci/（已有）+ MSI-X   │
                        │ src/mm/pmm.h（连续物理内存）       │
                        │ src/include/asm/barrier.h ← P0 新增│
                        └──────────────────────────────────┘
```

---

## 5. 详细设计

### L1：virtio 传输层（`src/drivers/virtio/`）

**新文件**

```
src/drivers/virtio/virtio_pci.h    能力结构、设备状态机、队列结构
src/drivers/virtio/virtio_pci.c    枚举 / reset / feature 协商 / 队列建立
src/drivers/virtio/virtqueue.c     split vring：desc/avail/used 管理、链提交、收包
```

**API（形状取自 Uinxed `virtpci.h`，语义落在 GNOS 的 PCI 上）**

```c
struct vp_device {
    const pci_dev_t *pci;
    volatile uint8_t *common;   /* common cfg  */
    volatile uint8_t *notify;   /* notify      */
    volatile uint8_t *isr;      /* ISR status  */
    volatile uint8_t *cfg;      /* device cfg  */
    uint64_t         features;  /* 协商结果     */
    uint8_t          status;
};

struct vp_virtq_seg { void *buf; uint32_t len; bool device_writes; };

int   vp_find(struct vp_device *d, uint16_t vendor, uint16_t device);
int   vp_setup(struct vp_device *d);          /* ACKNOWLEDGE→DRIVER→FEATURES_OK 链 */
int   vp_negotiate(struct vp_device *d, uint64_t want, uint64_t *got);
int   vp_setup_vq(struct vp_device *d, int idx, int num, struct vp_virtqueue *q);
void  vp_del_vq(struct vp_virtqueue *q);

/* 一次性校验全部 seg 再消费 desc —— 半提交是这类驱动最常见的 bug 源 */
int   vp_add_chain(struct vp_virtqueue *q, void *cookie,
                   const struct vp_virtq_seg *segs, int count);
void *vp_get_buf(struct vp_virtqueue *q, uint32_t *len);
void  vp_kick(struct vp_virtqueue *q);
bool  vp_irq_pending(struct vp_device *d);
```

**与 GNOS 现有 API 的映射**

| 需要 | GNOS 已有 |
|---|---|
| 找 `1af4:1059` | `pci_find(0x1AF4, 0x1059)`（`pci.h:40`） |
| 映射 common/notify/isr/device cfg BAR | `pci_map_bar(d, idx)`（`pci.h:56`，uncacheable） |
| bus master | `pci_enable(d)`（`pci.h:63`，注释已强调「没有它设备取不到自己的描述符」） |
| 能力位 | `pci_has_cap(d, PCI_CAP_ID_MSIX)`（`pci.h:76,87`） |
| 中断 | `pci_enable_msix(d, handler, &vec)`（`pci.h:84`） |
| vring 物理连续内存 | `pmm_alloc_contiguous(n)` + `pmm_virt(phys)`（`pmm.h:34`） |

**必须新增**

1. `src/include/asm/barrier.h`：`mb()` / `wmb()` / `rmb()`。x86 上
   `mfence` / `sfence` / `lfence` 即可（编译屏障用 `asm volatile("" ::: "memory")`）。
   **这是正确性前提，不是可选项**——见 §8.1。放 P0。
2. 分配/映射失败的回滚路径（`vp_setup_vq` 半途失败要能 `vp_del_vq` 干净退出）。

**vring 布局**

- 一次 `pmm_alloc_contiguous` 拿够三段：
  desc table `16 × num`、avail `6 + 2 × num`、used `6 + 8 × num`，
  整体页对齐；`num ≤ 1024`（规范上限）。
- `avail.idx` 读取前 `rmb()`，`used.idx` 读取前 `rmb()`，
  desc 写入完成后、更新 `avail.idx` 前 `wmb()`。

---

### L2：virtio-snd 协议驱动（`src/sound/virtio_snd.c`）

**新文件**：`src/sound/virtio_snd.c`、`src/sound/virtio_snd.h`

**四条队列**（VirtIO 1.2 §5.14.6.2）

| idx | 名 | 用途 |
|---|---|---|
| 0 | control | 同步请求/响应 |
| 1 | event | 设备 → 驱动通知 |
| 2 | TX | playback 数据（方向 `OUTPUT = 0`） |
| 3 | RX | capture 数据（方向 `INPUT = 1`） |

**消息集**（从 VirtIO 1.2 §5.14.6.5/6/7）

| 请求码 | 含义 |
|---|---|
| `0x0100` `VIRTIO_SND_R_PCM_INFO` | 查询流的能力（formats / rates / channels） |
| `0x0101` `PCM_SET_PARAMS` | buffer_bytes、period_bytes、channels、format、rate |
| `0x0102` `PCM_PREPARE` | 进入 PREPARED |
| `0x0103` `PCM_RELEASE` | 释放流 |
| `0x0104` `PCM_START` | 开始 |
| `0x0105` `PCM_STOP` | 停止 |

响应状态：`0x8000 OK`、`0x8001 BAD_MSG`、`0x8002 NOT_SUPP`、`0x8003 IO_ERR`。
格式 bit index：`U8 = 4`、`S16 = 5`；速率 bit index：`8000 = 1` … `192000 = 12`。

**关键设计决策：control 请求在进程上下文同步等待**

GNOS 没有通用 workqueue（只有 `kthread_create`），既有范式是 `e1000_irq()`
直接在 IRQ 干活。但 control 是**请求/响应**，在 IRQ 里等响应会造成
「等自己的中断」死锁。因此：

- control 请求在 **ioctl / 系统调用上下文**提交，然后**自旋等 used ring，
  必须带超时**（建议 50 ms），超时返回负 errno 并打 `dbg_puts`。
- **绝不能无限等。** Uinxed PR #87 修的核心 bug 就是「`start()` 静默失败 →
  状态被标成 RUNNING → 没人喂 ring → `read()` 无超时无信号出口地永久睡眠」。
  本设计里三处必须有超时：control 等待、playback 反压、capture 首读。

**数据通路**

- `snd_card_ops::write`：把 S16 交错帧切成 `period_bytes` 大小，
  填 TX desc chain（`struct virtio_snd_pcm_xfer` 头 + 音频负载），kick。
- MSI-X handler（`irq_install`）：`vp_get_buf()` 收 TX 完成 → 补交下一批 →
  唤醒 `pcm_poll` 的 `POLLOUT`。
- capture 对称：IRQ 收 RX → 拷进 ALSA ring → 唤醒 `POLLIN` 与阻塞读。

**period 缓冲池**：每方向用 `pmm_alloc_contiguous` 预分配 `N × period_bytes`
常驻池，**IRQ 里不分配内存**（GNOS 的 pmm 在 IRQ 上下文是否可重入未验证，
见 §8.6）。

**设备注册**

```c
int slot = subsys_register("virtio-snd", NULL, SUBSYS_CLASS_SOUND, 116, 16);
if (virtio_snd_init()) subsys_set_state(slot, SUBSYS_STATE_LIVE);
else                   subsys_set_state(slot, SUBSYS_STATE_FAILED);
```

从 `src/init/kernel.c` 的音频段（`kernel.c:305-319`）之后调用，
需在 `pci_init()` 之后。

---

### L3：声卡抽象（`src/sound/card.h`）

**新文件**：`src/sound/card.h`（接口）+ `src/sound/card.c`（注册表，可选）

```c
struct snd_card;

struct snd_card_ops {
    const char *name;
    int (*hw_params)(struct snd_card *, const struct snd_pcm_hw_params *);
    int (*prepare)(struct snd_card *);
    int (*start)(struct snd_card *);
    int (*stop)(struct snd_card *);
    int (*drain)(struct snd_card *);
    /* 喂/取交错 S16 帧，返回实际接受/交付的帧数 */
    int (*write)(struct snd_card *, const int16_t *src, uint32_t frames);
    int (*read)(struct snd_card *, int16_t *dst, uint32_t frames);
    /* 供 ALSA 层计算 avail/delay/hw_ptr */
    int (*avail)(struct snd_card *);
    int (*hw_ptr)(struct snd_card *);
};

int            snd_register_card(struct snd_card *c, const struct snd_card_ops *ops);
struct snd_card *snd_card(uint32_t index);
```

**改造范围（刻意压到最小）**

1. `audio.c` 的 `audio_write / audio_begin / audio_stop / audio_reset_ring /
   audio_frames_total / audio_in_flight_frames / audio_free_frames`
   **原封不动保留**，只在外面包一层 `ac97_ops` 实现 `snd_card_ops`。
   纯包装，零逻辑改动 → 风险最低，`make test` 可立刻验证没回归。
2. `alsa.c` 里所有 `audio_*` 调用点（`alsa.c:210,315,322,331,336,350,352,363,368,441`）
   换成 `card->ops->*`。
3. `pcm_stream_of()`（`alsa.c:408`）按 node 分流：minor 16 → playback，
   minor 24 → capture。
4. `hda.c` 实现 `snd_card_ops` → **顺手把现在断着的 HDA 接进 PCM 通路**。
5. `virtio_snd.c` 实现同一套 ops → 第三张卡。

**注册与节点编号**：卡 0 占 `pcmC0D0p` / `pcmC0D0c`（116,16 / 116,24），
后续卡按 `pcmC{card}D0` 递增 minor；`subsys_register` 的 name 用
`ac97` / `hda` / `virtio-snd` 区分（现有 `kernel.c:305`、`:313` 已在用）。

---

### L4：capture 补完

节点 `snd/pcmC0D0c`（116,24）**已经注册**（`alsa.c:505`），缺的是实现：

| 项 | 现状 | 要做 |
|---|---|---|
| 流对象 | 只有 `g_play`，`pcm_stream_of()` 恒返它（`alsa.c:408`） | 增 `g_cap`，按 minor 分流 |
| `pcm_read()` | 返回 `0`（`alsa.c:414`） | 走 `card->ops->read`，阻塞语义 + 超时 |
| `READI_FRAMES` | `-E_NOTTY`（`alsa.c:397`） | 实现 |
| `pcm_poll` | 只有 `POLLOUT` 分支（`alsa.c:441`） | 加 `POLLIN` |
| mmap status | `g_status_k` 单份（`alsa.c:499-501`） | 每流一份（ALSA 语义 status 是 per-stream） |

---

## 6. 分阶段实施与验收

| 阶段 | 交付 | 验收标准 |
|---|---|---|
| **P0** | `src/include/asm/barrier.h`（`mb/wmb/rmb`）+ 按 §5 开一个 issue | `make check` 绿；屏障有单测 |
| **P1** | L1 virtio 传输层 + **virtio-rng 冒烟驱动** | QEMU `-device virtio-rng-pci` 能读到真随机数 → **DMA、MSI-X、kick、used 收包全链路打通**，不依赖音频栈 |
| **P2** | L3 card 抽象：AC97 走 ops、**HDA 接上** | `make check` + `make test` 全绿；`make headless` 的 dbg.log 里 HDA 卡进 LIVE 且能播 |
| **P3** | L2 virtio-snd **playback** | dbg.log 出现 `virtio-snd` 注册行、`1af4:1059`、4 条队列；写 `pcmC0D0p` 有声，`hw_ptr` 随 `appl_ptr` 推进 |
| **P4** | L4 capture | 读 `pcmC0D0c` 返回帧；首读行为与 §7 基线一致 |

**为什么 P1 用 virtio-rng**：它是 VirtIO 里最小的请求/响应设备
（device id `0x1044`，单队列，无方向语义），约 50 行就能在**不动音频栈的
前提下**独立验证整个 L1。把传输层的 bug 和协议驱动的 bug 分开，省掉大量
二分定位时间。

每个阶段单独一个 commit，`make check` 必须全程保持绿色
（`Makefile:1849` `check-format` 只扫 `src/**/*.c|*.h` 且排除 `vendor/`，
`Makefile:897` `check-hdrs` 只查 `.h` 文件名唯一——**本文档是根目录 `.md`，
不触发这两个门禁**）。

---

## 7. 验证方法

**启动命令**（照抄 `Makefile:615-616` 的设备行，把 virtio-sound 加进去）：

```bash
qemu-system-x86_64 -machine q35 -bios OVMF \
  -device virtio-sound-pci,audiodev=snd0 -audiodev none,id=snd0 \
  -device isa-debugcon,chardev=dbg -chardev file,id=dbg,path=build/dbg.log \
  ...
```

（QEMU 必须带 sound 支持；`make test` 现有的 `-device AC97,audiodev=snd0`
说明宿主 QEMU 已具备该 audiodev 机制。）

**行为基线**（取自 Uinxed-Kernel PR #87 的实测日志，**仅作现象对照**）：

1. 枚举到 `1af4:1059`，feature 协商成功；
2. 注册卡片并打印 2 条流（playback id 0 / capture id 1）、MSI-X 中断就绪；
3. playback：一次 `write()` 后 TX 缓冲被设备取走并回收，`hw_ptr` 追上
   `appl_ptr`；
4. capture：**首次 `read()` 返回 `-EAGAIN`**，后续 `read()` 返回 4096 字节；
5. release 后重启两条流正常，QEMU stderr 全程无错。

**回归**：`make check`、`make test`、`make headless`（AC97 与 HDA 两条既有
路径必须保持绿）。

---

## 8. 风险与未决问题

| # | 风险 | 严重度 | 处置 |
|---|---|---|---|
| 1 | **GNOS 无内存屏障 helper**。vring 是设备与 CPU 共享的内存，`avail.idx`/`used.idx`/desc 若无屏障，编译器或乱序执行会让设备看到半成品描述符 → 随机数据损坏。 | **高** | P0 先补 `src/include/asm/barrier.h`，并在 §L1 的每个发布/读取点标注用哪种屏障 |
| 2 | `pci_find` **只扫 bus 0**（`pci.h` 注释明写） | 中 | q35 单 hierarchy 下够用；多 hierarchy 需扩 `pci_init()` |
| 3 | `PCI_MAX_DEVICES 16`（`pci.h:17`） | 中 | 设备多时会漏；virtio-snd + AC97 + HDA + e1000 + NVMe + xHCI 已接近上限，建议一并提到 32 |
| 4 | **IRQ 里能做多重活**。`e1000_irq`（`e1000.c:240`）的范式是直接干，但 virtio-snd 的 TX 补交若在 IRQ 里背靠背做，可能饿死其他中断 | 中 | IRQ 里只做「收完成 + 限流补交」（每中断最多补 N 个 period），超出的留到下一次；不阻塞 |
| 5 | `/dev/dsp`（14,3）是 AC97 专属 OSS 层，多卡时归属未定 | 低 | 本期保持不动（见 §1 非目标） |
| 6 | `pmm_alloc` / `pmm_free` 在 IRQ 上下文是否安全未验证 | 中 | 本期所有分配放进程上下文，IRQ 内只用常驻池（见 L2） |
| 7 | `pci_map_bar` 返回 uncacheable 映射，而 `pmm_virt` 给的是普通直映射；**vring 用哪一种** | 中 | vring 用 `pmm_virt`（普通直映射，x86 上 WB 对设备 DMA 可接受）；设备 cfg BAR 用 `pci_map_bar`。需实测确认 |
| 8 | QEMU 的 virtio-sound 是否需要 `VIRTIO_F_VERSION_1` 之外的 feature 位 | 低 | 协商时只请求必须的（`VERSION_1` 位 32、`RING_EVENT_IDX` 位 29 视情况），拒绝未知位 |
| 9 | `CONTRIBUTING.md.en §3` 说驱动放 `src/kernel/driver/*.c`，**但该目录不存在**，现存驱动实际在 `src/drivers/{pci,net,...}` 和 `src/sound/` | 低 | 文档陈旧；以现状为准，可顺手提 PR 修正 |

**待定的两个选型，动手前需拍板**：

- **A. control 请求的等待方式**：本文建议「进程上下文 + 自旋 + 50 ms 超时」。
  备选是 `kthread_create` 一个泵线程做异步等待——更优雅但要引入等待队列
  和唤醒机制，成本高。**倾向前者**（简单、可测、无新增并发原语）。
- **B. 是否先独立做 P2（card 抽象）再做 P1**：P2 风险低、能立刻修复
  `hda.c` 断连、且为 L2 铺路；P1 代码量大但完全独立。
  **倾向按 P0→P1→P2→P3→P4 顺序**，因为 P1 的 virtio-rng 验收不依赖 P2。

---

## 9. 参考

- VirtIO 1.2 specification — §5.14 sound device、§4.2 split vring、
  §4.2.3 device status、§4.2.2.2 feature negotiation
- QEMU `virtio-sound-pci` 实现与 `-audiodev` 机制
- GNOS `CONTRIBUTING.md.en` §2（代码契约）、§3（写驱动）、§5（分支与提交、
  **大改动先开 issue**）、§6（许可证）
- GNOS `src/include/sound/asound.h`（ALSA UAPI）、`src/sound/alsa.c`（内核侧实现）
- Uinxed-Kernel PR #87（Apache-2.0）—— **仅作行为与日志基线对照，不复制代码**；
  参考其修过的 bug（`start()` 返回值被丢弃 → 状态被标成 RUNNING →
  无超时永久睡眠），作为本实现的反面清单
