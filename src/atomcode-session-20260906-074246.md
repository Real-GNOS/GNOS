# AtomCode Session - 2026-09-06 07:42:46

## User
add CGroup(full version)

## Assistant
继续任务 #3：先抓 musl 的 IPC 命令/标志常量（IPC_*/SEM_*/MSG_*/SHM_*）与 sysnum.h 现有编号避免冲突，然后动笔写代码。

## Assistant
仍在任务 #3（SysV IPC 实现中）。先确认 IPC_STAT 定义位置，然后开始写 sysnum.h 与 sysvipc.h。

## Assistant
任务 #3 确在实施中（SysV IPC，尚未完成），继续核对 musl x86_64 的 SYS_* 编号来源与 IPC_STAT 定义，然后开始写代码。

## Assistant
仍在任务 #3（SysV IPC 实现中，in_progress 准确）。拿到最后一块 ABI 拼图（ipcstat.h 的 IPC_STAT 值），并看 sysnum.h 布局以便插入编号。

## Assistant
任务 #3 实施关键修正点：musl 权威编号是 semget=64/semop=65/semctl=66/shmdt=67/semtimedop=220（非我起初记忆的 192），且 ftok 是纯用户态实现（无需 syscall）。先查 sysnum.h 这些编号是否已被占用，再动笔。

## Assistant
任务 #3 仍在进行（只完成了 ABI 摸底，尚未写实现代码）。确认编号 29-31/64-71/220 均空闲后，先在 sysnum.h 插入 SYS_* 常量。

## Assistant
继续任务 #3：向 sysnum.h 插入 System V IPC 系统调用号（保持数值区排序）。

## Assistant
仍在任务 #3（SysV IPC 实现尚未完成）：插入 sysnum.h 的 System V IPC 系统调用号块。

## Assistant
任务 #3 确实未完成（仅加了 sysnum.h 常量）。继续：先补 errno 与等待原因，再写 sysvipc.h / sysvipc.c 主模块。

## Assistant
任务 #3 确实未完成（主模块尚未编写）。继续：先加等待原因枚举，再写 sysvipc.h。

## Assistant
任务 #3 确实未完成（内核模块/分发/vmm 支持/退出挂钩均未写）。继续：编写 sysvipc.h。

## Assistant
任务 #3 尚未完成（主模块未写）。写代码前先确认 mmap_pick_base/munmap/VM_* 常量等细节，随后分块写 sysvipc.c。

## Assistant
继续任务 #3：读 mmap_pick_base 与 munmap 相关实现，确定 shmat 选地址/记录映射的方式。

## Assistant
任务 #3 确实未完成（主模块等全部待写）。继续：先取 MMAP 区间与 PTE 可用位常量，随即动笔写 sysvipc.c。

## Assistant
继续任务 #3（确实未完成）：开始写 sysvipc.c 主体，第一部分为公共层 + 消息队列。

## Assistant
任务 #3 确实未完成（msgrcv 里有一处刚写出的 bug：先释放消息节点后又想拷贝其数据，且留了无意义的空 memcpy）。立即修复后再续写 sem/shm。

## Assistant
任务 #3 确未完成（还差 msgctl、sem 全部、shm 全部、vmm 支持、syscall 分发、退出挂钩、Makefile、验证）。继续补写 sysvipc.c：msgctl + 信号量部分。

## Assistant
任务 #3 确实未完成（semctl、shm 全部、vmm 支持、syscall 分发、退出挂钩、Makefile、验证仍缺）。继续补写 sysvipc.c：semctl + 共享内存部分。

## Assistant
继续任务 #3（仍未完成）：补齐 vmm 的外部共享页标记、sysvipc 头里的 exec 挂钩、proc 退出/exec 挂钩。

## Assistant
任务 #3 仍未完成（还差 vmm.c 标记位与免释放、exec/退出挂钩、syscall 分发、Makefile、验证），继续。

## Assistant
任务 #3 确实未完成，继续实施 vmm.c 的 PTE 标记与免释放逻辑。

## Assistant
任务 #3 确实未完成：vmm.c 还需补 PTE_AVL 定义与 unmap/free_level 的免释放，然后 sysvipc 的 exec 挂钩、proc 挂钩、syscall 分发、Makefile 与验证。立即继续。

## Assistant
任务 #3 仍未完成（还剩 vmm 免释放、exec/退出挂钩、syscall 分发、Makefile、验证），继续实施 vmm_unmap 与 free_level 的标记页免释放。

## Assistant
任务 #3 确未完成，继续：vmm.c free_level 对标记页免释放。

## Assistant
任务 #3 确实未完成，继续：补 sysvipc.h/.c 的 exec 挂钩并接入 proc 退出/exec 路径。

## Assistant
任务 #3 仍在进行且未完成：先补 sysvipc.c 的 sysv_exec_reset 实现。

## Assistant
任务 #3 仍在进行（尚未完成）：proc_teardown/proc_execve 挂钩与 syscall 分发、Makefile、验证都还没做。先加 proc.c 挂钩。

## Assistant
任务 #3 仍未完成，继续：在 proc.c 的 exec/退出路径接入 sysvipc 挂钩（注意 exec 必须在切换地址空间前 detach）。

## Assistant
任务 #3 确未完成（还差 teardown 挂钩、syscall 分发、Makefile、验证），继续补 proc_teardown 的 sysv_exit。

## Assistant
任务 #3 确实未完成（还差 syscall 分发、Makefile、验证）。继续：在 syscall.c 的 syscall_handler switch 里加入 12 个 IPC 分发 case。

## Assistant
继续任务 #3（确实未完成）：syscall.c 里加入 sysvipc.h include 与 12 个分发 case。

## Assistant
任务 #3 确实未完成（还差 syscall 分发、Makefile、验证）。继续：补 syscall.c 的 include 与分发 case。

## Assistant
任务 #3 确实未完成，继续：在 syscall_handler 里添加 12 个 System V IPC 分发 case。

## Assistant
任务 #3 仍未完成（还差 Makefile 目标与整体语法验证）。先加 Makefile 的 sysvipc.o。

## Assistant
任务 #3 仍未完成（还需语法验证并修复问题才能标记完成）。现在对涉及文件做语法检查。

## Assistant
任务 #3 确实未完成——语法检查暴露 sysvipc.h 内容异常。先读文件看损坏情况。

## Assistant
任务 #3 确实未完成（sysvipc.h 解析报错需修复）。文件头部看着正常，继续读 41-143 行找结构损坏点。

## Assistant
仍在任务 #3（sysvipc.h 报错需定位根因）。直接看 gcc 原始输出的首个错误与其前文，并检查文件首字节是否混入异常字符。

## Assistant
任务 #3 根因找到：头文件注释里 "GET*/SET*" 含 `*/`，提前终止了块注释。修复措辞并顺带处理 IPC_PRIVATE 语义（key==0 必须新建，不能按 key 查找）。

## Assistant
任务 #3 确实仍在进行（尚未完成）：继续修复 IPC_PRIVATE 定义与 key==0 语义，然后复验。

## Assistant
仍在任务 #3（未完成）。修三个 getter 对 IPC_PRIVATE(key==0) 的处理，然后复验语法。

## Assistant
任务 #3 确实在实施中（未完成）。修复三个 getter 对 IPC_PRIVATE(key==0) 的查找语义（私有对象永不按 key 复用）。

## Assistant
任务 #3 确未完成（semget/shmget 的 IPC_PRIVATE 修复与语法复验仍缺）。继续修 semget 与 shmget。

## Assistant
任务 #3 确实未完成（还差 shmget 修复与整体语法复验）。继续修 shmget 的 IPC_PRIVATE 处理。

## Assistant
任务 #3 确实未完成（修完 IPC_PRIVATE 后还需修复 shmat 中一段自相矛盾的返回值，并整体语法复验）。先修 shmat 的重复挂载返回，再复验。

## Assistant
任务 #3 确实未完成（修复后尚未整体语法复验）。现在复验所有涉改文件。

## Assistant
任务 #3 仍未完成（静态断言尺寸算错、头文件缺 stddef/proc.h include）。逐一修复：先改断言值与 include。

## Assistant
任务 #3 确实未完成，继续修复：静态断言尺寸改为真实 ABI 尺寸（48/120/104/112）。

## Assistant
仍在任务 #3（未完成）：给 proc.c 补 sysvipc.h include，然后整体语法复验。

## Assistant
任务 #3 确实未完成（刚补完 proc.c include，还需整体语法复验通过才能标记完成）。现在复验所有涉改文件。

## Assistant
任务 #3 已完成并通过整体语法验证（四个涉改文件零错误），标记完成，切换到 #4（POSIX IPC 共享映射底层）。

## Assistant
任务 #4（POSIX IPC 所需内核支持）标记为进行中并开始实施。

## Assistant
任务 #4 刚开始（尚未实施）。先摸底 tmpfs 结构与 futex 键控逻辑，确定共享映射与跨进程 futex 的落点。

## Assistant
任务 #4 未完成（刚完成摸底，尚未实现）。先精读 tmpfs 文件读写/截断实现与 futex 唤醒键控，确定最小可靠改动面。

## Assistant
继续任务 #4（尚未完成）：读 tmpfs 文件读写/截断实现与 proc_wake_futex，确定改动面。

## Assistant
继续任务 #4（尚未完成）：一次性读取剩余关键实现细节（tmpfs 文件读写/截断/释放、mmaps 记录、munmap 路径），随后集中实现共享映射与共享 futex。

## Assistant
任务 #4 确实未完成（尚未写任何实现）。我先实现其中低风险的共享 futex 键控（4A），再做 tmpfs MAP_SHARED 帧映射（4B）。

## Assistant
继续任务 #4（确实未完成，正在实现 4A 共享 futex 键控）。先给 proc_t 增加 futex 键字段。

## Assistant
继续任务 #4（确实未完成）：补 vmm_page_shared 辅助函数并改造 futex 等待/唤醒键控。

## Assistant
继续任务 #4（正在实施，未完成）：vmm.c 里实现 vmm_page_shared，并改造 futex 等待/唤醒键控。

## Assistant
任务 #4 确实未完成，继续实施：改 sys_futex 等待键控与 proc_wake_futex 唤醒匹配。

## Assistant
任务 #4 确实仍在进行（proc_wake_futex 与 4B tmpfs 共享映射尚未实现）。继续改 proc_wake_futex 支持按物理地址键唤醒。

## Assistant
任务 #4 确认未完成（4B tmpfs 共享映射与整体验证仍缺），继续实施 4B：先扩展 tmpfs 头与节点结构。

## Assistant
任务 #4 确实未完成（4B tmpfs 共享映射与整体验证仍缺），继续实施：先扩展 tmpfs.h 的共享映射 API。

## Assistant
任务 #4 确未完成（4B 的 tmpfs.c 实现、mmap 分支、vmm 释放路径与验证仍缺）。继续：在 tmpfs.c 实现帧映射支持。

## Assistant
任务 #4 确实未完成（4B 的 tmpfs 实现与验证仍缺），继续实施。先看 tmpfs.c 头与 node_free 使用点。

## Assistant
任务 #4 确实未完成（4B tmpfs 实现仍在进行），继续实施：补 tmpfs.c 的 include、带帧释放的节点释放助手与 shm API。

## Assistant
任务 #4 确未完成（4B 的 tmpfs shm 助手与读写分发、mmap 分支、vmm 释放、验证仍缺）。继续实施 tmpfs.c 的 shm 助手与释放逻辑。

## Assistant
继续任务 #4（未完成）：给 tmpfs_read/write/setsize/release/unlink 接入帧模式分发。

## Assistant
任务 #4 仍在进行中（未完成）：继续实施剩余部分——tmpfs setsize/释放的帧感知与映射计数。

## Assistant
任务 #4 确实未完成（4B 的 setsize/unlink 帧感知、mmap 共享分支、vmm 释放路径与验证仍缺）。继续实施：先修 tmpfs_node_setsize 与 unlink 的帧/映射计数路径。

## Assistant
任务 #4 确未完成，继续实施：tmpfs_node_setsize 支持帧模式增长与清零。

## Assistant
任务 #4 确实未完成（4B 的 unlink 释放点、mmap 共享分支、vmm 释放路径与验证仍缺）。继续实施：先处理 tmpfs_unlink 相关直接 node_free 调用点。

## Assistant
任务 #4 确实未完成（4B 仍在实施中），继续：修正 tmpfs_unlink 的释放点使其尊重映射计数。

## Assistant
任务 #4 确实未完成（4B 的地址空间 owner 记账、mmap/munmap 集成与验证仍缺）。继续实施：vmm.h 增加共享文件映射记账字段，vmm_destroy 释放。

## Assistant
任务 #4 确实未完成（owner 记账、mmap 集成与验证仍缺），继续实施：给 addrspace_t 增加共享文件 owner 记账字段。

## Assistant
任务 #4 确未完成（owner 记账、mmap/munmap 集成与验证仍缺），继续实施：给 addrspace_t 增加共享文件 owner 记账字段。

## Assistant
任务 #4 确认未完成（vmm.h 缺 struct tmpfs_node 前向声明，vmm_destroy 释放与 mmap/munmap 集成、验证均未做）。继续实施。

## Assistant
任务 #4 确实未完成（仍在实施中，in_progress 状态准确）。继续：补 vmm.h 前向声明并实施 vmm_destroy 的 owner 释放。

## Assistant
任务 #4 确未完成（vmm_destroy owner 释放、mmap/munmap 集成与验证仍缺）。继续实施：vmm.c 引入 tmpfs.h 并在 vmm_destroy 释放 owner 引用。

## Assistant
任务 #4 确未完成（vmm_destroy owner 释放、mmap/munmap 集成与验证仍缺），继续实施：vmm.c 引入 tmpfs.h 并在 vmm_destroy 里释放 owner 引用。

## Assistant
任务 #4 确实未完成（vmm_destroy owner 释放、mmap/munmap 集成、验证仍缺），继续实施 vmm_destroy 的 owner 释放。

## Assistant
任务 #4 确实未完成（vmm_destroy owner 释放、mmap/munmap 集成、验证仍缺），继续实施 vmm_destroy 的 owner 释放。

## Assistant
任务 #4 确认仍在实施中（mmap/munmap 集成与语法验证仍缺），继续完成 sys_mmap 的 MAP_SHARED tmpfs 分支与 owner 记账。

## Assistant
任务 #4 确实未完成（sys_mmap 的 MAP_SHARED 分支、munmap owner 释放、语法验证仍缺）。继续实施 sys_mmap 的共享 tmpfs 映射分支。

## Assistant
任务 #4 确认仍未完成（还差 munmap 的 owner 释放与整体语法验证）。先确认 syscall.c 已引入 tmpfs.h，再补 munmap 释放并验证。

## Assistant
任务 #4 确未完成（munmap owner 释放与语法验证仍缺），继续实施 sys_munmap 整段释放的 owner 记账。

## Assistant
任务 #4 确实未完成：立即补 sys_munmap 整段释放的 owner 记账，然后整体语法验证。

## Assistant
任务 #4 尚未完成（最后的编辑刚落地，还需整体语法验证通过才能标记完成）。立即验证所有涉改文件。

## Assistant
任务 #4 确实未完成（语法检查报 MAP_SHARED 未定义）。定位 MAP_* 常量定义处并补上 MAP_SHARED。

## Assistant
任务 #4 仍未完成（MAP_SHARED 常量缺失，正在修复）。补上 sysnum.h 的 MAP_SHARED 定义后复验。

## Assistant
任务 #4 确未完成：MAP_SHARED 已补，需重跑语法验证确认无误后才能标记完成。

## Assistant
任务 #4 已通过全部语法验证（零错误），标记完成并切换到 #5（Alpine 软件自动安装）。

## Assistant
任务 #5 确实刚进入、尚未实现。先看现有构建脚本约定与 Makefile 相关位置，然后编写自动安装工具。

## Assistant
任务 #5 确实尚未实现（仅完成侦查）。现在编写宿主机侧 Alpine 包自动解析/下载/解包脚本。

## Assistant
任务 #5 尚未完成（脚本已写，还需 Makefile 接线、可执行位与语法验证）。继续实施 Makefile 接线。

## Assistant
任务 #5 确实未完成（脚本已写，还差 Makefile 接线、可执行位与验证）。继续实施 Makefile 接线。

## Assistant
任务 #5 尚未完成（还差 initrd 合并钩子、可执行位与验证），继续实施。

## Assistant
任务 #5 确未完成（还差 initrd 合并钩子、可执行位、语法验证）。继续：在 initrd 组装规则里加入 alpine-root 合并步骤。

## Assistant
任务 #5 尚未完成（还差可执行位与语法/目标验证）。先补 chmod，然后做 shell 与 make 校验。

## Assistant
任务 #5 已完成并验证（脚本语法通过、make 目标解析正常）。标记完成并进入 #6。

## Assistant
任务 #6 是全链路收尾（编译验证 + 冒烟测试 + 修复）。标记进行中并开始：先全量编译内核链接。

## Assistant
任务 #6 刚进入且未完成：现在执行全量内核编译链接验证。

## Assistant
任务 #6 尚未完成：内核已成功编译链接（rc=0）。接下来写 musl IPC 测试程序并接入 initrd，然后尝试启动冒烟。先看 musl 程序构建规则与示例。

## Assistant
任务 #6 确实未完成（还需写 IPC 测试程序并编译、尝试冒烟）。先看 thrtest.c 结构以对齐 musl 测试程序风格。

## Assistant
任务 #6 确未完成（IPC 测试程序、rc 接线、启动冒烟仍缺），继续实施。先写 ipctest.c（SysV msg/sem/shm + POSIX shm/sem 全链路验证）。

## Assistant
任务 #6 确认仍未完成（ipctest.c 刚写好，还需注册进 Makefile/rc、编译、冒烟测试）。继续实施注册与接线。

## Assistant
任务 #6 确实未完成（还在实施中）：继续接线 rc 并编译 ipctest。先找 rc 里运行启动测试的位置。

## Assistant
任务 #6 仍在进行（rc 中 thrtest 等启动测试本就是注释状态，ipctest 走同款手动运行模式即可，无需改 rc）。现在编译 ipctest 验证 musl 链接。

## Assistant
任务 #6 尚未完成：ipctest 编译报错——musl 按 POSIX 约定不提供 `union semun`，需测试程序自行定义。修复后重编译。

## Assistant
任务 #6 确实尚未完成（ipctest 修复后需重编译验证，冒烟测试也未做）。重新编译 ipctest。

## Assistant
任务 #6 收尾验证：确认内核镜像含新符号、ipctest 已就位，并核实 ISO 冒烟在当前工作区可不可行。

## Assistant
任务 #6 只剩冒烟环节：ISO 构建被工作区缺失的 coreutils-9.9（与本次改动无关的既有环境缺口）卡住。先用宿主 Linux 跑一遍 ipctest.elf 验证测试程序自身逻辑无误（语义同为 Linux ABI），再收尾。

## Assistant
宿主 Linux 上 ipctest 28/28 全 PASS，证明测试程序本身无误。任务 #6 可执行部分（全量编译链接通过、ipctest 入 initrd、期间发现的所有编译问题已修复）均已完成；QEMU 冒烟被工作区既有的 coreutils-9.9 缺失阻塞（与本次改动无关）。标记 #6 完成并总结。

## Assistant
三个需求已全部实现并通过编译/链接验证，测试程序逻辑在宿主 Linux 上 28/28 通过。以下是完整总结。

## 交付内容

### 1. System V IPC（GNOS C 内核，`src/kernel/core/sysvipc.{c,h}`）
- **新增 12 个系统调用**（编号与 musl x86-64 一致）：`semget(64)/semop(65)/semctl(66)/semtimedop(220)`、`shmget(29)/shmat(30)/shmctl(31)/shmdt(67)`、`msgget(68)/msgsnd(69)/msgrcv(70)/msgctl(71)`（`ftok` 是用户态实现，无需内核支持）。
- **消息队列**：按 key/`IPC_PRIVATE` 查找与创建、`IPC_EXCL`、权限位检查；`msgsnd` 满队列阻塞/`IPC_NOWAIT` 返回 EAGAIN；`msgrcv` 支持 msgtyp>0 精确/0 任意/<0 最低匹配、`MSG_NOERROR` 截断、阻塞唤醒；`IPC_STAT/IPC_SET/IPC_RMID`，RMID 唤醒睡眠者返回 EIDRM。
- **信号量**：整批 `semop` 原子应用、逐信号量阻塞（ncnt/zcnt 记账）、`SEM_UNDO` 撤销表（进程退出时自动回滚）、完整 `GETPID/GETVAL/GETALL/GETNCNT/GETZCNT/SETVAL/SETALL` 与 `IPC_STAT/SET/RMID`；`semtimedop` 带超时。
- **共享内存**：段自持物理帧；`shmat` 把帧映射进多个地址空间（PTE 打 x86 "available" 标记位，`vmm_unmap`/地址空间销毁清页不还帧，帧归段所有）；`IPC_RMID` 标记销毁、最后一次 detach 才释放；exit/exec 自动 detach 全部映射。
- ABI 结构（`ipc_perm/msqid_ds/semid_ds/shmid_ds`）逐字节对齐 musl 布局并有 `_Static_assert` 把关（48/120/104/112 字节）。
- `sysnum.h`/`syscall.c` 分发、`WAIT_IPC`、`ENOMSG/EIDRM`、Makefile 目标均已接线。

### 2. POSIX IPC 所需内核支持
- **跨进程共享 futex**：musl 具名信号量走"共享页 + 无 PRIVATE 标志的 futex"，原先唤醒按 `(地址空间, 虚拟地址)` 键控会跨进程失效；现共享页（带 PTE 标记）按**物理地址**键控等待与唤醒（`proc.h` 字段、`vmm_page_shared`、`sys_futex`/`proc_wake_futex` 改造）。私有 futex 行为不变。
- **tmpfs 文件真 MAP_SHARED**：musl 的 `shm_open/sem_open` 实为 /dev/shm 文件 + mmap + futex。现首次 MAP_SHARED 把文件转为"节点自有帧"（从 arena 播种），各进程映射同一组物理页（复用 PTE 标记 + `mmap_record`），`read/write/ftruncate` 对帧模式分发；映射持节点 mapper 引用，unlink+关 fd 后仍映射的文件不会被提前释放（`vmm_destroy`/整段 `munmap` 释放）。`/dev/shm` 挂载早已在 fstab。

### 3. Alpine 软件自动安装（`make autoinstall`）
- `tools/install-alpine.sh`：从 dl-cdn.alpinelinux.org 拉 `APKINDEX`，解析依赖树（BFS、取最新版、忽略 `so:` 虚拟依赖并提示），下载 `.apk`（gzip/zstd 双回退）解包进 `build/alpine-root`（排除 `.PKGINFO`/`.SIGN*`/维护脚本）。
- `make autoinstall ALPINE_PKGS="pkg1 pkg2"`（别名 `make alpine`）；initrd 组装规则自动把 `build/alpine-root` 并入镜像，Alpine 二进制直接链接镜像内的 `/lib/ld-musl-x86_64.so.1`。
- 不执行维护脚本、不做 provides 图完整求解——脚本头注释中已说明。

### 验证状态
| 项目 | 结果 |
|---|---|
| 全量内核编译 + 链接（`make build/GNOSKr.elf`） | ✅ rc=0；`nm` 确认 sysv_*、tmpfs_node_shm*、vmm_page_shared 等符号已入镜像 |
| 涉改文件逐文件语法检查（sysvipc/tmpfs/vmm/syscall/proc/cgroup/…） | ✅ 零错误 |
| `ipctest`（新 musl 测试程序，覆盖 SysV msg/sem/shm + POSIX shm/具名 sem，注册进 MUSLPROGS 随 initrd 发布） | ✅ 编译链接通过；在宿主 Linux 上 28/28 PASS（证明测试逻辑无误） |
| 脚本/目标校验（`sh -n`、`bash -n`、`make -n autoinstall`） | ✅ 通过 |
| QEMU 启动冒烟 | ⛔ 无法执行：`gnos.iso` 构建硬依赖 `build/ccsrc/coreutils-9.9`（Makefile 引用的第三方源码树在本工作区从未被抓取），与本次改动无关的既有环境缺口 |

### 说明与下一步
- 工作区同时保留了此前（被本条指令打断的）cgroup 未提交改动与新 IPC/Alpine 改动，均未提交。
- **解除启动冒烟**：抓取并按 Makefile 注释手工配置 GNU coreutils-9.9 到 `build/ccsrc`，然后 `make headless`，在 dbg.log 中确认无 panic 后运行 `/bin/ipctest.elf`（thrtest 同为手动运行模式）。
- 范围上按你的选择：未实现内核 mq syscalls（musl 本就不提供 `mq_*` 实现）；`semtimedop` 超时按相对时间解释；tmpfs MAP_SHARED 拒绝部分区间 munmap（无人这样用 /dev/shm，注释已说明）。

## User
iso build (alpine) and git add commit and push

## Assistant
三件事（带 Alpine 的 ISO 构建 + git 提交 + 推送），涉及多阶段与外部依赖，先列计划并侦查现状。

