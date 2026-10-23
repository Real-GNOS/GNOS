# DRM 三方对决：GNOS vs Uinxed(移植源) vs Linux

GNOS 的 DRM 现在是**一套树**，按 Uinxed 的模块划分组织，不再有并存的
legacy/ported 两份实现。

## 参战方

| | GNOS `src/drivers/gpu/drm/` | Uinxed-Kernel `drivers/gpu/drm/` | Linux 主线 DRM |
|---|---|---|---|
| 来源 | 自研，目录与接口对齐 Uinxed | ViudiraTech (Apache 2.0) | 原版 |
| 规模 | 约 17.0k 行（37 个 .c / 13.3k 行 + 21 个头文件 / 3.7k 行） | 约 19.8k 行（含头文件） | 核心 84,584 / 全树含驱动 **419,876** |
| 硬件 | bochs VBE_DISPI + QEMU stdvga | 同样只有 VBE/fb 后端 | 数百种真实 GPU |
| 能力 | atomic KMS、GEM、dumb buffer、vblank、PRIME/dma-buf、rbtree、idr、modeset lock | 同一套迷你 DRM core API | 全部 |
| 目录 | `core/ gem/ kms/ lib/ simpledrm/ svga/` | `core/ gem/ kms/ lib/ ...` | 按驱动与子系统分层 |

## 现在的 GNOS 是什么样

**一套实现，按职责分目录**

- `core/`：设备生命周期、`/dev/dri` 节点、per-open 状态、ioctl 分发、
  vblank、打印，以及 GNOS VFS 桥（`drm_devtmpfs.c` + `drm_vfs.c`）。
- `gem/`：GEM 对象、handle/flink、mmap offset 分配器、dumb buffer、
  framebuffer，以及 `drm_prime.c` 的真实 dma-buf 导出/导入。
- `kms/`：CRTC/plane/encoder/connector/property/mode 对象与 atomic 提交。
- `lib/`：idr、hashtab、区间分配、锁、格式化、freestanding C 库垫片和
  两个通用容器（intrusive list、rbtree）。
- `simpledrm/`：软件 KMS 驱动、刷新/拷贝路径、Bochs VBE 后端。
- `svga/`：QEMU stdvga 硬件探测。

原来平铺目录里的 `drm_dumb.c`、`drm_event.c`、`drm_kms.c` 和
`drm_internal.h` 是未接入构建的旧实现，已经删除；它们的功能分别由
`gem/drm_gem_dumb.c`、`core/drm_vblank.c`、`kms/` 和
`src/include/drivers/gpu/drm/` 下的公共头承担。

**PRIME 从"假 fd 表"换成了真 dma-buf**

- 导出返回进程 fd 表里的真实描述符：匿名 VFS 节点 + `anon_bind()`，
  可以 `dup()`、可以通过 Unix socket 传递。
- dma-buf 对导出 GEM 持强引用；GEM 上的 `export_dma_buf` 是弱回指针，
  最后一个描述符关闭时清空。
- 同设备导入直接返回原 GEM 对象；跨设备导入创建借用 backing 的包装
  对象，包装对象不经过驱动的释放钩子。
- 描述符的 `.release` 负责放掉 dma-buf 引用；GNOS 没有 VMA get/put
  钩子，所以映射本身不单独计数，使用期间必须保持 fd 打开——这正是
  dma-buf ABI 的要求。

**仍然诚实的地方**

- atomic 路径配的还是 VBE 这个不支持 page flip 的硬件，所谓"法拉利
  发动机装在手扶拖拉机上"这一点没变；`simpledrm` 用刷新线程把客户端
  缓冲拷贝到扫描输出，而不是等硬件 flip 完成。
- 没有真实 GPU 驱动，也没有 GPU 内存管理；`drm_mm` 只是给
  framebuffer/offset 分配用的通用区间分配器。
- 热插拔、EDID、DP/HDMI 这些真实显示链路都不存在，设备树是启动时
  静态注册的。

## 对照组

**Uinxed-Kernel（移植源，19.8k 行）**

- 目录结构、GEM/PRIME 接口、atomic 对象模型是 GNOS 对齐的基准。
- dma-buf 的设计（强引用 + 弱回指针、同设备直接返回原对象、跨设备
  包装）就是 GNOS 现在采用的方案。
- 它同样只有 VBE/fb 后端，所以两边在"DRM core 完整、硬件后端贫弱"
  这一点上同病相怜。

**LuminaOS（0 行 DRM）**

- 直接 VGA 裸画窗口，272 行 desktop.c 解决战斗。
- 没有 /dev/dri，没有合成器协议，但用户看到的效果和 GNOS 在 QEMU 里
  的软件渲染殊途同归。
- 目录里躺着 5.5G 的 linux-src 参考——拥有最强 DRM 却一行不用。

**Linux 主线（42 万行）**

- GNOS 的 17k ≈ Linux 全树的 4.1%。
- Linux 那 40 万行里 90% 是真实 GPU 驱动；核心 8.4 万行对教学内核
  本来就是降维打击，没人真的从头写它。

## 结论

- 论**结构**：GNOS 现在和 Uinxed 同构，找 GEM 去 `gem/`，找 atomic 去
  `kms/`，不再需要在平铺目录里靠文件名猜。
- 论**完整度**：PRIME 已经是能跨进程传递的真 dma-buf，不再是进程内
  的 fd 编号表；这是这次对齐里最实质的能力提升。
- 论**诚实**：硬件后端仍然只有 VBE/stdvga，QEMU 显卡就写 QEMU 驱动，
  没有假装支持真实 GPU。
- 一句话：GNOS 以前造了个玩具枪（legacy）又买了把模型枪（ported），
  现在把模型枪拆成标准零件、补上真弹匣（dma-buf PRIME），至少口径
  和 Uinxed 一致了；对面 LuminaOS 徒手画了个准星，而 Linux 是军火库
  本身。
