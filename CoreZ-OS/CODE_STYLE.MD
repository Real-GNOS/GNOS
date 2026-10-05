# 代码规范

## 1. 命名规范

### 1.1 struct / enum / union 标签

全大写 + 域前缀。

规则：`struct <PREFIX>_<NAME>`，全大写，下划线分段。核心高频类型（`TASK`、`FILE` 类）允许省略前缀，使用单段名。

```c
struct EXT2_DIRENT { ... };      /* 正确：域前缀 + 大写 */
struct TASK { ... };             /* 正确：核心高频类型允许单段 */
struct Registers { ... };        /* 违例：PascalCase */
struct task_struct { ... };      /* 违例：小写下划线 */
```

`enum` / `union` 标签遵循同样规则。禁止 `typedef struct` 匿名包装，调用点一律显式写 `struct XXX`。  
typedef 仅允许两种：函数指针类型（小写下划线，如 `thread_func`）和不透明句柄（如 `pid_t`）。

域前缀分配表：

| 前缀 | 归属 | 例子 |
| --- | --- | --- |
| `MM_` | kernel/mm | `MM_POOL` 类 |
| `TASK_` / `SCHED_` | kernel/sched | `TASK_RUNNING` |
| `FS_` / `EXT2_` / `PROC_` | kernel/fs | `EXT2_DIRENT` |
| `NET_` / `ETH_` / `ARP_` / `IP_` / `TCP_` / `UDP_` | 网络协议栈 | `TCP_PCB` |
| `E1000_` / `RTL8139_` | 网卡驱动 | — |
| `PCI_` | drivers/pci 总线枚举 | `PCI_BDF` |
| `IDE_` / `DISK_` / `BLOCK_` / `NVME_` | drivers/block | `IDE_CHANNEL` |
| `WL_` / `GFX_` / `COMP_` / `GUI_` / `VGPU_` | kernel/gui | `WL_SURFACE`、`GFX_RGB`、`COMP_BAR_H` |
| `SYS_` | kernel/syscall 原生 ABI | `SYS_BRK` |
| `EXEC_` | kernel/userprog 程序加载 | `EXEC_IMAGE` |
| `PE_` | kernel/userprog PE/COFF 镜像加载 | `PE_SECTION` |
| `LINUX_` | musl/Linux ABI 兼容层，只用于 ABI 镜像类型 | `LINUX_STAT` |
| `WIN_` | Win32 ABI 兼容层 | `WIN_FMT` |
| `X86_` / `IDT_` / `GDT_` / `MB2_` / `BOOT_` / `SMP_` | arch、引导与 SMP 启动 | `IDT_ENTRY` |
| `TTY_` / `KB_` / `MOUSE_` | drivers/char | `TTY_ICANON` |
| `RB_` / `LIST_` / `STR_` / `RAND_` | lib | `RB_NODE` |
| `PNG_` | lib/png 图像解码 | `PNG_IMAGE` |
| `LC_` | libc/compat 自有 mini-libc | `LC_IOVEC` |

新增子系统在前缀表登记，不要凭空造前缀。

### 1.2 枚举值

全大写 `<PREFIX>_<NAME>`，与所属枚举同前缀。

```c
enum TASK_STATUS {
    TASK_RUNNING = 1u << 0,   /* 位标志显式写 1u << n */
    TASK_READY   = 1u << 1,
};
```

### 1.3 函数

导出函数：小写下划线，`<模块>_<动作>_<宾语>`。例如 `ext2_lookup`、`wm_manage`、`comp_damage_rect`、`thread_yield`。

文件内私有函数必须 `static`，可省模块前缀，如 `arrange`、`bar_invalidate`。

syscall 三层前缀，语义固定：

- `sys_xxx`：功能实现本体；
- `nsys_xxx`：从 `struct X86_REGS` 解包寄存器参数、注册进分发表的薄封装；
- `compat_xxx`：musl/Linux ABI 兼容路径，文件为 `kernel/syscall/linux_compat.c`。

判定语义的函数用 `is_` / `has_` 前缀，返回 `int` 0/1，如 `fs_is_chardev`、`compat_fd_isdir`。

### 1.4 全局变量

小写下划线。必须 `static`，或带模块/语境前缀，如 `task_table`、`foreground_pid`、`kernel_kphys`。

全大写保留给宏与类型标签，禁止用于变量。例如 `extern const struct tty_ops TTY` 应写为 `tty_ops`。

内核数据禁止静态初始化以外的隐式全局状态。跨模块可写的全局必须配套锁，并在头文件注释中声明保护者。

### 1.5 宏与常量

全大写 `<PREFIX>_<NAME>`，如 `MAX_TASKS`、`PIT_HZ`、`WL_MAX_SURFACES`。

位宽相关字面量必须带后缀 `u` / `ul` / `ull`。十六进制前缀用小写 `0x`。哨兵、魔数必须命名，例如 `STACK_MAGIC 0x19860726`。

硬件寄存器位段、ABI 布局偏移允许就地写字面量，但同一文件内第二次出现时必须提为 `#define`，首次出现处注释来源。

### 1.6 文件与目录

文件名全小写 snake_case。`asmFunc.h` 这类命名应改为 `asm_func.h`。

`includes/` 目录树必须与 `kernel/` `drivers/` `arch/` 源码树镜像，一个模块一个子目录。

目录即层：

- `drivers/` 只放硬件驱动，如 e1000、rtl8139、ide、nvme、pci 总线枚举、keyboard、tty；
- 网络协议栈不属于驱动，目标位置 `kernel/net/`；
- 设备驱动不得放进子系统目录；
- 无主头文件要么归位，要么删除。

### 1.7 include guard

统一 `#ifndef <MODULE>_<NAME>_H`，如 `SCHED_THREAD_H`、`FS_EXT2_H`。

禁止 `#pragma once`，禁止 `__XXX_H__` 双下划线风格。guard 名必须含模块段，避免裸 `THREAD_H` 这类碰撞。

---

## 2. 类型使用

内核数据结构、序列化/ABI 字段一律使用 `<stdint.h>` 固定宽度类型，如 `uint32_t`、`int64_t`。

循环索引、局部计数、纯内部布尔允许 `int`。

用户态整型地址往返必须经 `uintptr_t` 中转：`(const void *)(uintptr_t)arg`。

禁止 `long` / `unsigned` 裸类型参与 ABI 或序列化。

---

## 3. 格式规范

缩进 4 空格，禁止 tab。

括号使用 K&R/1TBS，函数与控制语句的 `{` 同行：

```c
int32_t compat_ioctl(int32_t fd, uint32_t cmd) {
    if (bad) {
        return -LINUX_EINVAL;
    }
}
```

行宽 ≤ 100 列。续行对齐到括号内或固定缩进 8 空格。

单语句 `if` 允许省略花括号，但必须独立成行，禁止与其它语句同行。

指针 `*` 贴变量：`struct TASK *t`，不是 `struct TASK* t`。强转后不加空格：`(uint32_t)gfd`。

`switch` 的 `case` 缩进一层，多个 `case` 落同一语句可同行列对齐。

三目运算符链按列对齐。

---

## 4. 注释规范

文档注释统一 `/** ... */`，放在声明前。行内备注用 `//`。中文为主，标识符与术语保留英文。

常用标签：

- `@param <name> <说明>`：参数说明，顺序与参数一致。输出参数在说明里标“输出”。
- `@returns <说明>`：返回值、错误码。失败路径写清返回什么。
- `@remarks <说明>`：细节、并发、锁前提、生命周期、算法背景。
- `@note`：补充说明。
- `@warning`：危险点、调用约束。
- `@see`：相关函数、类型、文件。
- `@deprecated`：弃用原因与替代。
- `@example`：用法示例。
- `@internal`：内部符号，不对外。

没有内容的标签不写。`@param` 无参数时省略，`@returns` 无返回值时省略。

函数：

```c
/**
 * 在目录中查找名字对应的项。
 *
 * @param dir  目录 inode，调用者持有 dir->lock
 * @param name 以 NUL 结尾的路径分量
 * @returns 找到返回项指针；未找到或参数非法返回 NULL
 *
 * @remarks
 * 不修改目录内容。返回指针在目录锁释放后失效
 */
struct EXT2_DIRENT *ext2_lookup(struct EXT2_INODE *dir, const char *name);
```

结构体：

```c
/**
 *  任务控制块
 *
 *  @remarks
 *  由 task_table_lock 保护。生命周期从 task_create 到 task_exit
 */
struct TASK { ... };
```

枚举：

```c
/**
 *  任务状态位
 *
 *  @remarks
 *  位标志，可组合
 */
enum TASK_STATUS { ... };
```

文件头：

```c
/**
 *  线程调度与上下文切换
 *
 *  @remarks
 *  调度队列由 sched_lock 保护。上下文切换必须在关中断窗口内完成
 */
```

必须写注释的位置：

- 每个 struct/enum 标签：用途与生命周期；
- 每个导出函数：参数、返回、锁前提；
- 并发敏感点：“由 xx_lock 保护”“关中断窗口”；
- 非直观算法：COW 决策、TLB 刷新、双 ABI 分流。

禁止：空泛注释、注释掉的代码、与代码不同步的注释。

---

## 5. 头文件规范

头文件必须自包含：单独编译该头不报错。

`.c` 的第一个 include 是自己的头，其后按字母序。

项目内 include 用引号 + 从 `includes/` 起的全路径：`#include "kernel/sched/thread.h"`。

跨模块共享的类型、常量放 `includes/` 对应子树。禁止在 `.c` 里定义公共 struct。

---

## 6. 错误码与双 ABI 约定

原生路径：失败返回 `-1` 或负的错误值，`errno` 字段只在有 POSIX 对应时设置。

compat 路径：必须返回 `-LINUX_EXXX`，且用户指针先过 `access_ok`。

两条路径不得混用错误码集合。compat 层禁止直接改内核语义，只做翻译。