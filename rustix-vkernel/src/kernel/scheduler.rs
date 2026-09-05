use core::arch::{asm, global_asm};
use core::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering};

use crate::fixed::InlineString;
use crate::arch::console_write;
use crate::ipc::{
    CapabilityId, EndpointId, IpcError, IpcRights, IpcTable, Message, PageLoan, PageLoanId,
    PageLoanTable, ReplyTokenId, RequestKind, IPC_ABI_VERSION, MAX_ENDPOINTS, MAX_LOAN_PAGES,
    MAX_PAYLOAD_BYTES, QUEUE_DEPTH, SERVICE_VFS, SYS_ENDPOINT_CREATE, SYS_IPC_ABI_INFO,
    SYS_IPC_CALL_SEND, SYS_IPC_LOAN_MAP, SYS_IPC_LOAN_SEND, SYS_IPC_RECEIVE, SYS_IPC_REPLY,
    SYS_SERVICE_LOOKUP,
};
use crate::linux::Errno;
use crate::mm::address::{PhysicalAddress, VirtualAddress, PAGE_SIZE, USER_ADDRESS_LIMIT};
use crate::mm::frame;
use crate::mm::paging::{AddressSpace, MapFlags};
use crate::mm::user::{checked_user_length, checked_user_offset, UserAccess};
use crate::services::{DirEntries, InodeKind, VfsService, MAX_VFS_PATH};
use crate::sync::StaticCell;

use super::elf;
use super::run_queue;
use super::sched::MAX_CPUS;
use super::task::{Pid, SharedTaskTable, TaskKind, TaskState, CLONE_SIGHAND, CLONE_THREAD, CLONE_VM};

const MAX_THREADS: usize = 8;
const INITIAL_THREADS: usize = 5;
const BOOT_STACK_TOP: u64 = 0xffff_ffff_8080_0000;
const THREAD_STACK_BASE: u64 = 0xffff_a400_0000_1000;
const THREAD_STACK_STRIDE: u64 = 0x20_000;
const THREAD_STACK_PAGES: usize = 8;
const USER_CODE_ADDRESS: u64 = 0x0000_0000_0040_0000;
const USER_STACK_TOP: u64 = 0x0000_7fff_ffff_f000;
const USER_STACK_PAGES: usize = 64;
const KERNEL_CODE_SELECTOR: u64 = 0x08;
const KERNEL_DATA_SELECTOR: u64 = 0x10;
const USER_CODE_SELECTOR: u64 = 0x23;
const USER_DATA_SELECTOR: u64 = 0x1b;
const INITIAL_RFLAGS: u64 = 0x202;
const MAX_SERVICES: usize = 8;
const MAX_FDS: usize = 16;
const MAX_OPEN_FILES: usize = 24;
const MAX_PIPES: usize = 8;
const PIPE_CAPACITY: usize = 256;
const MAX_EPOLL: usize = 4;
const MAX_SOCKETS: usize = 8;
const SOCKET_BUFFER_BYTES: usize = 1024;
const MAX_MMAP_REGIONS: usize = 16;
const MAX_BATCH_SYSCALLS: usize = 8;
const ASYNC_IO_DEPTH: usize = 8;
const ASYNC_IO_BYTES: usize = 256;
const MMAP_BASE: u64 = 0x0000_6000_0000_0000;
const MMAP_LIMIT: u64 = 0x0000_7000_0000_0000;
const AT_FDCWD: i32 = -100;

const SYS_READ: u64 = 0;
const SYS_WRITE: u64 = 1;
const SYS_OPEN: u64 = 2;
const SYS_FSTAT: u64 = 5;
const SYS_RT_SIGACTION: u64 = 13;
const SYS_RT_SIGPROCMASK: u64 = 14;
const SYS_READV: u64 = 19;
const SYS_WRITEV: u64 = 20;
const SYS_GETPID: u64 = 39;
const SYS_EXECVE: u64 = 59;
const SYS_CLONE: u64 = 56;
const SYS_FORK: u64 = 57;
const SYS_WAIT4: u64 = 61;
const SYS_EXIT: u64 = 60;
const SYS_CLOSE: u64 = 3;
const SYS_LSEEK: u64 = 8;
const SYS_GETDENTS64: u64 = 217;
const SYS_OPENAT: u64 = 257;
const SYS_STATX: u64 = 332;
const SYS_MMAP: u64 = 9;
const SYS_MPROTECT: u64 = 10;
const SYS_MUNMAP: u64 = 11;
const SYS_BRK: u64 = 12;
const SYS_IOCTL: u64 = 16;
const SYS_FCNTL: u64 = 72;
const SYS_DUP3: u64 = 292;
const SYS_PIPE2: u64 = 293;
const SYS_POLL: u64 = 7;
const SYS_SELECT: u64 = 23;
const SYS_EPOLL_CREATE1: u64 = 291;
const SYS_EPOLL_CTL: u64 = 233;
const SYS_EPOLL_WAIT: u64 = 232;
const SYS_SOCKET: u64 = 41;
const SYS_CONNECT: u64 = 42;
const SYS_ACCEPT: u64 = 43;
const SYS_SENDTO: u64 = 44;
const SYS_RECVFROM: u64 = 45;
const SYS_SHUTDOWN: u64 = 48;
const SYS_BIND: u64 = 49;
const SYS_LISTEN: u64 = 50;
const SYS_GETSOCKNAME: u64 = 51;
const SYS_GETPEERNAME: u64 = 52;
const SYS_SOCKETPAIR: u64 = 53;
const SYS_SETSOCKOPT: u64 = 54;
const SYS_GETSOCKOPT: u64 = 55;
const SYS_NANOSLEEP: u64 = 35;
const SYS_UNAME: u64 = 63;
const SYS_GETTIMEOFDAY: u64 = 96;
const SYS_UMASK: u64 = 95;
const SYS_SYSINFO: u64 = 99;
const SYS_TIME: u64 = 201;
const SYS_CLOCK_GETTIME: u64 = 228;
const SYS_CLOCK_NANOSLEEP: u64 = 230;
const SYS_GETCPU: u64 = 309;
const SYS_GETRANDOM: u64 = 318;
const SYS_ARCH_PRCTL: u64 = 158;
const SYS_PRCTL: u64 = 157;
const SYS_SET_TID_ADDRESS: u64 = 218;
const SYS_EXIT_GROUP: u64 = 231;
const SYS_NEWFSTATAT: u64 = 262;
const SYS_GETUID: u64 = 102;
const SYS_GETGID: u64 = 104;
const SYS_CHMOD: u64 = 90;
const SYS_FCHMOD: u64 = 91;
const SYS_CHOWN: u64 = 92;
const SYS_FCHOWN: u64 = 93;
const SYS_FSYNC: u64 = 74;
const SYS_FDATASYNC: u64 = 75;
const SYS_SYNC: u64 = 162;
const SYS_FLOCK: u64 = 73;
const SYS_GETEUID: u64 = 107;
const SYS_GETEGID: u64 = 108;
const SYS_GETCWD: u64 = 79;
const SYS_CHDIR: u64 = 80;
const SYS_CHROOT: u64 = 161;
const SYS_LINKAT: u64 = 265;
const SYS_SYMLINKAT: u64 = 266;
const SYS_READLINKAT: u64 = 267;

const ARCH_SET_FS: u64 = 0x1002;
const ARCH_GET_FS: u64 = 0x1003;
const IA32_FS_BASE: u32 = 0xc000_0100;

const SYS_SELF_HEARTBEAT: u64 = 0x1000;
const SYS_SELF_GETPID: u64 = 0x1001;
const SYS_SELF_SLEEP: u64 = 0x1002;
const SYS_SELF_FORK: u64 = 0x1003;
const SYS_SELF_EXIT: u64 = 0x1004;
const SYS_SELF_WAIT: u64 = 0x1005;
const SYS_SELF_CLONE: u64 = 0x1006;
const SYS_SELF_EXEC: u64 = 0x1007;
const SYS_SELF_SIGNAL: u64 = 0x1008;
const SYS_SELF_SIGRETURN: u64 = 0x1009;
const SYS_SELF_FUTEX_WAIT: u64 = 0x100a;
const SYS_SELF_FUTEX_WAKE: u64 = 0x100b;
const SYS_SELF_NICE: u64 = 0x100c;
const SYS_SELF_SCHEDULER: u64 = 0x100d;
const SYS_SELF_AFFINITY: u64 = 0x100e;
pub const SYS_BATCH: u64 = 0x1200;
pub const SYS_ASYNC_SUBMIT: u64 = 0x1201;
pub const SYS_ASYNC_REAP: u64 = 0x1202;

static INITIALIZED: AtomicBool = AtomicBool::new(false);
static CONTEXT_SWITCHES: AtomicU64 = AtomicU64::new(0);
static KERNEL_HEARTBEAT: AtomicU64 = AtomicU64::new(0);
static USER_HEARTBEAT: AtomicU64 = AtomicU64::new(0);
static USER_SYSCALLS: AtomicU64 = AtomicU64::new(0);
static PROCESS_CYCLES: AtomicU64 = AtomicU64::new(0);
static EXEC_CYCLES: AtomicU64 = AtomicU64::new(0);
static SIGNAL_DELIVERIES: AtomicU64 = AtomicU64::new(0);
static FUTEX_WAKEUPS: AtomicU64 = AtomicU64::new(0);
static POLICY_UPDATES: AtomicU64 = AtomicU64::new(0);
static IPC_USER_DELIVERIES: AtomicU64 = AtomicU64::new(0);
static IPC_USER_REJECTIONS: AtomicU64 = AtomicU64::new(0);
static IPC_ZERO_COPY_BYTES: AtomicU64 = AtomicU64::new(0);
static RECLAIMED_ADDRESS_SPACES: AtomicU64 = AtomicU64::new(0);
static ACTIVE_THREADS: AtomicUsize = AtomicUsize::new(0);
static ONLINE_CPUS: AtomicUsize = AtomicUsize::new(1);
static MIGRATIONS: AtomicU64 = AtomicU64::new(0);
static CPU_TICKS: [AtomicU64; MAX_CPUS] = [const { AtomicU64::new(0) }; MAX_CPUS];

pub fn timer_ticks() -> u64 {
    CPU_TICKS[0].load(Ordering::Relaxed)
}

static RANDOM_STATE: AtomicU64 = AtomicU64::new(0x9e37_79b9_7f4a_7c15);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ThreadState {
    Empty,
    Runnable,
    Sleeping(u64),
    WaitingChild,
    FutexWait(u64),
    IpcReceive {
        capability: u64,
        address: u64,
        capacity: u16,
    },
    Zombie(i32),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ThreadPolicy {
    Normal,
    Fifo,
    RoundRobin,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FdTarget {
    Closed,
    ConsoleInput,
    ConsoleOutput,
    File(u8),
    PipeRead(u8),
    PipeWrite(u8),
    Epoll(u8),
    Socket(u8),
}

#[derive(Clone, Copy)]
struct FdEntry {
    target: FdTarget,
    close_on_exec: bool,
}

impl FdEntry {
    const CLOSED: Self = Self {
        target: FdTarget::Closed,
        close_on_exec: false,
    };
}

#[derive(Clone, Copy)]
struct FdTable {
    entries: [FdEntry; MAX_FDS],
}

#[derive(Clone, Copy)]
struct MmapRegion {
    used: bool,
    start: u64,
    pages: usize,
    prot: u64,
}
impl MmapRegion {
    const fn empty() -> Self {
        Self {
            used: false,
            start: 0,
            pages: 0,
            prot: 0,
        }
    }
}

#[derive(Clone, Copy)]
struct Pipe {
    used: bool,
    refs: u16,
    data: [u8; PIPE_CAPACITY],
    head: usize,
    len: usize,
}
impl Pipe {
    const fn empty() -> Self {
        Self {
            used: false,
            refs: 0,
            data: [0; PIPE_CAPACITY],
            head: 0,
            len: 0,
        }
    }
}

#[derive(Clone, Copy)]
struct EpollWatch {
    used: bool,
    fd: i32,
    events: u32,
    data: u64,
}
impl EpollWatch {
    const fn empty() -> Self {
        Self {
            used: false,
            fd: -1,
            events: 0,
            data: 0,
        }
    }
}
#[derive(Clone, Copy)]
struct Epoll {
    used: bool,
    refs: u16,
    watches: [EpollWatch; 8],
}

#[derive(Clone, Copy)]
struct Socket {
    used: bool,
    refs: u16,
    family: u16,
    kind: u16,
    protocol: u16,
    listening: bool,
    peer: i8,
    pending: i8,
    shutdown_read: bool,
    shutdown_write: bool,
    local: [u8; 16],
    remote: [u8; 16],
    rx: [u8; SOCKET_BUFFER_BYTES],
    rx_len: usize,
    inet: i8,
}
impl Socket {
    const fn empty() -> Self {
        Self {
            used: false,
            refs: 0,
            family: 0,
            kind: 0,
            protocol: 0,
            listening: false,
            peer: -1,
            pending: -1,
            shutdown_read: false,
            shutdown_write: false,
            local: [0; 16],
            remote: [0; 16],
            rx: [0; SOCKET_BUFFER_BYTES],
            rx_len: 0,
            inet: -1,
        }
    }
}
impl Epoll {
    const fn empty() -> Self {
        Self {
            used: false,
            refs: 0,
            watches: [EpollWatch::empty(); 8],
        }
    }
}

impl FdTable {
    const fn empty() -> Self {
        Self {
            entries: [FdEntry::CLOSED; MAX_FDS],
        }
    }

    const fn standard() -> Self {
        let mut table = Self::empty();
        table.entries[0].target = FdTarget::ConsoleInput;
        table.entries[1].target = FdTarget::ConsoleOutput;
        table.entries[2].target = FdTarget::ConsoleOutput;
        table
    }
}

#[derive(Clone, Copy)]
struct OpenFileDescription {
    used: bool,
    references: u16,
    path: InlineString<MAX_VFS_PATH>,
    offset: u64,
    access_mode: u8,
    append: bool,
    directory: bool,
}

impl OpenFileDescription {
    const fn empty() -> Self {
        Self {
            used: false,
            references: 0,
            path: InlineString::new(),
            offset: 0,
            access_mode: 0,
            append: false,
            directory: false,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
struct BatchSyscall {
    number: u64,
    arguments: [u64; 6],
    result: u64,
}

impl BatchSyscall {
    const fn empty() -> Self {
        Self {
            number: 0,
            arguments: [0; 6],
            result: 0,
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
struct AsyncIoSubmission {
    operation: u64,
    fd: u64,
    buffer: u64,
    length: u64,
    offset: u64,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum AsyncIoState {
    Free,
    Pending,
    Complete,
    Failed(Errno),
}

#[derive(Clone, Copy)]
struct AsyncIoSlot {
    state: AsyncIoState,
    owner: u32,
    token: u64,
    operation: u8,
    path: InlineString<MAX_VFS_PATH>,
    offset: u64,
    length: usize,
    result: usize,
    buffer: [u8; ASYNC_IO_BYTES],
}

impl AsyncIoSlot {
    const fn empty() -> Self {
        Self {
            state: AsyncIoState::Free,
            owner: 0,
            token: 0,
            operation: 0,
            path: InlineString::new(),
            offset: 0,
            length: 0,
            result: 0,
            buffer: [0; ASYNC_IO_BYTES],
        }
    }
}

#[derive(Clone, Copy)]
#[repr(C, align(16))]
struct FpuState {
    bytes: [u8; 512],
}

impl FpuState {
    const fn initial() -> Self {
        let mut bytes = [0; 512];
        bytes[0] = 0x7f;
        bytes[1] = 0x03;
        bytes[24] = 0x80;
        bytes[25] = 0x1f;
        Self { bytes }
    }

    fn save(&mut self) {
        unsafe {
            asm!("fxsave64 [{}]", in(reg) self.bytes.as_mut_ptr(), options(nostack));
        }
    }

    fn restore(&self) {
        unsafe {
            asm!("fxrstor64 [{}]", in(reg) self.bytes.as_ptr(), options(nostack));
        }
    }
}

#[derive(Clone, Copy)]
struct Thread {
    id: u32,
    state: ThreadState,
    saved_rsp: u64,
    kernel_stack_top: u64,
    cr3: u64,
    fs_base: u64,
    fpu_state: FpuState,
    affinity: u64,
    policy: ThreadPolicy,
    nice: i8,
    rt_priority: u8,
    runtime_ticks: u64,
    ipc_delivered: bool,
    owns_address_space: bool,
    image: &'static [u8],
    fds: FdTable,
    mmap: [MmapRegion; MAX_MMAP_REGIONS],
    brk: u64,
    cwd: InlineString<MAX_VFS_PATH>,
    root: InlineString<MAX_VFS_PATH>,
    uid: u32,
    gid: u32,
    umask: u16,
}

#[derive(Clone, Copy)]
struct ServiceRegistration {
    service_id: u32,
    pid: u32,
    endpoint: EndpointId,
    owner_capability: CapabilityId,
}

#[derive(Clone, Copy)]
struct ServiceRegistry {
    entries: [Option<ServiceRegistration>; MAX_SERVICES],
}

impl ServiceRegistry {
    const fn new() -> Self {
        Self {
            entries: [None; MAX_SERVICES],
        }
    }

    fn register(&mut self, registration: ServiceRegistration) -> Result<(), ()> {
        if registration.service_id == 0
            || registration.pid == 0
            || self
                .entries
                .iter()
                .flatten()
                .any(|entry| entry.service_id == registration.service_id)
        {
            return Err(());
        }
        let slot = self
            .entries
            .iter_mut()
            .find(|entry| entry.is_none())
            .ok_or(())?;
        *slot = Some(registration);
        Ok(())
    }

    fn lookup(&self, service_id: u32) -> Option<ServiceRegistration> {
        self.entries
            .iter()
            .flatten()
            .find(|entry| entry.service_id == service_id)
            .copied()
    }

    fn remove_pid(&mut self, pid: u32) {
        for entry in &mut self.entries {
            if entry.is_some_and(|registration| registration.pid == pid) {
                *entry = None;
            }
        }
    }
}

impl Thread {
    const fn empty() -> Self {
        Self {
            id: 0,
            state: ThreadState::Empty,
            saved_rsp: 0,
            kernel_stack_top: 0,
            cr3: 0,
            fs_base: 0,
            fpu_state: FpuState::initial(),
            affinity: 1,
            policy: ThreadPolicy::Normal,
            nice: 0,
            rt_priority: 0,
            runtime_ticks: 0,
            ipc_delivered: false,
            owns_address_space: false,
            image: &[],
            fds: FdTable::empty(),
            mmap: [MmapRegion::empty(); MAX_MMAP_REGIONS],
            brk: 0,
            cwd: InlineString::from_str("/"),
            root: InlineString::from_str("/"),
            uid: 0,
            gid: 0,
            umask: 0o022,
        }
    }
}

struct SchedulerState {
    current: usize,
    threads: [Thread; MAX_THREADS],
    ipc: IpcTable,
    page_loans: PageLoanTable,
    services: ServiceRegistry,
    retired_spaces: [u64; MAX_THREADS],
    open_files: [OpenFileDescription; MAX_OPEN_FILES],
    pipes: [Pipe; MAX_PIPES],
    epolls: [Epoll; MAX_EPOLL],
    sockets: [Socket; MAX_SOCKETS],
    file_locks: [FileLock; MAX_FILE_LOCKS],
    async_io: [AsyncIoSlot; ASYNC_IO_DEPTH],
    next_async_token: u64,
}

impl SchedulerState {
    const fn new() -> Self {
        Self {
            current: 0,
            threads: [Thread::empty(); MAX_THREADS],
            ipc: IpcTable::new(),
            page_loans: PageLoanTable::new(),
            services: ServiceRegistry::new(),
            retired_spaces: [0; MAX_THREADS],
            open_files: [OpenFileDescription::empty(); MAX_OPEN_FILES],
            pipes: [Pipe::empty(); MAX_PIPES],
            epolls: [Epoll::empty(); MAX_EPOLL],
            sockets: [Socket::empty(); MAX_SOCKETS],
            file_locks: [FileLock::empty(); MAX_FILE_LOCKS],
            async_io: [AsyncIoSlot::empty(); ASYNC_IO_DEPTH],
            next_async_token: 1,
        }
    }

    fn enqueue(&self, cpu: usize, index: usize) -> bool {
        let thread = self.threads[index];
        thread.state == ThreadState::Runnable
            && thread.affinity & (1u64 << cpu) != 0
            && run_queue::enqueue(cpu, index, run_queue_priority(thread))
    }

    fn pick_next(&self, cpu: usize) -> usize {
        while let Some(index) = run_queue::pop_highest(cpu) {
            let thread = self.threads[index];
            if thread.state == ThreadState::Runnable
                && thread.affinity & (1u64 << cpu) != 0
                && thread.saved_rsp != 0
            {
                return index;
            }
        }
        self.current
    }
}

static SCHEDULER: StaticCell<SchedulerState> = StaticCell::new(SchedulerState::new());

#[repr(C)]
pub struct RegisterFrame {
    pub r15: u64,
    pub r14: u64,
    pub r13: u64,
    pub r12: u64,
    pub r11: u64,
    pub r10: u64,
    pub r9: u64,
    pub r8: u64,
    pub rdi: u64,
    pub rsi: u64,
    pub rbp: u64,
    pub rdx: u64,
    pub rcx: u64,
    pub rbx: u64,
    pub rax: u64,
    pub rip: u64,
    pub cs: u64,
    pub rflags: u64,
    pub rsp: u64,
    pub ss: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
struct UserSignalFrame {
    magic: u64,
    rip: u64,
    rsp: u64,
    rflags: u64,
    rax: u64,
}

const SIGNAL_FRAME_MAGIC: u64 = 0x5255_5354_5349_4746;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SchedulerSummary {
    pub threads: usize,
    pub context_switches: u64,
    pub kernel_heartbeat: u64,
    pub user_heartbeat: u64,
    pub user_syscalls: u64,
    pub process_cycles: u64,
    pub exec_cycles: u64,
    pub signal_deliveries: u64,
    pub futex_wakeups: u64,
    pub policy_updates: u64,
    pub ipc_deliveries: u64,
    pub ipc_rejections: u64,
    pub ipc_zero_copy_bytes: u64,
    pub address_spaces_reclaimed: u64,
    pub task_table_entries: usize,
    pub online_cpus: usize,
    pub migrations: u64,
    pub busiest_cpu_ticks: u64,
}

#[inline(never)]
pub fn initialize() -> Result<(), &'static str> {
    let interrupt_flags = disable_interrupts();
    let result = initialize_inner();
    restore_interrupts(interrupt_flags);
    result
}

#[inline(never)]
fn initialize_inner() -> Result<(), &'static str> {
    if INITIALIZED.load(Ordering::Acquire) {
        return Err("scheduler is already initialized");
    }

    CONTEXT_SWITCHES.store(0, Ordering::Relaxed);
    KERNEL_HEARTBEAT.store(0, Ordering::Relaxed);
    USER_HEARTBEAT.store(0, Ordering::Relaxed);
    USER_SYSCALLS.store(0, Ordering::Relaxed);
    PROCESS_CYCLES.store(0, Ordering::Relaxed);
    EXEC_CYCLES.store(0, Ordering::Relaxed);
    SIGNAL_DELIVERIES.store(0, Ordering::Relaxed);
    FUTEX_WAKEUPS.store(0, Ordering::Relaxed);
    POLICY_UPDATES.store(0, Ordering::Relaxed);
    IPC_USER_DELIVERIES.store(0, Ordering::Relaxed);
    IPC_USER_REJECTIONS.store(0, Ordering::Relaxed);
    IPC_ZERO_COPY_BYTES.store(0, Ordering::Relaxed);
    RECLAIMED_ADDRESS_SPACES.store(0, Ordering::Relaxed);
    ACTIVE_THREADS.store(INITIAL_THREADS, Ordering::Relaxed);
    ONLINE_CPUS.store(crate::arch::online_cpu_count(), Ordering::Release);
    MIGRATIONS.store(0, Ordering::Relaxed);
    for ticks in &CPU_TICKS {
        ticks.store(0, Ordering::Relaxed);
    }

    let kernel_stack_top = map_thread_stack(0)?;
    let boot_cr3 = current_cr3();

    let tasks = SharedTaskTable::new();
    let init_pid = tasks
        .spawn(None, "init", TaskKind::Kernel)
        .map_err(|_| "init task allocation failed")?;
    let kernel_pid = tasks
        .spawn(Some(init_pid), "kernel-worker", TaskKind::Kernel)
        .map_err(|_| "kernel worker task allocation failed")?;
    let user_thread = launch_user_task(
        &tasks,
        init_pid,
        "user-selftest",
        TaskKind::User,
        user_program(),
        1,
        5,
        0,
        0,
    )?;
    let mut ipc = IpcTable::new();
    let mut services = ServiceRegistry::new();
    let service_thread = launch_user_service(
        &tasks,
        init_pid,
        "vfs-service",
        SERVICE_VFS,
        &IPC_SERVICE_PROGRAM,
        2,
        &mut ipc,
        &mut services,
    )?;
    let client_thread = launch_user_task(
        &tasks,
        init_pid,
        "vfs-client",
        TaskKind::User,
        &IPC_CLIENT_PROGRAM,
        3,
        5,
        0,
        0,
    )?;

    let kernel_rsp = unsafe {
        build_kernel_frame(
            kernel_stack_top,
            kernel_thread_entry as *const () as usize as u64,
        )
    };
    let state = unsafe { &mut *SCHEDULER.get() };
    state.current = 0;
    state.threads = [Thread::empty(); MAX_THREADS];
    state.ipc = ipc;
    state.page_loans = PageLoanTable::new();
    state.services = services;
    state.retired_spaces = [0; MAX_THREADS];
    state.open_files = [OpenFileDescription::empty(); MAX_OPEN_FILES];
    state.pipes = [Pipe::empty(); MAX_PIPES];
    state.epolls = [Epoll::empty(); MAX_EPOLL];
    state.sockets = [Socket::empty(); MAX_SOCKETS];
    state.file_locks = [FileLock::empty(); MAX_FILE_LOCKS];
    state.async_io = [AsyncIoSlot::empty(); ASYNC_IO_DEPTH];
    state.next_async_token = 1;
    state.threads[..INITIAL_THREADS].copy_from_slice(&[
        Thread {
            id: init_pid,
            state: ThreadState::Runnable,
            saved_rsp: 0,
            kernel_stack_top: BOOT_STACK_TOP,
            cr3: boot_cr3,
            ..Thread::empty()
        },
        Thread {
            id: kernel_pid,
            state: ThreadState::Runnable,
            saved_rsp: kernel_rsp,
            kernel_stack_top,
            cr3: boot_cr3,
            nice: 5,
            ..Thread::empty()
        },
        user_thread,
        service_thread,
        client_thread,
    ]);
    run_queue::reset();
    if !run_queue::self_test() {
        return Err("run queue self-test failed");
    }
    for index in 1..INITIAL_THREADS {
        let _ = state.enqueue(0, index);
    }
    INITIALIZED.store(true, Ordering::Release);
    Ok(())
}

fn launch_user_task(
    tasks: &SharedTaskTable,
    parent: u32,
    name: &'static str,
    kind: TaskKind,
    program: &[u8],
    stack_slot: usize,
    nice: i8,
    argument0: u64,
    argument1: u64,
) -> Result<Thread, &'static str> {
    let pid = tasks
        .spawn(Some(parent), name, kind)
        .map_err(|_| "user task allocation failed")?;
    prepare_user_thread(pid, program, stack_slot, nice, argument0, argument1)
}

fn launch_user_service(
    tasks: &SharedTaskTable,
    parent: u32,
    name: &'static str,
    service_id: u32,
    program: &[u8],
    stack_slot: usize,
    ipc: &mut IpcTable,
    services: &mut ServiceRegistry,
) -> Result<Thread, &'static str> {
    let pid = tasks
        .spawn(Some(parent), name, TaskKind::Service)
        .map_err(|_| "service task allocation failed")?;
    let (endpoint, owner_capability) = ipc
        .create_endpoint(pid)
        .map_err(|_| "service endpoint allocation failed")?;
    services
        .register(ServiceRegistration {
            service_id,
            pid,
            endpoint,
            owner_capability,
        })
        .map_err(|_| "service registration failed")?;
    prepare_user_thread(
        pid,
        program,
        stack_slot,
        0,
        owner_capability.raw(),
        u64::from(service_id),
    )
}

fn prepare_user_thread(
    pid: u32,
    program: &[u8],
    stack_slot: usize,
    nice: i8,
    argument0: u64,
    argument1: u64,
) -> Result<Thread, &'static str> {
    let kernel_stack_top = map_thread_stack(stack_slot)?;
    let (cr3, user_rsp) = create_user_image(program)?;
    let saved_rsp = unsafe {
        build_user_frame_with_bootstrap(
            kernel_stack_top,
            USER_CODE_ADDRESS,
            user_rsp,
            argument0,
            argument1,
        )
    };
    Ok(Thread {
        id: pid,
        state: ThreadState::Runnable,
        saved_rsp,
        kernel_stack_top,
        cr3,
        nice,
        owns_address_space: true,
        fds: FdTable::standard(),
        ..Thread::empty()
    })
}

pub fn summary() -> SchedulerSummary {
    SchedulerSummary {
        threads: ACTIVE_THREADS.load(Ordering::Relaxed),
        context_switches: CONTEXT_SWITCHES.load(Ordering::Relaxed),
        kernel_heartbeat: KERNEL_HEARTBEAT.load(Ordering::Relaxed),
        user_heartbeat: USER_HEARTBEAT.load(Ordering::Relaxed),
        user_syscalls: USER_SYSCALLS.load(Ordering::Relaxed),
        process_cycles: PROCESS_CYCLES.load(Ordering::Relaxed),
        exec_cycles: EXEC_CYCLES.load(Ordering::Relaxed),
        signal_deliveries: SIGNAL_DELIVERIES.load(Ordering::Relaxed),
        futex_wakeups: FUTEX_WAKEUPS.load(Ordering::Relaxed),
        policy_updates: POLICY_UPDATES.load(Ordering::Relaxed),
        ipc_deliveries: IPC_USER_DELIVERIES.load(Ordering::Relaxed),
        ipc_rejections: IPC_USER_REJECTIONS.load(Ordering::Relaxed),
        ipc_zero_copy_bytes: IPC_ZERO_COPY_BYTES.load(Ordering::Relaxed),
        address_spaces_reclaimed: RECLAIMED_ADDRESS_SPACES.load(Ordering::Relaxed),
        task_table_entries: SharedTaskTable::new().active_count(),
        online_cpus: ONLINE_CPUS.load(Ordering::Acquire),
        migrations: MIGRATIONS.load(Ordering::Relaxed),
        busiest_cpu_ticks: CPU_TICKS
            .iter()
            .map(|ticks| ticks.load(Ordering::Relaxed))
            .max()
            .unwrap_or(0),
    }
}

pub fn record_migrations(count: usize) {
    MIGRATIONS.fetch_add(count as u64, Ordering::Relaxed);
}

pub extern "C" fn on_timer(frame: *mut RegisterFrame) -> *mut RegisterFrame {
    let cpu = (crate::arch::current_cpu_id() as usize).min(MAX_CPUS - 1);
    CPU_TICKS[cpu].fetch_add(1, Ordering::Relaxed);
    if cpu != 0 || !INITIALIZED.load(Ordering::Acquire) {
        return frame;
    }

    let state = unsafe { &mut *SCHEDULER.get() };
    let now = CPU_TICKS[cpu].load(Ordering::Relaxed);
    let mut woken = 0u16;
    for (index, thread) in state.threads.iter_mut().enumerate() {
        if matches!(thread.state, ThreadState::Sleeping(deadline) if deadline <= now) {
            thread.state = ThreadState::Runnable;
            let _ = SharedTaskTable::new().set_state(thread.id, TaskState::Ready);
            woken |= 1u16 << index;
        }
    }
    for index in 0..MAX_THREADS {
        if woken & (1u16 << index) != 0 {
            let _ = state.enqueue(cpu, index);
        }
    }
    let current = state.current;
    state.threads[current].saved_rsp = frame as u64;
    state.threads[current].runtime_ticks = state.threads[current].runtime_ticks.saturating_add(1);
    let _ = SharedTaskTable::new().account_tick(state.threads[current].id);
    reclaim_retired_spaces(state);
    let _ = state.enqueue(cpu, current);
    let next = state.pick_next(cpu);
    if next == current || state.threads[next].saved_rsp == 0 {
        return frame;
    }

    switch_to(state, current, next)
}

const SYSCALL_TRACE_LEN: usize = 32;
static SYSCALL_TRACE_INDEX: AtomicUsize = AtomicUsize::new(0);
static SYSCALL_TRACE_NUMBERS: [AtomicU64; SYSCALL_TRACE_LEN] =
    [const { AtomicU64::new(0) }; SYSCALL_TRACE_LEN];

pub fn dump_recent_syscalls() {
    console_write("\n[trace] recent syscalls:");
    let total = SYSCALL_TRACE_INDEX.load(Ordering::Relaxed);
    let start = total.saturating_sub(SYSCALL_TRACE_LEN);
    for index in start..total {
        let slot = index % SYSCALL_TRACE_LEN;
        let number = SYSCALL_TRACE_NUMBERS[slot].load(Ordering::Relaxed);
        let mut hex = [0u8; 8];
        for (position, offset) in (0..8).rev().enumerate() {
            let digit = ((number >> (offset * 4)) & 0xf) as u8;
            hex[position] = if digit < 10 { b'0' + digit } else { b'a' + digit - 10 };
        }
        console_write(" 0x");
        console_write(core::str::from_utf8(&hex).unwrap_or("?"));
    }
    console_write("\n");
}

pub extern "C" fn on_syscall(frame: *mut RegisterFrame) -> *mut RegisterFrame {
    let frame = unsafe { &mut *frame };
    if frame.cs & 3 != 3 || !INITIALIZED.load(Ordering::Acquire) {
        set_syscall_error(frame, Errno::ENOSYS);
        return frame;
    }

    USER_SYSCALLS.fetch_add(1, Ordering::Relaxed);
    let trace_slot = SYSCALL_TRACE_INDEX.fetch_add(1, Ordering::Relaxed) % SYSCALL_TRACE_LEN;
    SYSCALL_TRACE_NUMBERS[trace_slot].store(frame.rax, Ordering::Relaxed);
    let syscall = frame.rax;
    let state = unsafe { &mut *SCHEDULER.get() };
    advance_async_io(state);
    match syscall {
        SYS_READ => sys_read(state, frame),
        SYS_WRITE => sys_write(state, frame),
        SYS_OPEN => sys_open(state, frame),
        SYS_FSTAT => sys_fstat(state, frame),
        SYS_NEWFSTATAT => sys_newfstatat(state, frame),
        SYS_READV => sys_readv(state, frame),
        SYS_WRITEV => sys_writev(state, frame),
        SYS_RT_SIGACTION => sys_rt_sigaction(state, frame),
        SYS_RT_SIGPROCMASK => sys_rt_sigprocmask(state, frame),
        SYS_PRCTL => sys_prctl(state, frame),
        SYS_GETCWD => sys_getcwd(state, frame),
        SYS_CHDIR => sys_chdir(state, frame),
        SYS_CHROOT => sys_chroot(state, frame),
        SYS_CHMOD => sys_chmod(state, frame),
        SYS_FCHMOD => sys_fchmod(state, frame),
        SYS_CHOWN => sys_chown(state, frame),
        SYS_FCHOWN => sys_fchown(state, frame),
        SYS_FSYNC | SYS_FDATASYNC => sys_fsync(state, frame),
        SYS_SYNC => sys_sync(state, frame),
        SYS_FLOCK => sys_flock(state, frame),
        SYS_GETPID => {
            frame.rax = u64::from(state.threads[state.current].id);
            frame
        }
        SYS_GETUID | SYS_GETEUID => {
            frame.rax = u64::from(state.threads[state.current].uid);
            frame
        }
        SYS_GETGID | SYS_GETEGID => {
            frame.rax = u64::from(state.threads[state.current].gid);
            frame
        }
        SYS_UMASK => {
            let thread = &mut state.threads[state.current];
            let old = thread.umask;
            thread.umask = frame.rdi as u16 & 0o777;
            frame.rax = u64::from(old);
            frame
        }
        SYS_SET_TID_ADDRESS => {
            frame.rax = u64::from(state.threads[state.current].id);
            frame
        }
        SYS_EXIT | SYS_EXIT_GROUP => exit_current(state, frame, frame.rdi as i32),
        SYS_EXECVE => sys_execve(state, frame),
        SYS_CLONE => sys_clone(state, frame),
        SYS_FORK => sys_fork(state, frame),
        SYS_WAIT4 => sys_wait4(state, frame),
        SYS_CLOSE => sys_close(state, frame),
        SYS_LSEEK => sys_lseek(state, frame),
        SYS_GETDENTS64 => sys_getdents64(state, frame),
        SYS_OPENAT => sys_openat(state, frame),
        SYS_LINKAT => sys_linkat(state, frame),
        SYS_SYMLINKAT => sys_symlinkat(state, frame),
        SYS_READLINKAT => sys_readlinkat(state, frame),
        SYS_STATX => sys_statx(state, frame),
        SYS_MMAP => sys_mmap(state, frame),
        SYS_MUNMAP => sys_munmap(state, frame),
        SYS_MPROTECT => sys_mprotect(state, frame),
        SYS_BRK => sys_brk(state, frame),
        SYS_PIPE2 => sys_pipe2(state, frame),
        SYS_DUP3 => sys_dup3(state, frame),
        SYS_FCNTL => sys_fcntl(state, frame),
        SYS_IOCTL => sys_ioctl(state, frame),
        SYS_POLL => sys_poll(state, frame),
        SYS_SELECT => sys_select(state, frame),
        SYS_EPOLL_CREATE1 => sys_epoll_create1(state, frame),
        SYS_EPOLL_CTL => sys_epoll_ctl(state, frame),
        SYS_EPOLL_WAIT => sys_epoll_wait(state, frame),
        SYS_SOCKET => sys_socket(state, frame),
        SYS_CONNECT => sys_connect(state, frame),
        SYS_ACCEPT => sys_accept(state, frame),
        SYS_SENDTO => sys_sendto(state, frame),
        SYS_RECVFROM => sys_recvfrom(state, frame),
        SYS_SHUTDOWN => sys_shutdown(state, frame),
        SYS_BIND => sys_bind(state, frame),
        SYS_LISTEN => sys_listen(state, frame),
        SYS_GETSOCKNAME => sys_getsockname(state, frame),
        SYS_GETPEERNAME => sys_getpeername(state, frame),
        SYS_SOCKETPAIR => sys_socketpair(state, frame),
        SYS_SETSOCKOPT => sys_setsockopt(state, frame),
        SYS_GETSOCKOPT => sys_getsockopt(state, frame),
        SYS_NANOSLEEP => sys_nanosleep(state, frame),
        SYS_CLOCK_NANOSLEEP => sys_clock_nanosleep(state, frame),
        SYS_CLOCK_GETTIME => sys_clock_gettime(state, frame),
        SYS_GETTIMEOFDAY => sys_gettimeofday(state, frame),
        SYS_TIME => sys_time(state, frame),
        SYS_GETRANDOM => sys_getrandom(state, frame),
        SYS_ARCH_PRCTL => sys_arch_prctl(state, frame),
        SYS_UNAME => sys_uname(state, frame),
        SYS_SYSINFO => sys_sysinfo(state, frame),
        SYS_GETCPU => sys_getcpu(state, frame),
        SYS_BATCH => sys_batch(state, frame),
        SYS_ASYNC_SUBMIT => sys_async_submit(state, frame),
        SYS_ASYNC_REAP => sys_async_reap(state, frame),
        SYS_SELF_HEARTBEAT => {
            frame.rax = USER_HEARTBEAT
                .fetch_add(1, Ordering::Relaxed)
                .wrapping_add(1);
            frame
        }
        SYS_SELF_GETPID => {
            frame.rax = state.threads[state.current].id as u64;
            frame
        }
        SYS_SELF_SLEEP => block_current_for(state, frame, frame.rdi.clamp(1, 1_000)),
        SYS_SELF_FORK => {
            frame.rax = spawn_from_current(state, frame, false)
                .unwrap_or_else(|error| error.return_value());
            frame
        }
        SYS_SELF_EXIT => exit_current(state, frame, frame.rdi as i32),
        SYS_SELF_WAIT => wait_for_child(state, frame),
        SYS_SELF_CLONE => {
            frame.rax =
                spawn_from_current(state, frame, true).unwrap_or_else(|error| error.return_value());
            frame
        }
        SYS_SELF_EXEC => {
            if exec_current(state, frame).is_err() {
                set_syscall_error(frame, Errno::ENOEXEC);
            }
            frame
        }
        SYS_SELF_SIGNAL => {
            if deliver_signal_frame(state, frame, frame.rdi).is_err() {
                set_syscall_error(frame, Errno::EFAULT);
            }
            frame
        }
        SYS_SELF_SIGRETURN => {
            if restore_signal_frame(state, frame).is_err() {
                set_syscall_error(frame, Errno::EFAULT);
            }
            frame
        }
        SYS_SELF_FUTEX_WAIT => futex_wait(state, frame, frame.rdi),
        SYS_SELF_FUTEX_WAKE => {
            let key = frame.rdi;
            let mut woken = 0u64;
            let _ = SharedTaskTable::new().futex_wake(key, MAX_THREADS);
            let mut ready = 0u16;
            for (index, thread) in state.threads.iter_mut().enumerate() {
                if thread.state == ThreadState::FutexWait(key) {
                    thread.state = ThreadState::Runnable;
                    let _ = SharedTaskTable::new().set_state(thread.id, TaskState::Ready);
                    ready |= 1u16 << index;
                    woken += 1;
                }
            }
            for index in 0..MAX_THREADS {
                if ready & (1u16 << index) != 0 {
                    let _ = state.enqueue(0, index);
                }
            }
            frame.rax = woken;
            FUTEX_WAKEUPS.fetch_add(woken, Ordering::Relaxed);
            frame
        }
        SYS_SELF_NICE => {
            let nice = (frame.rdi as u32 as i32).clamp(-20, 19) as i8;
            state.threads[state.current].nice = nice;
            let _ = SharedTaskTable::new().set_nice(state.threads[state.current].id, nice);
            POLICY_UPDATES.fetch_add(1, Ordering::Relaxed);
            frame.rax = 0;
            frame
        }
        SYS_SELF_SCHEDULER => {
            let priority = frame.rsi as u8;
            let policy = match frame.rdi {
                0 if priority == 0 => Some(ThreadPolicy::Normal),
                1 if (1..=99).contains(&priority) => Some(ThreadPolicy::Fifo),
                2 if (1..=99).contains(&priority) => Some(ThreadPolicy::RoundRobin),
                _ => None,
            };
            if let Some(policy) = policy {
                state.threads[state.current].policy = policy;
                state.threads[state.current].rt_priority = priority;
                let task_policy = match policy {
                    ThreadPolicy::Normal => super::task::SchedulingPolicy::Normal,
                    ThreadPolicy::Fifo => super::task::SchedulingPolicy::Fifo,
                    ThreadPolicy::RoundRobin => super::task::SchedulingPolicy::RoundRobin,
                };
                let _ = SharedTaskTable::new().set_scheduler(
                    state.threads[state.current].id,
                    task_policy,
                    priority,
                );
                POLICY_UPDATES.fetch_add(1, Ordering::Relaxed);
                frame.rax = 0;
            } else {
                set_syscall_error(frame, Errno::EINVAL);
            }
            frame
        }
        SYS_SELF_AFFINITY => {
            let affinity = frame.rdi & 1;
            if affinity == 0 {
                set_syscall_error(frame, Errno::EINVAL);
            } else {
                state.threads[state.current].affinity = affinity;
                let _ = SharedTaskTable::new().set_affinity(
                    state.threads[state.current].id,
                    affinity,
                    ONLINE_CPUS.load(Ordering::Acquire),
                );
                POLICY_UPDATES.fetch_add(1, Ordering::Relaxed);
                frame.rax = affinity;
            }
            frame
        }
        0x100f => ipc_receive(state, frame),
        0x1010 => ipc_send(state, frame),
        17 => {
            let current = state.current;
            if state.threads[current].ipc_delivered {
                state.threads[current].ipc_delivered = false;
                IPC_USER_DELIVERIES.fetch_add(1, Ordering::Relaxed);
                frame.rax = 0;
            } else {
                set_syscall_error(frame, Errno::EAGAIN);
            }
            frame
        }
        SYS_IPC_ABI_INFO => {
            frame.rax = IPC_ABI_VERSION;
            frame.rdx = MAX_PAYLOAD_BYTES as u64;
            frame.rcx = QUEUE_DEPTH as u64;
            frame.r10 = MAX_LOAN_PAGES as u64;
            frame.r8 = MAX_ENDPOINTS as u64;
            frame.r9 = MAX_SERVICES as u64;
            frame
        }
        SYS_ENDPOINT_CREATE => endpoint_create(state, frame),
        SYS_SERVICE_LOOKUP => service_lookup(state, frame),
        SYS_IPC_CALL_SEND => ipc_call_send(state, frame),
        SYS_IPC_RECEIVE => ipc_receive(state, frame),
        SYS_IPC_REPLY => ipc_reply(state, frame),
        SYS_IPC_LOAN_SEND => ipc_loan_send(state, frame),
        SYS_IPC_LOAN_MAP => ipc_loan_map(state, frame),
        _ => {
            set_syscall_error(frame, Errno::ENOSYS);
            frame
        }
    }
}

fn set_syscall_error(frame: &mut RegisterFrame, error: Errno) {
    if !INITIALIZED.load(Ordering::Acquire) || error != Errno::ENOSYS {
        console_write("[err] sc=");
        let mut hex = [0u8; 8];
        let original = frame.rax;
        for (position, offset) in (0..8).rev().enumerate() {
            let digit = ((original >> (offset * 4)) & 0xf) as u8;
            hex[position] = if digit < 10 { b'0' + digit } else { b'a' + digit - 10 };
        }
        console_write(core::str::from_utf8(&hex).unwrap_or("?"));
        console_write(" fd=");
        let mut fd_hex = [0u8; 4];
        for (position, offset) in (0..4).rev().enumerate() {
            let digit = ((frame.rdi >> (offset * 4)) & 0xf) as u8;
            fd_hex[position] = if digit < 10 { b'0' + digit } else { b'a' + digit - 10 };
        }
        console_write(core::str::from_utf8(&fd_hex).unwrap_or("?"));
        console_write(" e=");
        console_write(error.message());
        console_write("\n");
    }
    frame.rax = error.return_value();
}

fn sys_batch(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let count = match usize::try_from(frame.rsi) {
        Ok(count) if count <= MAX_BATCH_SYSCALLS => count,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    if count == 0 {
        frame.rax = 0;
        return frame;
    }
    let byte_length = count * core::mem::size_of::<BatchSyscall>();
    let cr3 = state.threads[state.current].cr3;
    if validate_user_buffer(cr3, frame.rdi, byte_length, UserAccess::Write).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    let mut calls = [BatchSyscall::empty(); MAX_BATCH_SYSCALLS];
    let bytes =
        unsafe { core::slice::from_raw_parts_mut(calls.as_mut_ptr() as *mut u8, byte_length) };
    if copy_from_user(cr3, frame.rdi, bytes).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if calls[..count]
        .iter()
        .any(|call| !batch_syscall_supported(call.number))
    {
        return syscall_failed(frame, Errno::ENOSYS);
    }
    for call in &mut calls[..count] {
        call.result = batch_result(state, call.number, &call.arguments).unwrap_or(0);
    }
    let bytes = unsafe { core::slice::from_raw_parts(calls.as_ptr() as *const u8, byte_length) };
    if copy_to_user(cr3, frame.rdi, bytes).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    frame.rax = count as u64;
    frame
}

fn batch_syscall_supported(number: u64) -> bool {
    matches!(
        number,
        SYS_GETPID
            | SYS_SELF_GETPID
            | SYS_SET_TID_ADDRESS
            | SYS_GETUID
            | SYS_GETEUID
            | SYS_GETGID
            | SYS_GETEGID
            | SYS_GETCPU
            | SYS_SELF_HEARTBEAT
    )
}

fn batch_result(state: &SchedulerState, number: u64, _arguments: &[u64; 6]) -> Option<u64> {
    let thread = state.threads[state.current];
    match number {
        SYS_GETPID | SYS_SELF_GETPID | SYS_SET_TID_ADDRESS => Some(u64::from(thread.id)),
        SYS_GETUID | SYS_GETEUID => Some(u64::from(thread.uid)),
        SYS_GETGID | SYS_GETEGID => Some(u64::from(thread.gid)),
        SYS_GETCPU => Some(crate::arch::current_cpu_id() as u64),
        SYS_SELF_HEARTBEAT => Some(
            USER_HEARTBEAT
                .fetch_add(1, Ordering::Relaxed)
                .wrapping_add(1),
        ),
        _ => None,
    }
}

fn sys_async_submit(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let cr3 = state.threads[current].cr3;
    let mut submission = AsyncIoSubmission {
        operation: 0,
        fd: 0,
        buffer: 0,
        length: 0,
        offset: 0,
    };
    let bytes = unsafe {
        core::slice::from_raw_parts_mut(
            &mut submission as *mut AsyncIoSubmission as *mut u8,
            core::mem::size_of::<AsyncIoSubmission>(),
        )
    };
    if copy_from_user(cr3, frame.rdi, bytes).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if submission.operation > 1 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let length = match user_length(submission.length, ASYNC_IO_BYTES) {
        Ok(length) => length,
        Err(error) => return syscall_failed(frame, error),
    };
    let fd = match fd_index(submission.fd) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let FdTarget::File(index) = state.threads[current].fds.entries[fd].target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let description = state.open_files[usize::from(index)];
    if !description.used {
        return syscall_failed(frame, Errno::EBADF);
    }
    if description.directory {
        return syscall_failed(frame, Errno::EISDIR);
    }
    if submission.operation == 0 && description.access_mode == O_WRONLY as u8
        || submission.operation == 1 && description.access_mode == 0
    {
        return syscall_failed(frame, Errno::EBADF);
    }
    let slot_index = match state
        .async_io
        .iter()
        .position(|slot| slot.state == AsyncIoState::Free)
    {
        Some(index) => index,
        None => return syscall_failed(frame, Errno::EAGAIN),
    };
    let mut slot = AsyncIoSlot::empty();
    slot.state = AsyncIoState::Pending;
    slot.owner = state.threads[current].id;
    slot.token = state.next_async_token.max(1);
    state.next_async_token = state.next_async_token.wrapping_add(1).max(1);
    slot.operation = submission.operation as u8;
    slot.path = description.path;
    slot.offset = submission.offset;
    slot.length = length;
    if slot.operation == 1
        && length != 0
        && copy_from_user(cr3, submission.buffer, &mut slot.buffer[..length]).is_err()
    {
        return syscall_failed(frame, Errno::EFAULT);
    }
    let token = slot.token;
    state.async_io[slot_index] = slot;
    frame.rax = token;
    frame
}

fn advance_async_io(state: &mut SchedulerState) {
    let Some(index) = state
        .async_io
        .iter()
        .position(|slot| slot.state == AsyncIoState::Pending)
    else {
        return;
    };
    let slot = &mut state.async_io[index];
    let result = if slot.operation == 0 {
        VfsService::new().read_at(
            slot.path.as_str(),
            slot.offset,
            &mut slot.buffer[..slot.length],
        )
    } else {
        VfsService::new().write_at(slot.path.as_str(), slot.offset, &slot.buffer[..slot.length])
    };
    match result {
        Ok(length) => {
            slot.result = length;
            slot.state = AsyncIoState::Complete;
        }
        Err(error) => slot.state = AsyncIoState::Failed(error),
    }
}

fn sys_async_reap(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let owner = state.threads[current].id;
    let Some(index) = state
        .async_io
        .iter()
        .position(|slot| slot.state != AsyncIoState::Free && slot.token == frame.rdi)
    else {
        return syscall_failed(frame, Errno::ENOENT);
    };
    let slot = state.async_io[index];
    if slot.owner != owner {
        return syscall_failed(frame, Errno::EPERM);
    }
    match slot.state {
        AsyncIoState::Pending => syscall_failed(frame, Errno::EAGAIN),
        AsyncIoState::Failed(error) => {
            state.async_io[index] = AsyncIoSlot::empty();
            syscall_failed(frame, error)
        }
        AsyncIoState::Complete => {
            if slot.operation == 0 && slot.result != 0 {
                let capacity = match user_length(frame.rdx, ASYNC_IO_BYTES) {
                    Ok(capacity) => capacity,
                    Err(error) => return syscall_failed(frame, error),
                };
                if capacity < slot.result {
                    return syscall_failed(frame, Errno::ENOSPC);
                }
                if copy_to_user(
                    state.threads[current].cr3,
                    frame.rsi,
                    &slot.buffer[..slot.result],
                )
                .is_err()
                {
                    return syscall_failed(frame, Errno::EFAULT);
                }
            }
            state.async_io[index] = AsyncIoSlot::empty();
            frame.rax = slot.result as u64;
            frame
        }
        AsyncIoState::Free => syscall_failed(frame, Errno::ENOENT),
    }
}

fn sys_arch_prctl(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    match frame.rdi {
        ARCH_SET_FS => {
            if frame.rsi >= USER_ADDRESS_LIMIT {
                return syscall_failed(frame, Errno::EPERM);
            }
            state.threads[current].fs_base = frame.rsi;
            set_fs_base(frame.rsi);
            frame.rax = 0;
            frame
        }
        ARCH_GET_FS => {
            let value = state.threads[current].fs_base.to_ne_bytes();
            if let Err(error) = copy_to_user(state.threads[current].cr3, frame.rsi, &value) {
                return syscall_failed(frame, error);
            }
            frame.rax = 0;
            frame
        }
        _ => syscall_failed(frame, Errno::EINVAL),
    }
}

fn ipc_errno(error: IpcError) -> Errno {
    match error {
        IpcError::InvalidOwner
        | IpcError::InvalidEndpoint
        | IpcError::InvalidCapability
        | IpcError::InvalidReplyToken
        | IpcError::InvalidPageLoan => Errno::EBADF,
        IpcError::PermissionDenied => Errno::EACCES,
        IpcError::InvalidRights | IpcError::ProtocolViolation | IpcError::InvalidPageRange => {
            Errno::EINVAL
        }
        IpcError::PayloadTooLarge => Errno::EMSGSIZE,
        IpcError::QueueFull | IpcError::QueueEmpty => Errno::EAGAIN,
        IpcError::EndpointTableFull
        | IpcError::CapabilityTableFull
        | IpcError::ReplyTokenTableFull
        | IpcError::PageLoanTableFull => Errno::ENOSPC,
        IpcError::MessageIdExhausted => Errno::EOVERFLOW,
    }
}

const AT_EMPTY_PATH: u32 = 0x1000;
const AT_STATX_ALLOWED: u32 = 0x7900;
const O_ACCMODE: u32 = 0x3;
const O_WRONLY: u32 = 0x1;
const O_RDWR: u32 = 0x2;
const O_CREAT: u32 = 0x40;
const O_EXCL: u32 = 0x80;
const O_TRUNC: u32 = 0x200;
const O_APPEND: u32 = 0x400;
const O_LARGEFILE: u32 = 0x8000;
const O_DIRECTORY: u32 = 0x1_0000;
const O_CLOEXEC: u32 = 0x8_0000;
const OPEN_FLAGS_ALLOWED: u32 =
    O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_LARGEFILE | O_DIRECTORY | O_CLOEXEC;
const SEEK_SET: u64 = 0;
const SEEK_CUR: u64 = 1;
const SEEK_END: u64 = 2;
const STATX_BASIC_STATS: u32 = 0x07ff;
const S_IFREG: u16 = 0o100000;
const S_IFDIR: u16 = 0o040000;
const S_IFCHR: u16 = 0o020000;
const DT_DIR: u8 = 4;
const DT_REG: u8 = 8;
const DT_LNK: u8 = 10;

fn sys_open(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    frame.r10 = frame.rdx;
    frame.rdx = frame.rsi;
    frame.rsi = frame.rdi;
    frame.rdi = AT_FDCWD as i64 as u64;
    sys_openat(state, frame)
}

fn sys_readv(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    sys_iov(state, frame, false)
}

fn sys_writev(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    sys_iov(state, frame, true)
}

fn sys_iov(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    write: bool,
) -> *mut RegisterFrame {
    let count = match usize::try_from(frame.rdx) {
        Ok(count) if count <= 8 => count,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let fd = frame.rdi;
    let vector = frame.rsi;
    let cr3 = state.threads[state.current].cr3;
    let mut total = 0u64;
    for index in 0..count {
        let mut entry = [0u8; 16];
        let address = match checked_user_offset(vector, index * 16) {
            Ok(address) => address,
            Err(_) => return syscall_failed(frame, Errno::EFAULT),
        };
        if let Err(error) = copy_from_user(cr3, address, &mut entry) {
            return syscall_failed(frame, error);
        }
        let base = u64::from_ne_bytes(entry[..8].try_into().unwrap());
        let length = u64::from_ne_bytes(entry[8..].try_into().unwrap());
        if length == 0 {
            continue;
        }
        frame.rdi = fd;
        frame.rsi = base;
        frame.rdx = length;
        if write {
            sys_write(state, frame);
        } else {
            sys_read(state, frame);
        }
        if frame.rax as i64 <= 0 {
            if total == 0 {
                return frame;
            }
            break;
        }
        total = match total.checked_add(frame.rax) {
            Some(total) => total,
            None => return syscall_failed(frame, Errno::EOVERFLOW),
        };
        if frame.rax < length {
            break;
        }
    }
    frame.rax = total;
    frame
}

fn sys_fstat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let entry = state.threads[state.current].fds.entries[fd];
    let (mode, size, inode, links, uid, gid) = match entry.target {
        FdTarget::ConsoleInput | FdTarget::ConsoleOutput => (S_IFCHR | 0o666, 0, 1, 1, 0, 0),
        FdTarget::File(index) => {
            let description = state.open_files[usize::from(index)];
            let metadata = match VfsService::new().metadata(description.path.as_str()) {
                Ok(metadata) => metadata,
                Err(error) => return syscall_failed(frame, error),
            };
            (
                metadata.stat_mode(),
                metadata.size,
                metadata.inode,
                metadata.links,
                metadata.uid,
                metadata.gid,
            )
        }
        _ => (S_IFREG | 0o600, 0, fd as u64 + 1, 1, 0, 0),
    };
    copy_stat_result(state, frame, frame.rsi, mode, size, inode, links, uid, gid)
}

fn sys_newfstatat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if frame.r10 & !u64::from(AT_EMPTY_PATH) != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let current = state.current;
    let mut user_path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rsi, &mut user_path) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    let result = if user_path.is_empty() && frame.r10 & u64::from(AT_EMPTY_PATH) != 0 {
        path_from_fd(state, current, frame.rdi as i32, &mut path)
    } else {
        resolve_at_path(
            state,
            current,
            frame.rdi as i32,
            user_path.as_str(),
            &mut path,
        )
    };
    if let Err(error) = result {
        return syscall_failed(frame, error);
    }
    let metadata = match VfsService::new().metadata(path.as_str()) {
        Ok(metadata) => metadata,
        Err(error) => return syscall_failed(frame, error),
    };
    copy_stat_result(
        state,
        frame,
        frame.rdx,
        metadata.stat_mode(),
        metadata.size,
        metadata.inode,
        metadata.links,
        metadata.uid,
        metadata.gid,
    )
}

fn copy_stat_result(
    state: &SchedulerState,
    frame: &mut RegisterFrame,
    address: u64,
    mode: u16,
    size: u64,
    inode: u64,
    links: u32,
    uid: u32,
    gid: u32,
) -> *mut RegisterFrame {
    let mut output = [0u8; 144];
    put_u64(&mut output, 0, 1);
    put_u64(&mut output, 8, inode);
    put_u64(&mut output, 16, u64::from(links));
    put_u32(&mut output, 24, u32::from(mode));
    put_u32(&mut output, 28, uid);
    put_u32(&mut output, 32, gid);
    put_u64(&mut output, 48, size);
    put_u64(&mut output, 56, 4096);
    put_u64(&mut output, 64, size.div_ceil(512));
    if let Err(error) = copy_to_user(state.threads[state.current].cr3, address, &output) {
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn sys_rt_sigaction(state: &SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if frame.r10 != 8 || !(1..=64).contains(&frame.rdi) {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let cr3 = state.threads[state.current].cr3;
    if frame.rsi != 0 && validate_user_buffer(cr3, frame.rsi, 32, UserAccess::Read).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if frame.rdx != 0 && copy_to_user(cr3, frame.rdx, &[0; 32]).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    frame.rax = 0;
    frame
}

fn sys_rt_sigprocmask(state: &SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if frame.r10 != 8 || frame.rdi > 2 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let cr3 = state.threads[state.current].cr3;
    if frame.rsi != 0 && validate_user_buffer(cr3, frame.rsi, 8, UserAccess::Read).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if frame.rdx != 0 && copy_to_user(cr3, frame.rdx, &[0; 8]).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    frame.rax = 0;
    frame
}

fn sys_prctl(state: &SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let cr3 = state.threads[state.current].cr3;
    match frame.rdi {
        15 => {
            if validate_user_buffer(cr3, frame.rsi, 16, UserAccess::Read).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
        }
        16 => {
            let mut name = [0u8; 16];
            name[..7].copy_from_slice(b"busybox");
            if copy_to_user(cr3, frame.rsi, &name).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
        }
        _ => return syscall_failed(frame, Errno::EINVAL),
    }
    frame.rax = 0;
    frame
}

fn sys_getcwd(state: &SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let cwd = state.threads[state.current].cwd.as_str().as_bytes();
    let required = cwd.len() + 1;
    if frame.rsi < required as u64 {
        return syscall_failed(frame, Errno::ERANGE);
    }
    let mut output = [0u8; MAX_VFS_PATH + 1];
    output[..cwd.len()].copy_from_slice(cwd);
    if let Err(error) = copy_to_user(
        state.threads[state.current].cr3,
        frame.rdi,
        &output[..required],
    ) {
        return syscall_failed(frame, error);
    }
    frame.rax = required as u64;
    frame
}

fn sys_chdir(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut input = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdi, &mut input) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(state, current, AT_FDCWD, input.as_str(), &mut path) {
        return syscall_failed(frame, error);
    }
    if !VfsService::new().is_directory(path.as_str()) {
        return syscall_failed(frame, Errno::ENOTDIR);
    }
    state.threads[current].cwd = path;
    frame.rax = 0;
    frame
}

fn sys_chroot(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut input = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdi, &mut input) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(state, current, AT_FDCWD, input.as_str(), &mut path) {
        return syscall_failed(frame, error);
    }
    if !VfsService::new().is_directory(path.as_str()) {
        return syscall_failed(frame, Errno::ENOTDIR);
    }
    state.threads[current].root = path;
    let cwd = state.threads[current].cwd.as_str();
    let root = state.threads[current].root.as_str();
    let cwd_in_root = root == "/"
        || cwd == root
        || cwd
            .strip_prefix(root)
            .is_some_and(|suffix| suffix.starts_with('/'));
    if !cwd_in_root {
        state.threads[current].cwd = state.threads[current].root;
    }
    frame.rax = 0;
    frame
}

fn sys_chmod(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut input = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdi, &mut input) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(state, current, AT_FDCWD, input.as_str(), &mut path) {
        return syscall_failed(frame, error);
    }
    if state.threads[current].uid != 0 {
        return syscall_failed(frame, Errno::EPERM);
    }
    match VfsService::new().chmod(path.as_str(), frame.rsi as u16) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_fchmod(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = path_from_fd(state, current, frame.rdi as i32, &mut path) {
        return syscall_failed(frame, error);
    }
    if state.threads[current].uid != 0 {
        return syscall_failed(frame, Errno::EPERM);
    }
    match VfsService::new().chmod(path.as_str(), frame.rsi as u16) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_chown(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut input = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdi, &mut input) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(state, current, AT_FDCWD, input.as_str(), &mut path) {
        return syscall_failed(frame, error);
    }
    if state.threads[current].uid != 0 {
        return syscall_failed(frame, Errno::EPERM);
    }
    match VfsService::new().chown(path.as_str(), frame.rsi as u32, frame.rdx as u32) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_fchown(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = path_from_fd(state, current, frame.rdi as i32, &mut path) {
        return syscall_failed(frame, error);
    }
    if state.threads[current].uid != 0 {
        return syscall_failed(frame, Errno::EPERM);
    }
    match VfsService::new().chown(path.as_str(), frame.rsi as u32, frame.rdx as u32) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_linkat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut old = InlineString::<MAX_VFS_PATH>::new();
    let mut new = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rsi, &mut old) {
        return syscall_failed(frame, error);
    }
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.r10, &mut new) {
        return syscall_failed(frame, error);
    }
    let mut old_path = InlineString::<MAX_VFS_PATH>::new();
    let mut new_path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(
        state,
        current,
        frame.rdi as i32,
        old.as_str(),
        &mut old_path,
    ) {
        return syscall_failed(frame, error);
    }
    if let Err(error) = resolve_at_path_nofollow(
        state,
        current,
        frame.rdx as i32,
        new.as_str(),
        &mut new_path,
    ) {
        return syscall_failed(frame, error);
    }
    match VfsService::new().hard_link(old_path.as_str(), new_path.as_str()) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_symlinkat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let mut target = InlineString::<MAX_VFS_PATH>::new();
    let mut link = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdi, &mut target) {
        return syscall_failed(frame, error);
    }
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rdx, &mut link) {
        return syscall_failed(frame, error);
    }
    let mut link_path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path_nofollow(
        state,
        current,
        frame.rsi as i32,
        link.as_str(),
        &mut link_path,
    ) {
        return syscall_failed(frame, error);
    }
    match VfsService::new().symlink(target.as_str(), link_path.as_str()) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_readlinkat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let capacity = match user_length(frame.rdx, MAX_VFS_PATH) {
        Ok(capacity) => capacity,
        Err(error) => return syscall_failed(frame, error),
    };
    let mut input = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(state.threads[current].cr3, frame.rsi, &mut input) {
        return syscall_failed(frame, error);
    }
    let mut raw = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) =
        resolve_at_path_nofollow(state, current, frame.rdi as i32, input.as_str(), &mut raw)
    {
        return syscall_failed(frame, error);
    }
    let mut target = InlineString::<MAX_VFS_PATH>::new();
    let result = VfsService::new().read_link(raw.as_str(), &mut target);
    if let Err(error) = result {
        return syscall_failed(frame, error);
    }
    let length = target.len().min(capacity);
    if let Err(error) = copy_to_user(
        state.threads[current].cr3,
        frame.r10,
        &target.as_str().as_bytes()[..length],
    ) {
        return syscall_failed(frame, error);
    }
    frame.rax = length as u64;
    frame
}

fn sys_openat(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let thread = state.threads[current];
    let flags = match u32::try_from(frame.rdx) {
        Ok(flags) if flags & !OPEN_FLAGS_ALLOWED == 0 && flags & O_ACCMODE != O_ACCMODE => flags,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let mut user_path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(thread.cr3, frame.rsi, &mut user_path) {
        return syscall_failed(frame, error);
    }
    if user_path.is_empty() {
        return syscall_failed(frame, Errno::ENOENT);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = resolve_at_path(
        state,
        current,
        frame.rdi as i32,
        user_path.as_str(),
        &mut path,
    ) {
        return syscall_failed(frame, error);
    }
    let vfs = VfsService::new();
    let existed = vfs.path_exists(path.as_str());
    if flags & O_CREAT != 0 {
        if existed && flags & O_EXCL != 0 {
            return syscall_failed(frame, Errno::EEXIST);
        }
        if !existed {
            if let Err(error) = vfs.create_file_with(
                path.as_str(),
                frame.r10 as u16,
                thread.uid,
                thread.gid,
                thread.umask,
            ) {
                return syscall_failed(frame, error);
            }
        }
    } else if !existed {
        return syscall_failed(frame, Errno::ENOENT);
    }
    let metadata = match vfs.metadata(path.as_str()) {
        Ok(metadata) => metadata,
        Err(error) => return syscall_failed(frame, error),
    };
    if flags & O_DIRECTORY != 0 && !metadata.directory {
        return syscall_failed(frame, Errno::ENOTDIR);
    }
    if metadata.directory && flags & O_ACCMODE != 0 {
        return syscall_failed(frame, Errno::EISDIR);
    }
    let read_requested = flags & O_ACCMODE != O_WRONLY;
    let write_requested = flags & O_ACCMODE != 0;
    if read_requested && !metadata.allows(thread.uid, thread.gid, crate::services::PERM_READ)
        || write_requested && !metadata.allows(thread.uid, thread.gid, crate::services::PERM_WRITE)
    {
        return syscall_failed(frame, Errno::EACCES);
    }
    if flags & O_TRUNC != 0 {
        if flags & O_ACCMODE == 0 {
            return syscall_failed(frame, Errno::EACCES);
        }
        if let Err(error) = vfs.truncate(path.as_str()) {
            return syscall_failed(frame, error);
        }
    }
    let fd = match state.threads[current]
        .fds
        .entries
        .iter()
        .position(|entry| entry.target == FdTarget::Closed)
    {
        Some(fd) => fd,
        None => return syscall_failed(frame, Errno::EMFILE),
    };
    let description = match state.open_files.iter().position(|entry| !entry.used) {
        Some(description) => description,
        None => return syscall_failed(frame, Errno::ENFILE),
    };
    let mut open_file = OpenFileDescription::empty();
    open_file.used = true;
    open_file.references = 1;
    open_file.path = path;
    open_file.access_mode = (flags & O_ACCMODE) as u8;
    open_file.append = flags & O_APPEND != 0;
    open_file.directory = metadata.directory;
    state.open_files[description] = open_file;
    state.threads[current].fds.entries[fd] = FdEntry {
        target: FdTarget::File(description as u8),
        close_on_exec: flags & O_CLOEXEC != 0,
    };
    frame.rax = fd as u64;
    frame
}

fn sys_close(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let current = state.current;
    let entry = state.threads[current].fds.entries[fd];
    if entry.target == FdTarget::Closed {
        return syscall_failed(frame, Errno::EBADF);
    }
    if let FdTarget::File(index) = entry.target {
        let description = state.open_files[usize::from(index)];
        let owner = state.threads[state.current].id;
        release_process_locks(state, owner, description.path.as_str());
    }
    state.threads[current].fds.entries[fd] = FdEntry::CLOSED;
    release_fd_target(state, entry.target);
    frame.rax = 0;
    frame
}

fn sys_read(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let count = match user_length(frame.rdx, isize::MAX as usize) {
        Ok(count) => count,
        Err(error) => return syscall_failed(frame, error),
    };
    if count == 0 {
        frame.rax = 0;
        return frame;
    }
    let thread = state.threads[state.current];
    let target = thread.fds.entries[fd].target;
    match target {
        FdTarget::ConsoleInput => {
            let mut bytes = [0u8; 256];
            let mut length = 0;
            while length < count.min(bytes.len()) {
                let Some(byte) = crate::arch::read_console_byte() else {
                    break;
                };
                bytes[length] = byte;
                length += 1;
            }
            if length == 0 {
                return syscall_failed(frame, Errno::EAGAIN);
            }
            if copy_to_user(thread.cr3, frame.rsi, &bytes[..length]).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
            frame.rax = length as u64;
            frame
        }
        FdTarget::ConsoleOutput | FdTarget::Closed => syscall_failed(frame, Errno::EBADF),
        FdTarget::PipeRead(pipe) => {
            let pipe = &mut state.pipes[usize::from(pipe)];
            if pipe.len == 0 {
                return syscall_failed(frame, Errno::EAGAIN);
            }
            let length = count.min(pipe.len);
            let mut bytes = [0u8; PIPE_CAPACITY];
            for index in 0..length {
                bytes[index] = pipe.data[(pipe.head + index) % PIPE_CAPACITY];
            }
            if copy_to_user(thread.cr3, frame.rsi, &bytes[..length]).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
            pipe.head = (pipe.head + length) % PIPE_CAPACITY;
            pipe.len -= length;
            frame.rax = length as u64;
            frame
        }
        FdTarget::PipeWrite(_) | FdTarget::Epoll(_) => syscall_failed(frame, Errno::EBADF),
        FdTarget::Socket(index) => {
            let index = usize::from(index);
            if state.sockets[index].inet >= 0 {
                let conn = state.sockets[index].inet as usize;
                let deadline = crate::kernel::scheduler::timer_ticks() + 10000;
                loop {
                    if state.sockets[index].shutdown_read {
                        return syscall_failed(frame, Errno::EBADF);
                    }
                    let mut kernel_buf = [0u8; SOCKET_BUFFER_BYTES];
                    let n = crate::net::tcp_recv(conn, &mut kernel_buf);
                    match n {
                        Ok(0) => {
                            frame.rax = 0;
                            return frame;
                        }
                        Ok(len) => {
                            let len = len.min(count);
                            if copy_to_user(thread.cr3, frame.rsi, &kernel_buf[..len]).is_err() {
                                return syscall_failed(frame, Errno::EFAULT);
                            }
                            frame.rax = len as u64;
                            return frame;
                        }
                        Err(crate::net::NetErr::Again) => {}
                        Err(crate::net::NetErr::Reset) => {
                            return syscall_failed(frame, Errno::ECONNRESET);
                        }
                        Err(_) => return syscall_failed(frame, Errno::ENOTCONN),
                    }
                    if !crate::net::tcp_connected(conn) {
                        return syscall_failed(frame, Errno::ENOTCONN);
                    }
                    if crate::kernel::scheduler::timer_ticks() >= deadline {
                        return syscall_failed(frame, Errno::EAGAIN);
                    }
                    crate::net::pump();
                    crate::arch::delay(30_000);
                }
            }
            let length = count.min(state.sockets[index].rx_len);
            if length == 0 {
                return syscall_failed(frame, Errno::EAGAIN);
            }
            if copy_to_user(thread.cr3, frame.rsi, &state.sockets[index].rx[..length]).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
            state.sockets[index]
                .rx
                .copy_within(length..state.sockets[index].rx_len, 0);
            state.sockets[index].rx_len -= length;
            frame.rax = length as u64;
            frame
        }
        FdTarget::File(index) => {
            let index = usize::from(index);
            let description = state.open_files[index];
            if !description.used || description.access_mode == O_WRONLY as u8 {
                return syscall_failed(frame, Errno::EBADF);
            }
            if description.directory {
                return syscall_failed(frame, Errno::EISDIR);
            }
            let vfs = VfsService::new();
            let mut total = 0usize;
            let mut offset = description.offset;
            let mut buffer = [0u8; 256];
            while total < count {
                let chunk = (count - total).min(buffer.len());
                let length =
                    match vfs.read_at(description.path.as_str(), offset, &mut buffer[..chunk]) {
                        Ok(length) => length,
                        Err(error) if total == 0 => return syscall_failed(frame, error),
                        Err(_) => break,
                    };
                if length == 0 {
                    break;
                }
                let user_address = match user_address_offset(frame.rsi, total) {
                    Ok(address) => address,
                    Err(error) => return syscall_failed(frame, error),
                };
                if copy_to_user(thread.cr3, user_address, &buffer[..length]).is_err() {
                    if total == 0 {
                        return syscall_failed(frame, Errno::EFAULT);
                    }
                    break;
                }
                total += length;
                offset = offset.saturating_add(length as u64);
                if length < chunk {
                    break;
                }
            }
            state.open_files[index].offset = offset;
            frame.rax = total as u64;
            frame
        }
    }
}

fn sys_write(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let count = match user_length(frame.rdx, isize::MAX as usize) {
        Ok(count) => count,
        Err(error) => return syscall_failed(frame, error),
    };
    if count == 0 {
        frame.rax = 0;
        return frame;
    }
    let thread = state.threads[state.current];
    let target = thread.fds.entries[fd].target;
    let mut buffer = [0u8; 256];
    match target {
        FdTarget::ConsoleOutput => {
            let mut total = 0usize;
            while total < count {
                let chunk = (count - total).min(buffer.len());
                let user_address = match user_address_offset(frame.rsi, total) {
                    Ok(address) => address,
                    Err(error) => return syscall_failed(frame, error),
                };
                if copy_from_user(thread.cr3, user_address, &mut buffer[..chunk]).is_err() {
                    if total == 0 {
                        return syscall_failed(frame, Errno::EFAULT);
                    }
                    break;
                }
                write_console_bytes(&buffer[..chunk]);
                total += chunk;
            }
            frame.rax = total as u64;
            frame
        }
        FdTarget::ConsoleInput | FdTarget::Closed => syscall_failed(frame, Errno::EBADF),
        FdTarget::PipeWrite(pipe) => {
            let pipe = &mut state.pipes[usize::from(pipe)];
            let length = count.min(PIPE_CAPACITY - pipe.len);
            if length == 0 {
                return syscall_failed(frame, Errno::EAGAIN);
            }
            let mut bytes = [0u8; PIPE_CAPACITY];
            if copy_from_user(thread.cr3, frame.rsi, &mut bytes[..length]).is_err() {
                return syscall_failed(frame, Errno::EFAULT);
            }
            for index in 0..length {
                pipe.data[(pipe.head + pipe.len + index) % PIPE_CAPACITY] = bytes[index];
            }
            pipe.len += length;
            frame.rax = length as u64;
            frame
        }
        FdTarget::PipeRead(_) | FdTarget::Epoll(_) => syscall_failed(frame, Errno::EBADF),
        FdTarget::Socket(index) => {
            let index = usize::from(index);
            if state.sockets[index].inet >= 0 {
                let conn = state.sockets[index].inet as usize;
                let mut user_buf = [0u8; SOCKET_BUFFER_BYTES];
                let length = count.min(user_buf.len());
                if copy_from_user(thread.cr3, frame.rsi, &mut user_buf[..length]).is_err() {
                    return syscall_failed(frame, Errno::EFAULT);
                }
                let deadline = crate::kernel::scheduler::timer_ticks() + 5000;
                let mut total = 0usize;
                while total < length {
                    match crate::net::tcp_send(conn, &user_buf[total..length]) {
                        Ok(n) => { total += n; }
                        Err(crate::net::NetErr::TimedOut) => {
                            return syscall_failed(frame, Errno::ETIMEDOUT);
                        }
                        Err(crate::net::NetErr::Down) => {
                            return syscall_failed(frame, Errno::ENETUNREACH);
                        }
                        Err(_) => {
                            return syscall_failed(frame, Errno::ECONNRESET);
                        }
                    }
                    if crate::kernel::scheduler::timer_ticks() >= deadline {
                        return syscall_failed(frame, Errno::ETIMEDOUT);
                    }
                }
                frame.rax = length as u64;
                return frame;
            }
            match socket_send(state, index, frame.rsi, count as u64, None) {
                Ok(length) => {
                    frame.rax = length as u64;
                    frame
                }
                Err(error) => syscall_failed(frame, error),
            }
        }
        FdTarget::File(index) => {
            let index = usize::from(index);
            let description = state.open_files[index];
            if !description.used || description.access_mode == 0 {
                return syscall_failed(frame, Errno::EBADF);
            }
            if description.directory {
                return syscall_failed(frame, Errno::EISDIR);
            }
            let vfs = VfsService::new();
            let mut offset = if description.append {
                match vfs.metadata(description.path.as_str()) {
                    Ok(metadata) => metadata.size,
                    Err(error) => return syscall_failed(frame, error),
                }
            } else {
                description.offset
            };
            let mut total = 0usize;
            while total < count {
                let chunk = (count - total).min(buffer.len());
                let user_address = match user_address_offset(frame.rsi, total) {
                    Ok(address) => address,
                    Err(error) => return syscall_failed(frame, error),
                };
                if copy_from_user(thread.cr3, user_address, &mut buffer[..chunk]).is_err() {
                    if total == 0 {
                        return syscall_failed(frame, Errno::EFAULT);
                    }
                    break;
                }
                match vfs.write_at(description.path.as_str(), offset, &buffer[..chunk]) {
                    Ok(length) => {
                        total += length;
                        offset = offset.saturating_add(length as u64);
                    }
                    Err(error) if total == 0 => return syscall_failed(frame, error),
                    Err(_) => break,
                }
            }
            state.open_files[index].offset = offset;
            frame.rax = total as u64;
            frame
        }
    }
}

fn sys_lseek(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let target = state.threads[state.current].fds.entries[fd].target;
    let FdTarget::File(index) = target else {
        return syscall_failed(
            frame,
            if target == FdTarget::Closed {
                Errno::EBADF
            } else {
                Errno::ESPIPE
            },
        );
    };
    let index = usize::from(index);
    let description = state.open_files[index];
    if !description.used {
        return syscall_failed(frame, Errno::EBADF);
    }
    let base = match frame.rdx {
        SEEK_SET => 0i128,
        SEEK_CUR => i128::from(description.offset),
        SEEK_END if description.directory => {
            let mut entries = DirEntries::new();
            if let Err(error) = VfsService::new().list_dir(description.path.as_str(), &mut entries)
            {
                return syscall_failed(frame, error);
            }
            (entries.len() + 2) as i128
        }
        SEEK_END => match VfsService::new().metadata(description.path.as_str()) {
            Ok(metadata) => i128::from(metadata.size),
            Err(error) => return syscall_failed(frame, error),
        },
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let offset = base + i128::from(frame.rsi as i64);
    if !(0..=i128::from(i64::MAX)).contains(&offset) {
        return syscall_failed(frame, Errno::EINVAL);
    }
    state.open_files[index].offset = offset as u64;
    frame.rax = offset as u64;
    frame
}

fn sys_getdents64(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let capacity = match user_length(frame.rdx, 8192) {
        Ok(capacity) => capacity,
        Err(error) => return syscall_failed(frame, error),
    };
    let thread = state.threads[state.current];
    let FdTarget::File(index) = thread.fds.entries[fd].target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let index = usize::from(index);
    let description = state.open_files[index];
    if !description.used || !description.directory {
        return syscall_failed(frame, Errno::ENOTDIR);
    }
    let mut entries = DirEntries::new();
    if let Err(error) = VfsService::new().list_dir(description.path.as_str(), &mut entries) {
        return syscall_failed(frame, error);
    }
    let entry_count = entries.len() + 2;
    let mut cursor = description.offset as usize;
    let mut output = [0u8; 8192];
    let mut length = 0usize;
    while cursor < entry_count {
        let name = match cursor {
            0 => ".",
            1 => "..",
            _ => entries.get(cursor - 2).unwrap_or(""),
        };
        let record_length = (19 + name.len() + 1 + 7) & !7;
        if length + record_length > capacity {
            break;
        }
        let mut child_path = InlineString::<MAX_VFS_PATH>::new();
        let (directory, inode, symlink) = if cursor < 2 {
            (true, path_inode(description.path.as_str(), name), false)
        } else {
            let _ = child_path.push_str(description.path.as_str());
            if description.path.as_str() != "/" {
                let _ = child_path.push_byte(b'/');
            }
            let _ = child_path.push_str(name);
            match VfsService::new().lookup(child_path.as_str()) {
                Ok(dentry) => (
                    dentry.inode.kind == InodeKind::Directory,
                    dentry.inode.number,
                    dentry.inode.kind == InodeKind::Symlink,
                ),
                Err(_) => (
                    child_is_directory(description.path.as_str(), name),
                    path_inode(description.path.as_str(), name),
                    false,
                ),
            }
        };
        output[length..length + 8].copy_from_slice(&inode.to_ne_bytes());
        output[length + 8..length + 16].copy_from_slice(&((cursor + 1) as i64).to_ne_bytes());
        output[length + 16..length + 18].copy_from_slice(&(record_length as u16).to_ne_bytes());
        output[length + 18] = if directory {
            DT_DIR
        } else if symlink {
            DT_LNK
        } else {
            DT_REG
        };
        output[length + 19..length + 19 + name.len()].copy_from_slice(name.as_bytes());
        length += record_length;
        cursor += 1;
    }
    if length == 0 && cursor < entry_count {
        return syscall_failed(frame, Errno::EINVAL);
    }
    if copy_to_user(thread.cr3, frame.rsi, &output[..length]).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    state.open_files[index].offset = cursor as u64;
    frame.rax = length as u64;
    frame
}

fn sys_statx(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let flags = match u32::try_from(frame.rdx) {
        Ok(flags) if flags & !AT_STATX_ALLOWED == 0 => flags,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let current = state.current;
    let thread = state.threads[current];
    let mut user_path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(thread.cr3, frame.rsi, &mut user_path) {
        return syscall_failed(frame, error);
    }
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    let path_result = if user_path.is_empty() && flags & AT_EMPTY_PATH != 0 {
        path_from_fd(state, current, frame.rdi as i32, &mut path)
    } else if user_path.is_empty() {
        Err(Errno::ENOENT)
    } else {
        resolve_at_path(
            state,
            current,
            frame.rdi as i32,
            user_path.as_str(),
            &mut path,
        )
    };
    if let Err(error) = path_result {
        return syscall_failed(frame, error);
    }
    let metadata = match VfsService::new().metadata(path.as_str()) {
        Ok(metadata) => metadata,
        Err(error) => return syscall_failed(frame, error),
    };
    let mut statx = [0u8; 256];
    put_u32(&mut statx, 0, STATX_BASIC_STATS);
    put_u32(&mut statx, 4, 4096);
    put_u32(&mut statx, 16, metadata.links);
    put_u32(&mut statx, 20, metadata.uid);
    put_u32(&mut statx, 24, metadata.gid);
    put_u16(&mut statx, 28, metadata.stat_mode());
    put_u64(&mut statx, 32, metadata.inode);
    put_u64(&mut statx, 40, metadata.size);
    put_u64(&mut statx, 48, metadata.size.div_ceil(512));
    put_u64(&mut statx, 144, 1);
    if copy_to_user(thread.cr3, frame.r8, &statx).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    frame.rax = 0;
    frame
}

fn syscall_failed(frame: &mut RegisterFrame, error: Errno) -> *mut RegisterFrame {
    set_syscall_error(frame, error);
    frame
}

const PROT_READ: u64 = 1;
const PROT_WRITE: u64 = 2;
const PROT_EXEC: u64 = 4;
const MAP_FIXED: u64 = 0x10;
const MAP_ANONYMOUS: u64 = 0x20;
const F_DUPFD: u64 = 0;
const F_GETFD: u64 = 1;
const F_SETFD: u64 = 2;
const F_GETFL: u64 = 3;
const F_SETFL: u64 = 4;
const F_GETLK: u64 = 5;
const F_SETLK: u64 = 6;
const F_SETLKW: u64 = 7;
const F_OFD_GETLK: u64 = 36;
const F_OFD_SETLK: u64 = 37;
const F_OFD_SETLKW: u64 = 38;
const FD_CLOEXEC: u64 = 1;
const F_RDLCK: i16 = 0;
const F_WRLCK: i16 = 1;
const F_UNLCK: i16 = 2;
const EPOLL_CTL_ADD: u64 = 1;
const EPOLL_CTL_DEL: u64 = 2;
const EPOLL_CTL_MOD: u64 = 3;
const POLLIN: u16 = 1;
const POLLOUT: u16 = 4;
const MAX_FILE_LOCKS: usize = 32;

#[derive(Clone, Copy)]
struct FileLock {
    used: bool,
    owner: u32,
    path: InlineString<MAX_VFS_PATH>,
    lock_type: i16,
    start: i64,
    length: i64,
}

impl FileLock {
    const fn empty() -> Self {
        Self {
            used: false,
            owner: 0,
            path: InlineString::new(),
            lock_type: F_UNLCK,
            start: 0,
            length: 0,
        }
    }
}

fn map_flags(prot: u64) -> Result<MapFlags, Errno> {
    if prot & !(PROT_READ | PROT_WRITE | PROT_EXEC) != 0
        || prot & PROT_WRITE != 0 && prot & PROT_EXEC != 0
    {
        return Err(Errno::EINVAL);
    }
    let mut flags = MapFlags::PRESENT | MapFlags::USER;
    if prot & PROT_WRITE != 0 {
        flags |= MapFlags::WRITABLE | MapFlags::NO_EXECUTE;
    }
    if prot & PROT_EXEC == 0 {
        flags |= MapFlags::NO_EXECUTE;
    }
    Ok(flags)
}

fn sys_mmap(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let length = match usize::try_from(frame.rsi) {
        Ok(v) if v != 0 => v,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let pages = (length.saturating_add(PAGE_SIZE as usize - 1)) / PAGE_SIZE as usize;
    let flags = match map_flags(frame.rdx) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    if frame.r10 & MAP_ANONYMOUS == 0 {
        return syscall_failed(frame, Errno::ENODEV);
    }
    let current = state.current;
    let requested = frame.rdi;
    let start = if requested != 0 && frame.r10 & MAP_FIXED != 0 {
        requested & !(PAGE_SIZE - 1)
    } else {
        let mut candidate = MMAP_BASE;
        while candidate < MMAP_LIMIT
            && state.threads[current].mmap.iter().any(|r| {
                r.used
                    && candidate < r.start + r.pages as u64 * PAGE_SIZE
                    && candidate + pages as u64 * PAGE_SIZE > r.start
            })
        {
            candidate += pages as u64 * PAGE_SIZE;
        }
        candidate
    };
    if start < PAGE_SIZE
        || start
            .checked_add(pages as u64 * PAGE_SIZE)
            .is_none_or(|end| end > MMAP_LIMIT)
    {
        return syscall_failed(frame, Errno::ENOMEM);
    }
    let Some(slot) = state.threads[current].mmap.iter().position(|r| !r.used) else {
        return syscall_failed(frame, Errno::ENOMEM);
    };
    let Some(address) = VirtualAddress::new(start) else {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let mut space = match AddressSpace::from_root(state.threads[current].cr3) {
        Ok(s) => s,
        Err(_) => return syscall_failed(frame, Errno::EFAULT),
    };
    if space.allocate_range(address, pages, flags).is_err() {
        return syscall_failed(frame, Errno::ENOMEM);
    }
    state.threads[current].mmap[slot] = MmapRegion {
        used: true,
        start,
        pages,
        prot: frame.rdx,
    };
    frame.rax = start;
    frame
}

fn sys_munmap(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let start = frame.rdi & !(PAGE_SIZE - 1);
    let length = match usize::try_from(frame.rsi) {
        Ok(v) if v != 0 => v,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let pages = (length.saturating_add(PAGE_SIZE as usize - 1)) / PAGE_SIZE as usize;
    let current = state.current;
    let Some(index) = state.threads[current]
        .mmap
        .iter()
        .position(|r| r.used && r.start == start && r.pages >= pages)
    else {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let mut space = match AddressSpace::from_root(state.threads[current].cr3) {
        Ok(s) => s,
        Err(_) => return syscall_failed(frame, Errno::EFAULT),
    };
    if space
        .release_range(VirtualAddress::new(start).unwrap(), pages, true)
        .is_err()
    {
        return syscall_failed(frame, Errno::EINVAL);
    }
    if pages == state.threads[current].mmap[index].pages {
        state.threads[current].mmap[index] = MmapRegion::empty();
    } else {
        state.threads[current].mmap[index].start += pages as u64 * PAGE_SIZE;
        state.threads[current].mmap[index].pages -= pages;
    }
    frame.rax = 0;
    frame
}

fn sys_mprotect(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let start = frame.rdi & !(PAGE_SIZE - 1);
    let length = match usize::try_from(frame.rsi) {
        Ok(v) if v != 0 => v,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let pages = (length.saturating_add(PAGE_SIZE as usize - 1)) / PAGE_SIZE as usize;
    let flags = match map_flags(frame.rdx) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let current = state.current;
    let Some(region) = state.threads[current].mmap.iter().find(|r| {
        r.used
            && start >= r.start
            && start + pages as u64 * PAGE_SIZE <= r.start + r.pages as u64 * PAGE_SIZE
    }) else {
        return syscall_failed(frame, Errno::ENOMEM);
    };
    let _ = region;
    let mut space = match AddressSpace::from_root(state.threads[current].cr3) {
        Ok(s) => s,
        Err(_) => return syscall_failed(frame, Errno::EFAULT),
    };
    for i in 0..pages {
        if space
            .set_user_flags(
                VirtualAddress::new(start + i as u64 * PAGE_SIZE).unwrap(),
                flags,
            )
            .is_err()
        {
            return syscall_failed(frame, Errno::ENOMEM);
        }
    }
    state.threads[current]
        .mmap
        .iter_mut()
        .find(|r| r.used && r.start <= start && start < r.start + r.pages as u64 * PAGE_SIZE)
        .map(|r| r.prot = frame.rdx);
    frame.rax = 0;
    frame
}

fn sys_brk(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    if state.threads[current].brk == 0 {
        state.threads[current].brk = 0x0000_5000_0000_0000;
    }
    let requested = frame.rdi;
    if requested == 0 {
        frame.rax = state.threads[current].brk;
        return frame;
    }
    if requested < state.threads[current].brk {
        frame.rax = state.threads[current].brk;
        return frame;
    }
    let old = state.threads[current].brk;
    let old_page = (old + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    let new_page = (requested + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    if new_page > old_page {
        let pages = ((new_page - old_page) / PAGE_SIZE) as usize;
        let mut space = match AddressSpace::from_root(state.threads[current].cr3) {
            Ok(s) => s,
            Err(_) => return frame,
        };
        if space
            .allocate_range(
                VirtualAddress::new(old_page).unwrap(),
                pages,
                MapFlags::PRESENT | MapFlags::WRITABLE | MapFlags::USER | MapFlags::NO_EXECUTE,
            )
            .is_err()
        {
            return frame;
        }
    }
    state.threads[current].brk = requested;
    frame.rax = requested;
    frame
}

fn alloc_fd(table: &FdTable, minimum: usize) -> Option<usize> {
    (minimum..MAX_FDS).find(|fd| table.entries[*fd].target == FdTarget::Closed)
}
fn dup_fd(state: &mut SchedulerState, old: usize, new: usize, cloexec: bool) {
    let target = state.threads[state.current].fds.entries[old].target;
    state.threads[state.current].fds.entries[new] = FdEntry {
        target,
        close_on_exec: cloexec,
    };
    match target {
        FdTarget::File(i) => state.open_files[usize::from(i)].references += 1,
        FdTarget::PipeRead(i) | FdTarget::PipeWrite(i) => state.pipes[usize::from(i)].refs += 1,
        FdTarget::Epoll(i) => state.epolls[usize::from(i)].refs += 1,
        FdTarget::Socket(i) => state.sockets[usize::from(i)].refs += 1,
        _ => {}
    }
}

fn sys_pipe2(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let Some(pipe) = state.pipes.iter().position(|p| !p.used) else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let Some(r) = alloc_fd(&state.threads[current].fds, 0) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    let Some(w) = alloc_fd(&state.threads[current].fds, r + 1) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    if frame.rsi & !0x80800 != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    };
    state.pipes[pipe] = Pipe {
        used: true,
        refs: 2,
        ..Pipe::empty()
    };
    state.threads[current].fds.entries[r] = FdEntry {
        target: FdTarget::PipeRead(pipe as u8),
        close_on_exec: frame.rsi & 0x80000 != 0,
    };
    state.threads[current].fds.entries[w] = FdEntry {
        target: FdTarget::PipeWrite(pipe as u8),
        close_on_exec: frame.rsi & 0x80000 != 0,
    };
    let bytes = [r as u32, w as u32];
    if copy_to_user(state.threads[current].cr3, frame.rdi, unsafe {
        core::slice::from_raw_parts(bytes.as_ptr() as *const u8, 8)
    })
    .is_err()
    {
        return syscall_failed(frame, Errno::EFAULT);
    }
    frame.rax = 0;
    frame
}
fn sys_dup3(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let old = match fd_index(frame.rdi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let new = match fd_index(frame.rsi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    if old == new {
        return syscall_failed(frame, Errno::EINVAL);
    };
    if frame.rdx & !0x80000 != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let current = state.current;
    if state.threads[current].fds.entries[old].target == FdTarget::Closed {
        return syscall_failed(frame, Errno::EBADF);
    };
    if state.threads[current].fds.entries[new].target != FdTarget::Closed {
        let t = state.threads[current].fds.entries[new].target;
        state.threads[current].fds.entries[new] = FdEntry::CLOSED;
        release_fd_target(state, t);
    }
    dup_fd(state, old, new, frame.rdx & 0x80000 != 0);
    frame.rax = new as u64;
    frame
}
fn sys_fcntl(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let current = state.current;
    let entry = state.threads[current].fds.entries[fd];
    if entry.target == FdTarget::Closed {
        return syscall_failed(frame, Errno::EBADF);
    };
    match frame.rsi {
        F_GETFD => {
            frame.rax = if entry.close_on_exec { FD_CLOEXEC } else { 0 };
            frame
        }
        F_SETFD => {
            state.threads[current].fds.entries[fd].close_on_exec = frame.rdx & FD_CLOEXEC != 0;
            frame.rax = 0;
            frame
        }
        F_GETFL => {
            frame.rax = match entry.target {
                FdTarget::File(i) => state.open_files[usize::from(i)].access_mode as u64,
                FdTarget::PipeRead(_) => 0,
                FdTarget::PipeWrite(_) => 1,
                _ => 0,
            };
            frame
        }
        F_DUPFD => {
            let Some(new) = alloc_fd(&state.threads[current].fds, frame.rdx as usize) else {
                return syscall_failed(frame, Errno::EMFILE);
            };
            dup_fd(state, fd, new, false);
            frame.rax = new as u64;
            frame
        }
        F_GETLK | F_SETLK | F_SETLKW | F_OFD_GETLK | F_OFD_SETLK | F_OFD_SETLKW => {
            sys_file_lock(state, frame, fd)
        }
        _ => syscall_failed(frame, Errno::EINVAL),
    }
}

fn sys_file_lock(state: &mut SchedulerState, frame: &mut RegisterFrame, fd: usize) -> *mut RegisterFrame {
    let target = state.threads[state.current].fds.entries[fd].target;
    let FdTarget::File(index) = target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let description = state.open_files[usize::from(index)];
    if !description.used || description.directory {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let cr3 = state.threads[state.current].cr3;
    let mut input = [0u8; 32];
    if copy_from_user(cr3, frame.rdx, &mut input).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    let lock_type = i16::from_ne_bytes([input[0], input[1]]);
    let whence = i16::from_ne_bytes([input[2], input[3]]);
    if !matches!(lock_type, F_RDLCK | F_WRLCK | F_UNLCK) || !matches!(whence, 0 | 1 | 2) {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let raw_start = i64::from_ne_bytes(input[8..16].try_into().unwrap());
    let length = i64::from_ne_bytes(input[16..24].try_into().unwrap());
    let base = match whence {
        0 => 0,
        1 => description.offset as i64,
        2 => match VfsService::new().metadata(description.path.as_str()) {
            Ok(metadata) => metadata.size as i64,
            Err(error) => return syscall_failed(frame, error),
        },
        _ => unreachable!(),
    };
    let start = base.checked_add(raw_start).filter(|start| *start >= 0);
    let Some(start) = start else {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let end = if length == 0 {
        i64::MAX
    } else {
        match start.checked_add(length) {
            Some(end) if end > start => end,
            _ => return syscall_failed(frame, Errno::EINVAL),
        }
    };
    let owner = state.threads[state.current].id;
    let conflict = state.file_locks.iter().find(|lock| {
        lock.used
            && lock.path.as_str() == description.path.as_str()
            && lock.owner != owner
            && lock.lock_type != F_UNLCK
            && lock_ranges_overlap(start, end, lock.start, lock_end(**lock))
            && (lock_type == F_WRLCK || lock.lock_type == F_WRLCK)
    });
    if matches!(frame.rsi, F_GETLK | F_OFD_GETLK) {
        let mut output = [0u8; 32];
        let reported_type = conflict.map_or(F_UNLCK, |lock| lock.lock_type);
        output[0..2].copy_from_slice(&reported_type.to_ne_bytes());
        if let Some(lock) = conflict {
            output[2..4].copy_from_slice(&0i16.to_ne_bytes());
            output[8..16].copy_from_slice(&lock.start.to_ne_bytes());
            let reported_length = if lock_end(*lock) == i64::MAX {
                0
            } else {
                lock.length
            };
            output[16..24].copy_from_slice(&reported_length.to_ne_bytes());
            output[24..28].copy_from_slice(&(lock.owner as i32).to_ne_bytes());
        }
        if copy_to_user(cr3, frame.rdx, &output).is_err() {
            return syscall_failed(frame, Errno::EFAULT);
        }
        frame.rax = 0;
        return frame;
    }
    if conflict.is_some() {
        return syscall_failed(frame, Errno::EAGAIN);
    }
    for lock in &mut state.file_locks {
        if lock.used
            && lock.owner == owner
            && lock.path.as_str() == description.path.as_str()
            && lock_ranges_overlap(start, end, lock.start, lock_end(*lock))
        {
            *lock = FileLock::empty();
        }
    }
    if lock_type != F_UNLCK {
        let Some(slot) = state.file_locks.iter_mut().find(|lock| !lock.used) else {
            return syscall_failed(frame, Errno::ENOLCK);
        };
        slot.used = true;
        slot.owner = owner;
        slot.path = description.path;
        slot.lock_type = lock_type;
        slot.start = start;
        slot.length = length;
    }
    frame.rax = 0;
    frame
}

fn sys_flock(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    const LOCK_SH: u64 = 1;
    const LOCK_EX: u64 = 2;
    const LOCK_NB: u64 = 4;
    const LOCK_UN: u64 = 8;
    if frame.rsi & !(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN) != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let FdTarget::File(index) = state.threads[state.current].fds.entries[fd].target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let description = state.open_files[usize::from(index)];
    let owner = state.threads[state.current].id;
    let lock_type = if frame.rsi & LOCK_UN != 0 {
        F_UNLCK
    } else if frame.rsi & LOCK_EX != 0 {
        F_WRLCK
    } else if frame.rsi & LOCK_SH != 0 {
        F_RDLCK
    } else {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let conflict = state.file_locks.iter().any(|lock| {
        lock.used
            && lock.owner != owner
            && lock.path.as_str() == description.path.as_str()
            && (lock_type == F_WRLCK || lock.lock_type == F_WRLCK)
    });
    if conflict {
        return syscall_failed(frame, Errno::EAGAIN);
    }
    release_process_locks(state, owner, description.path.as_str());
    if lock_type != F_UNLCK {
        let Some(slot) = state.file_locks.iter_mut().find(|lock| !lock.used) else {
            return syscall_failed(frame, Errno::ENOLCK);
        };
        slot.used = true;
        slot.owner = owner;
        slot.path = description.path;
        slot.lock_type = lock_type;
        slot.start = 0;
        slot.length = 0;
    }
    frame.rax = 0;
    frame
}

fn lock_end(lock: FileLock) -> i64 {
    if lock.length == 0 {
        i64::MAX
    } else {
        lock.start.saturating_add(lock.length)
    }
}

fn lock_ranges_overlap(first_start: i64, first_end: i64, second_start: i64, second_end: i64) -> bool {
    first_start < second_end && second_start < first_end
}

fn release_process_locks(state: &mut SchedulerState, owner: u32, path: &str) {
    for lock in &mut state.file_locks {
        if lock.used && lock.owner == owner && (path.is_empty() || lock.path.as_str() == path) {
            *lock = FileLock::empty();
        }
    }
}

fn sys_fsync(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(fd) => fd,
        Err(error) => return syscall_failed(frame, error),
    };
    let FdTarget::File(index) = state.threads[state.current].fds.entries[fd].target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let description = state.open_files[usize::from(index)];
    if !description.used {
        return syscall_failed(frame, Errno::EBADF);
    }
    match VfsService::new().fsync_path(description.path.as_str()) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_sync(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let _ = state;
    VfsService::new().sync_all();
    frame.rax = 0;
    frame
}
fn sys_ioctl(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let fd = match fd_index(frame.rdi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    if state.threads[state.current].fds.entries[fd].target == FdTarget::Closed {
        return syscall_failed(frame, Errno::EBADF);
    };
    if frame.rsi == 0x5413 {
        let ws = [25u16, 80, 0, 0];
        let bytes = unsafe { core::slice::from_raw_parts(ws.as_ptr() as *const u8, 8) };
        if copy_to_user(state.threads[state.current].cr3, frame.rdx, bytes).is_err() {
            return syscall_failed(frame, Errno::EFAULT);
        }
        frame.rax = 0;
        frame
    } else {
        syscall_failed(frame, Errno::ENOTTY)
    }
}

fn fd_ready(state: &SchedulerState, target: FdTarget, events: u16) -> u16 {
    match target {
        FdTarget::ConsoleInput => events & POLLIN,
        FdTarget::ConsoleOutput => events & POLLOUT,
        FdTarget::PipeRead(i) => {
            if state.pipes[usize::from(i)].len != 0 {
                events & POLLIN
            } else {
                0
            }
        }
        FdTarget::PipeWrite(i) => {
            if state.pipes[usize::from(i)].len < PIPE_CAPACITY {
                events & POLLOUT
            } else {
                0
            }
        }
        FdTarget::File(i) => {
            if state.open_files[usize::from(i)].directory {
                events & POLLIN
            } else {
                events & (POLLIN | POLLOUT)
            }
        }
        FdTarget::Socket(i) => {
            let socket = state.sockets[usize::from(i)];
            let mut ready = 0;
            if socket.inet >= 0 {
                if crate::net::tcp_readable(socket.inet as usize) {
                    ready |= events & POLLIN;
                }
                if crate::net::tcp_connected(socket.inet as usize) {
                    ready |= events & POLLOUT;
                }
            } else {
                if socket.rx_len != 0 {
                    ready |= events & POLLIN;
                }
                if !socket.shutdown_write && socket.rx_len < SOCKET_BUFFER_BYTES {
                    ready |= events & POLLOUT;
                }
            }
            ready
        }
        _ => 0,
    }
}
fn sys_poll(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let nfds = match usize::try_from(frame.rsi) {
        Ok(v) if v <= MAX_FDS => v,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let current = state.current;
    let mut ready = 0u64;
    for i in 0..nfds {
        let mut p = [0u8; 8];
        let address = match user_address_offset(frame.rdi, i * 8) {
            Ok(address) => address,
            Err(error) => return syscall_failed(frame, error),
        };
        if copy_from_user(state.threads[current].cr3, address, &mut p).is_err() {
            return syscall_failed(frame, Errno::EFAULT);
        }
        let fd = i32::from_ne_bytes(p[0..4].try_into().unwrap());
        let events = u16::from_ne_bytes(p[4..6].try_into().unwrap());
        let revents = if fd < 0 {
            0
        } else {
            match fd_index(fd as u64) {
                Ok(n) => fd_ready(state, state.threads[current].fds.entries[n].target, events),
                Err(_) => 0,
            }
        };
        p[6..8].copy_from_slice(&revents.to_ne_bytes());
        if copy_to_user(state.threads[current].cr3, address, &p).is_err() {
            return syscall_failed(frame, Errno::EFAULT);
        }
        if revents != 0 {
            ready += 1;
        }
    }
    frame.rax = ready;
    frame
}
fn sys_select(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let nfds = match usize::try_from(frame.rdi) {
        Ok(v) if v <= MAX_FDS => v,
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let current = state.current;
    let mut in_set = [0u8; 2];
    let mut out_set = [0u8; 2];
    if frame.rsi != 0 && copy_from_user(state.threads[current].cr3, frame.rsi, &mut in_set).is_err()
    {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if frame.rdx != 0
        && copy_from_user(state.threads[current].cr3, frame.rdx, &mut out_set).is_err()
    {
        return syscall_failed(frame, Errno::EFAULT);
    }
    let mut ready = 0u64;
    for fd in 0..nfds {
        let byte = fd / 8;
        let bit = 1u8 << (fd % 8);
        let target = state.threads[current].fds.entries[fd].target;
        let r = fd_ready(state, target, POLLIN);
        let w = fd_ready(state, target, POLLOUT);
        if in_set[byte] & bit != 0 && r == 0 {
            in_set[byte] &= !bit
        } else if in_set[byte] & bit != 0 {
            ready += 1;
        }
        if out_set[byte] & bit != 0 && w == 0 {
            out_set[byte] &= !bit
        } else if out_set[byte] & bit != 0 {
            ready += 1;
        }
    }
    if frame.rsi != 0 {
        let _ = copy_to_user(state.threads[current].cr3, frame.rsi, &in_set);
    }
    if frame.rdx != 0 {
        let _ = copy_to_user(state.threads[current].cr3, frame.rdx, &out_set);
    }
    frame.rax = ready;
    frame
}
fn sys_epoll_create1(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if frame.rdi & !0x80000 != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let current = state.current;
    let Some(index) = state.epolls.iter().position(|e| !e.used) else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let Some(fd) = alloc_fd(&state.threads[current].fds, 0) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    state.epolls[index] = Epoll {
        used: true,
        refs: 1,
        ..Epoll::empty()
    };
    state.threads[current].fds.entries[fd] = FdEntry {
        target: FdTarget::Epoll(index as u8),
        close_on_exec: frame.rdi & 0x80000 != 0,
    };
    frame.rax = fd as u64;
    frame
}
fn sys_epoll_ctl(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let epfd = match fd_index(frame.rdi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let fd = match fd_index(frame.rdx) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let current = state.current;
    let FdTarget::Epoll(index) = state.threads[current].fds.entries[epfd].target else {
        return syscall_failed(frame, Errno::EINVAL);
    };
    let ep = &mut state.epolls[usize::from(index)];
    let mut event = [0u8; 16];
    if frame.r10 != 0 && copy_from_user(state.threads[current].cr3, frame.r10, &mut event).is_err()
    {
        return syscall_failed(frame, Errno::EFAULT);
    }
    let events = u32::from_ne_bytes(event[0..4].try_into().unwrap());
    let data = u64::from_ne_bytes(event[8..16].try_into().unwrap());
    match frame.rsi {
        EPOLL_CTL_ADD => {
            let Some(w) = ep.watches.iter_mut().find(|w| !w.used) else {
                return syscall_failed(frame, Errno::ENOSPC);
            };
            *w = EpollWatch {
                used: true,
                fd: fd as i32,
                events,
                data,
            };
        }
        EPOLL_CTL_DEL | EPOLL_CTL_MOD => {
            let Some(w) = ep.watches.iter_mut().find(|w| w.used && w.fd == fd as i32) else {
                return syscall_failed(frame, Errno::ENOENT);
            };
            if frame.rsi == EPOLL_CTL_DEL {
                *w = EpollWatch::empty()
            } else {
                w.events = events;
                w.data = data;
            }
        }
        _ => return syscall_failed(frame, Errno::EINVAL),
    }
    frame.rax = 0;
    frame
}
fn sys_epoll_wait(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let epfd = match fd_index(frame.rdi) {
        Ok(v) => v,
        Err(e) => return syscall_failed(frame, e),
    };
    let maxevents = match usize::try_from(frame.rdx) {
        Ok(v) if v > 0 => v.min(8),
        _ => return syscall_failed(frame, Errno::EINVAL),
    };
    let current = state.current;
    let FdTarget::Epoll(index) = state.threads[current].fds.entries[epfd].target else {
        return syscall_failed(frame, Errno::EBADF);
    };
    let ep = state.epolls[usize::from(index)];
    let mut count = 0usize;
    for watch in ep.watches.iter().filter(|w| w.used) {
        if count >= maxevents {
            break;
        }
        let fd = match fd_index(watch.fd as u64) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let revents = fd_ready(
            state,
            state.threads[current].fds.entries[fd].target,
            (watch.events & 0xffff) as u16,
        ) as u32;
        if revents == 0 {
            continue;
        }
        let mut out = [0u8; 16];
        out[0..4].copy_from_slice(&revents.to_ne_bytes());
        out[8..16].copy_from_slice(&watch.data.to_ne_bytes());
        let address = match user_address_offset(frame.rsi, count * 16) {
            Ok(address) => address,
            Err(error) => return syscall_failed(frame, error),
        };
        if copy_to_user(state.threads[current].cr3, address, &out).is_err() {
            return syscall_failed(frame, Errno::EFAULT);
        }
        count += 1;
    }
    frame.rax = count as u64;
    frame
}

const AF_UNIX: u16 = 1;
const AF_INET: u16 = 2;
const SOCK_STREAM: u16 = 1;
const SOCK_DGRAM: u16 = 2;
const SOCK_CLOEXEC: u64 = 0x80000;
const SOCK_NONBLOCK: u64 = 0x800;

fn socket_fd(state: &SchedulerState, raw: u64) -> Result<(usize, usize), Errno> {
    let fd = fd_index(raw)?;
    let FdTarget::Socket(index) = state.threads[state.current].fds.entries[fd].target else {
        return Err(Errno::ENOTSOCK);
    };
    if !state.sockets[usize::from(index)].used {
        return Err(Errno::EBADF);
    }
    Ok((fd, usize::from(index)))
}

fn copy_sockaddr_from_user(cr3: u64, address: u64, length: u64) -> Result<[u8; 16], Errno> {
    let length = user_length(length, 16)?;
    if length < 2 {
        return Err(Errno::EINVAL);
    }
    let mut sockaddr = [0u8; 16];
    copy_from_user(cr3, address, &mut sockaddr[..length])?;
    Ok(sockaddr)
}

fn copy_sockaddr_to_user(
    cr3: u64,
    address: u64,
    length_address: u64,
    sockaddr: &[u8; 16],
) -> Result<(), Errno> {
    if address == 0 && length_address == 0 {
        return Ok(());
    }
    if address == 0 || length_address == 0 {
        return Err(Errno::EFAULT);
    }
    let mut raw_length = [0u8; 4];
    copy_from_user(cr3, length_address, &mut raw_length)?;
    let length = (u32::from_ne_bytes(raw_length) as usize).min(sockaddr.len());
    copy_to_user(cr3, address, &sockaddr[..length])?;
    copy_to_user(cr3, length_address, &(sockaddr.len() as u32).to_ne_bytes())
}

fn sys_socket(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let family = frame.rdi as u16;
    let kind = (frame.rsi & 0xf) as u16;
    if !matches!(family, AF_UNIX | AF_INET) || !matches!(kind, SOCK_STREAM | SOCK_DGRAM) {
        return syscall_failed(frame, Errno::EAFNOSUPPORT);
    }
    if frame.rsi & !(0xf | SOCK_CLOEXEC | SOCK_NONBLOCK) != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let current = state.current;
    let Some(index) = state.sockets.iter().position(|socket| !socket.used) else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let Some(fd) = alloc_fd(&state.threads[current].fds, 0) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    state.sockets[index] = Socket {
        used: true,
        refs: 1,
        family,
        kind,
        protocol: frame.rdx as u16,
        ..Socket::empty()
    };
    state.threads[current].fds.entries[fd] = FdEntry {
        target: FdTarget::Socket(index as u8),
        close_on_exec: frame.rsi & SOCK_CLOEXEC != 0,
    };
    frame.rax = fd as u64;
    frame
}

fn sys_bind(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let cr3 = state.threads[state.current].cr3;
    let address = match copy_sockaddr_from_user(cr3, frame.rsi, frame.rdx) {
        Ok(address) => address,
        Err(error) => return syscall_failed(frame, error),
    };
    if state
        .sockets
        .iter()
        .enumerate()
        .any(|(other, socket)| other != index && socket.used && socket.local == address)
    {
        return syscall_failed(frame, Errno::EADDRINUSE);
    }
    state.sockets[index].local = address;
    frame.rax = 0;
    frame
}

fn sys_listen(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    if state.sockets[index].kind != SOCK_STREAM {
        return syscall_failed(frame, Errno::EOPNOTSUPP);
    }
    state.sockets[index].listening = true;
    frame.rax = 0;
    frame
}

fn sys_connect(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, client) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let cr3 = state.threads[state.current].cr3;
    let address = match copy_sockaddr_from_user(cr3, frame.rsi, frame.rdx) {
        Ok(address) => address,
        Err(error) => return syscall_failed(frame, error),
    };
    let family = state.sockets[client].family;
    let kind = state.sockets[client].kind;

    if family == AF_INET && kind == SOCK_STREAM {
        let dst_ip = [address[4], address[5], address[6], address[7]];
        let dst_port = u16::from_be_bytes([address[2], address[3]]);
        match crate::net::tcp_connect(dst_ip, dst_port, 5000) {
            Ok(conn) => {
                state.sockets[client].inet = conn as i8;
                state.sockets[client].remote = address;
                frame.rax = 0;
                frame
            }
            Err(crate::net::NetErr::Refused) => syscall_failed(frame, Errno::ECONNREFUSED),
            Err(crate::net::NetErr::TimedOut) => syscall_failed(frame, Errno::ETIMEDOUT),
            Err(crate::net::NetErr::Down) => syscall_failed(frame, Errno::ENETUNREACH),
            Err(_) => syscall_failed(frame, Errno::ECONNREFUSED),
        }
    } else {
        let Some(server) = state.sockets.iter().position(|socket| {
            socket.used
                && (kind == SOCK_DGRAM || socket.listening)
                && socket.pending < 0
                && socket.family == family
                && socket.kind == kind
                && socket.local == address
        }) else {
            return syscall_failed(frame, Errno::ECONNREFUSED);
        };
        state.sockets[client].remote = address;
        if kind == SOCK_DGRAM {
            state.sockets[client].peer = server as i8;
            state.sockets[server].peer = client as i8;
        } else {
            state.sockets[server].pending = client as i8;
        }
        frame.rax = 0;
        frame
    }
}

fn sys_accept(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, listener) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let pending = state.sockets[listener].pending;
    if pending < 0 {
        return syscall_failed(frame, Errno::EAGAIN);
    }
    let client = pending as usize;
    let Some(accepted) = state.sockets.iter().position(|socket| !socket.used) else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let current = state.current;
    let Some(fd) = alloc_fd(&state.threads[current].fds, 0) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    let listener_socket = state.sockets[listener];
    let client_address = state.sockets[client].local;
    state.sockets[accepted] = Socket {
        used: true,
        refs: 1,
        family: listener_socket.family,
        kind: listener_socket.kind,
        protocol: listener_socket.protocol,
        peer: client as i8,
        local: listener_socket.local,
        remote: client_address,
        ..Socket::empty()
    };
    state.sockets[client].peer = accepted as i8;
    state.sockets[listener].pending = -1;
    state.threads[current].fds.entries[fd] = FdEntry {
        target: FdTarget::Socket(accepted as u8),
        close_on_exec: false,
    };
    if let Err(error) = copy_sockaddr_to_user(
        state.threads[current].cr3,
        frame.rsi,
        frame.rdx,
        &client_address,
    ) {
        state.threads[current].fds.entries[fd] = FdEntry::CLOSED;
        state.sockets[accepted] = Socket::empty();
        return syscall_failed(frame, error);
    }
    frame.rax = fd as u64;
    frame
}

fn sys_socketpair(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let family = frame.rdi as u16;
    let kind = (frame.rsi & 0xf) as u16;
    if family != AF_UNIX || !matches!(kind, SOCK_STREAM | SOCK_DGRAM) {
        return syscall_failed(frame, Errno::EOPNOTSUPP);
    }
    let current = state.current;
    let Some(first) = state.sockets.iter().position(|socket| !socket.used) else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let Some(second) = state
        .sockets
        .iter()
        .enumerate()
        .find(|(index, socket)| *index != first && !socket.used)
        .map(|(index, _)| index)
    else {
        return syscall_failed(frame, Errno::ENFILE);
    };
    let Some(first_fd) = alloc_fd(&state.threads[current].fds, 0) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    let Some(second_fd) = alloc_fd(&state.threads[current].fds, first_fd + 1) else {
        return syscall_failed(frame, Errno::EMFILE);
    };
    state.sockets[first] = Socket {
        used: true,
        refs: 1,
        family,
        kind,
        protocol: frame.rdx as u16,
        peer: second as i8,
        ..Socket::empty()
    };
    state.sockets[second] = Socket {
        used: true,
        refs: 1,
        family,
        kind,
        protocol: frame.rdx as u16,
        peer: first as i8,
        ..Socket::empty()
    };
    let close_on_exec = frame.rsi & SOCK_CLOEXEC != 0;
    state.threads[current].fds.entries[first_fd] = FdEntry {
        target: FdTarget::Socket(first as u8),
        close_on_exec,
    };
    state.threads[current].fds.entries[second_fd] = FdEntry {
        target: FdTarget::Socket(second as u8),
        close_on_exec,
    };
    let descriptors = [first_fd as u32, second_fd as u32];
    let bytes = unsafe {
        core::slice::from_raw_parts(descriptors.as_ptr() as *const u8, descriptors.len() * 4)
    };
    if let Err(error) = copy_to_user(state.threads[current].cr3, frame.r10, bytes) {
        state.threads[current].fds.entries[first_fd] = FdEntry::CLOSED;
        state.threads[current].fds.entries[second_fd] = FdEntry::CLOSED;
        state.sockets[first] = Socket::empty();
        state.sockets[second] = Socket::empty();
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn socket_send(
    state: &mut SchedulerState,
    index: usize,
    user_address: u64,
    length: u64,
    destination: Option<[u8; 16]>,
) -> Result<usize, Errno> {
    if state.sockets[index].shutdown_write {
        return Err(Errno::EPIPE);
    }
    let length = user_length(length, SOCKET_BUFFER_BYTES)?;
    let target = if let Some(address) = destination {
        state
            .sockets
            .iter()
            .position(|socket| socket.used && socket.local == address)
            .ok_or(Errno::ECONNREFUSED)?
    } else if state.sockets[index].peer >= 0 {
        state.sockets[index].peer as usize
    } else {
        return Err(Errno::ENOTCONN);
    };
    let available = SOCKET_BUFFER_BYTES - state.sockets[target].rx_len;
    if available == 0 {
        return Err(Errno::EAGAIN);
    }
    let length = length.min(available);
    let mut bytes = [0u8; SOCKET_BUFFER_BYTES];
    copy_from_user(
        state.threads[state.current].cr3,
        user_address,
        &mut bytes[..length],
    )?;
    let offset = state.sockets[target].rx_len;
    state.sockets[target].rx[offset..offset + length].copy_from_slice(&bytes[..length]);
    state.sockets[target].rx_len += length;
    state.sockets[target].remote = state.sockets[index].local;
    Ok(length)
}

fn sys_sendto(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    if state.sockets[index].inet >= 0 {
        let conn = state.sockets[index].inet as usize;
        let mut user_buf = [0u8; SOCKET_BUFFER_BYTES];
        let length = match user_length(frame.rdx, SOCKET_BUFFER_BYTES) {
            Ok(l) => l,
            Err(e) => return syscall_failed(frame, e),
        };
        if copy_from_user(
            state.threads[state.current].cr3,
            frame.rsi,
            &mut user_buf[..length],
        )
        .is_err()
        {
            return syscall_failed(frame, Errno::EFAULT);
        }
        let deadline = crate::kernel::scheduler::timer_ticks() + 5000;
        let mut total = 0usize;
        while total < length {
            match crate::net::tcp_send(conn, &user_buf[total..length]) {
                Ok(n) => {
                    total += n;
                }
                Err(crate::net::NetErr::TimedOut) => {
                    return syscall_failed(frame, Errno::ETIMEDOUT);
                }
                Err(_) => {
                    return syscall_failed(frame, Errno::ECONNRESET);
                }
            }
            if crate::kernel::scheduler::timer_ticks() >= deadline {
                return syscall_failed(frame, Errno::ETIMEDOUT);
            }
        }
        frame.rax = length as u64;
        return frame;
    }
    let destination = if frame.r8 == 0 {
        None
    } else {
        match copy_sockaddr_from_user(state.threads[state.current].cr3, frame.r8, frame.r9) {
            Ok(address) => Some(address),
            Err(error) => return syscall_failed(frame, error),
        }
    };
    match socket_send(state, index, frame.rsi, frame.rdx, destination) {
        Ok(length) => {
            frame.rax = length as u64;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_recvfrom(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    if state.sockets[index].shutdown_read {
        frame.rax = 0;
        return frame;
    }
    if state.sockets[index].inet >= 0 {
        let conn = state.sockets[index].inet as usize;
        let deadline = crate::kernel::scheduler::timer_ticks() + 10000;
        let cr3 = state.threads[state.current].cr3;
        loop {
            let mut kernel_buf = [0u8; SOCKET_BUFFER_BYTES];
            match crate::net::tcp_recv(conn, &mut kernel_buf) {
                Ok(0) => {
                    frame.rax = 0;
                    return frame;
                }
                Ok(len) => {
                    let max = match user_length(frame.rdx, SOCKET_BUFFER_BYTES) {
                        Ok(l) => l,
                        Err(e) => return syscall_failed(frame, e),
                    };
                    let take = len.min(max);
                    if copy_to_user(cr3, frame.rsi, &kernel_buf[..take]).is_err() {
                        return syscall_failed(frame, Errno::EFAULT);
                    }
                    let remote = state.sockets[index].remote;
                    if frame.r8 != 0 {
                        let _ = copy_sockaddr_to_user(cr3, frame.r8, frame.r9, &remote);
                    }
                    frame.rax = take as u64;
                    return frame;
                }
                Err(crate::net::NetErr::Again) => {}
                Err(crate::net::NetErr::Reset) => {
                    return syscall_failed(frame, Errno::ECONNRESET);
                }
                Err(_) => return syscall_failed(frame, Errno::ENOTCONN),
            }
            if !crate::net::tcp_connected(conn) {
                return syscall_failed(frame, Errno::ENOTCONN);
            }
            if crate::kernel::scheduler::timer_ticks() >= deadline {
                return syscall_failed(frame, Errno::EAGAIN);
            }
            crate::net::pump();
            crate::arch::delay(30_000);
        }
    }
    let length = match user_length(frame.rdx, SOCKET_BUFFER_BYTES) {
        Ok(length) => length.min(state.sockets[index].rx_len),
        Err(error) => return syscall_failed(frame, error),
    };
    if length == 0 {
        return syscall_failed(frame, Errno::EAGAIN);
    }
    let cr3 = state.threads[state.current].cr3;
    if let Err(error) = copy_to_user(cr3, frame.rsi, &state.sockets[index].rx[..length]) {
        return syscall_failed(frame, error);
    }
    state.sockets[index]
        .rx
        .copy_within(length..state.sockets[index].rx_len, 0);
    state.sockets[index].rx_len -= length;
    let remote = state.sockets[index].remote;
    if let Err(error) = copy_sockaddr_to_user(cr3, frame.r8, frame.r9, &remote) {
        return syscall_failed(frame, error);
    }
    frame.rax = length as u64;
    frame
}

fn sys_shutdown(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    match frame.rsi {
        0 => state.sockets[index].shutdown_read = true,
        1 => state.sockets[index].shutdown_write = true,
        2 => {
            state.sockets[index].shutdown_read = true;
            state.sockets[index].shutdown_write = true;
        }
        _ => return syscall_failed(frame, Errno::EINVAL),
    }
    frame.rax = 0;
    frame
}

fn sys_getsockname(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let address = state.sockets[index].local;
    match copy_sockaddr_to_user(
        state.threads[state.current].cr3,
        frame.rsi,
        frame.rdx,
        &address,
    ) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_getpeername(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    if state.sockets[index].peer < 0 && state.sockets[index].remote == [0; 16] {
        return syscall_failed(frame, Errno::ENOTCONN);
    }
    let address = state.sockets[index].remote;
    match copy_sockaddr_to_user(
        state.threads[state.current].cr3,
        frame.rsi,
        frame.rdx,
        &address,
    ) {
        Ok(()) => {
            frame.rax = 0;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

fn sys_setsockopt(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if let Err(error) = socket_fd(state, frame.rdi) {
        return syscall_failed(frame, error);
    }
    if frame.r8 != 0 && frame.r10 != 0 {
        let length = match user_length(frame.r8, 64) {
            Ok(length) => length,
            Err(error) => return syscall_failed(frame, error),
        };
        if let Err(error) = validate_user_buffer(
            state.threads[state.current].cr3,
            frame.r10,
            length,
            UserAccess::Read,
        ) {
            return syscall_failed(frame, error);
        }
    }
    frame.rax = 0;
    frame
}

fn sys_getsockopt(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let (_, index) = match socket_fd(state, frame.rdi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let value = match frame.rdx {
        3 => state.sockets[index].kind as u32,
        39 => state.sockets[index].family as u32,
        _ => 0,
    };
    let cr3 = state.threads[state.current].cr3;
    if let Err(error) = copy_to_user(cr3, frame.r10, &value.to_ne_bytes()) {
        return syscall_failed(frame, error);
    }
    if let Err(error) = copy_to_user(cr3, frame.r8, &4u32.to_ne_bytes()) {
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn put_i64(output: &mut [u8], offset: usize, value: i64) {
    output[offset..offset + 8].copy_from_slice(&value.to_ne_bytes());
}

fn monotonic_timespec() -> [u8; 16] {
    let nanoseconds = crate::arch::monotonic_time_ns();
    let mut output = [0u8; 16];
    put_i64(&mut output, 0, (nanoseconds / 1_000_000_000) as i64);
    put_i64(&mut output, 8, (nanoseconds % 1_000_000_000) as i64);
    output
}

fn sys_clock_gettime(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if !matches!(frame.rdi, 0 | 1 | 4 | 5 | 6 | 7) {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let timespec = monotonic_timespec();
    if let Err(error) = copy_to_user(state.threads[state.current].cr3, frame.rsi, &timespec) {
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn sys_gettimeofday(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    if frame.rdi != 0 {
        let nanoseconds = crate::arch::monotonic_time_ns();
        let mut timeval = [0u8; 16];
        put_i64(&mut timeval, 0, (nanoseconds / 1_000_000_000) as i64);
        put_i64(
            &mut timeval,
            8,
            ((nanoseconds % 1_000_000_000) / 1_000) as i64,
        );
        if let Err(error) = copy_to_user(state.threads[state.current].cr3, frame.rdi, &timeval) {
            return syscall_failed(frame, error);
        }
    }
    if frame.rsi != 0 {
        if let Err(error) = copy_to_user(state.threads[state.current].cr3, frame.rsi, &[0; 8]) {
            return syscall_failed(frame, error);
        }
    }
    frame.rax = 0;
    frame
}

fn sys_time(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let seconds = crate::arch::monotonic_time_ns() / 1_000_000_000;
    if frame.rdi != 0 {
        if let Err(error) = copy_to_user(
            state.threads[state.current].cr3,
            frame.rdi,
            &(seconds as i64).to_ne_bytes(),
        ) {
            return syscall_failed(frame, error);
        }
    }
    frame.rax = seconds;
    frame
}

fn sleep_from_timespec(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    request: u64,
    absolute: bool,
) -> *mut RegisterFrame {
    let mut timespec = [0u8; 16];
    if let Err(error) = copy_from_user(state.threads[state.current].cr3, request, &mut timespec) {
        return syscall_failed(frame, error);
    }
    let seconds = i64::from_ne_bytes(timespec[0..8].try_into().unwrap());
    let nanoseconds = i64::from_ne_bytes(timespec[8..16].try_into().unwrap());
    if seconds < 0 || !(0..1_000_000_000).contains(&nanoseconds) {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let requested = (seconds as u128)
        .saturating_mul(1_000_000_000)
        .saturating_add(nanoseconds as u128);
    let duration = if absolute {
        requested.saturating_sub(crate::arch::monotonic_time_ns() as u128)
    } else {
        requested
    };
    let ticks = duration.div_ceil(1_000_000).min(u64::MAX as u128) as u64;
    frame.rax = 0;
    if ticks == 0 {
        frame
    } else {
        block_current_for(state, frame, ticks)
    }
}

fn sys_nanosleep(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    sleep_from_timespec(state, frame, frame.rdi, false)
}

fn sys_clock_nanosleep(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
) -> *mut RegisterFrame {
    if !matches!(frame.rdi, 0 | 1 | 7) || frame.rsi & !1 != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    sleep_from_timespec(state, frame, frame.rdx, frame.rsi & 1 != 0)
}

fn next_random() -> u64 {
    let entropy = crate::arch::monotonic_time_ns()
        .rotate_left(17)
        .wrapping_add(crate::arch::current_cpu_id() as u64);
    let mut current = RANDOM_STATE.load(Ordering::Relaxed);
    loop {
        let mut next = current ^ entropy;
        next ^= next << 13;
        next ^= next >> 7;
        next ^= next << 17;
        if next == 0 {
            next = 0x9e37_79b9_7f4a_7c15;
        }
        match RANDOM_STATE.compare_exchange_weak(current, next, Ordering::AcqRel, Ordering::Relaxed)
        {
            Ok(_) => return next,
            Err(observed) => current = observed,
        }
    }
}

fn sys_getrandom(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    const GRND_RANDOM: u64 = 2;
    const GRND_INSECURE: u64 = 4;
    if frame.rdx & !7 != 0 || frame.rdx & (GRND_RANDOM | GRND_INSECURE) == 6 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let length = match user_length(frame.rsi, 1 << 20) {
        Ok(length) => length,
        Err(error) => return syscall_failed(frame, error),
    };
    if let Err(error) = validate_user_buffer(
        state.threads[state.current].cr3,
        frame.rdi,
        length,
        UserAccess::Write,
    ) {
        return syscall_failed(frame, error);
    }
    let mut written = 0usize;
    while written < length {
        let random = next_random().to_ne_bytes();
        let chunk = (length - written).min(random.len());
        if let Err(error) = copy_to_user(
            state.threads[state.current].cr3,
            match user_address_offset(frame.rdi, written) {
                Ok(address) => address,
                Err(error) => return syscall_failed(frame, error),
            },
            &random[..chunk],
        ) {
            return syscall_failed(frame, error);
        }
        written += chunk;
    }
    frame.rax = length as u64;
    frame
}

fn write_uts_field(output: &mut [u8; 390], index: usize, value: &str) {
    let start = index * 65;
    let length = value.len().min(64);
    output[start..start + length].copy_from_slice(&value.as_bytes()[..length]);
}

fn sys_uname(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let mut output = [0u8; 390];
    write_uts_field(&mut output, 0, "Rustix");
    write_uts_field(&mut output, 1, "rustix");
    write_uts_field(&mut output, 2, "0.1.0");
    write_uts_field(&mut output, 3, "microkernel");
    write_uts_field(&mut output, 4, crate::arch::arch_name());
    write_uts_field(&mut output, 5, "localdomain");
    if let Err(error) = copy_to_user(state.threads[state.current].cr3, frame.rdi, &output) {
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn sys_sysinfo(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let mut output = [0u8; 112];
    put_i64(
        &mut output,
        0,
        (crate::arch::monotonic_time_ns() / 1_000_000_000) as i64,
    );
    let total = crate::mm::with_memory_map(|map| map.usable_bytes());
    let free = crate::mm::frame::stats().free_frames as u64 * PAGE_SIZE;
    put_u64(&mut output, 32, total);
    put_u64(&mut output, 40, free);
    output[80..82].copy_from_slice(&(ACTIVE_THREADS.load(Ordering::Relaxed) as u16).to_ne_bytes());
    output[104..108].copy_from_slice(&1u32.to_ne_bytes());
    if let Err(error) = copy_to_user(state.threads[state.current].cr3, frame.rdi, &output) {
        return syscall_failed(frame, error);
    }
    frame.rax = 0;
    frame
}

fn sys_getcpu(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let cr3 = state.threads[state.current].cr3;
    if frame.rdi != 0 {
        if let Err(error) =
            copy_to_user(cr3, frame.rdi, &crate::arch::current_cpu_id().to_ne_bytes())
        {
            return syscall_failed(frame, error);
        }
    }
    if frame.rsi != 0 {
        if let Err(error) = copy_to_user(cr3, frame.rsi, &0u32.to_ne_bytes()) {
            return syscall_failed(frame, error);
        }
    }
    frame.rax = 0;
    frame
}

fn fd_index(raw: u64) -> Result<usize, Errno> {
    usize::try_from(raw)
        .ok()
        .filter(|fd| *fd < MAX_FDS)
        .ok_or(Errno::EBADF)
}

fn user_length(raw: u64, maximum: usize) -> Result<usize, Errno> {
    checked_user_length(raw, maximum).map_err(|_| Errno::EINVAL)
}

fn user_address_offset(address: u64, offset: usize) -> Result<u64, Errno> {
    checked_user_offset(address, offset).map_err(|_| Errno::EFAULT)
}

fn validate_user_buffer(
    cr3: u64,
    address: u64,
    length: usize,
    access: UserAccess,
) -> Result<(), Errno> {
    AddressSpace::from_root(cr3)
        .map_err(|_| Errno::EFAULT)?
        .validate_user_range(address, length, matches!(access, UserAccess::Write))
        .map_err(|_| Errno::EFAULT)
}

fn copy_from_user(cr3: u64, address: u64, output: &mut [u8]) -> Result<(), Errno> {
    validate_user_buffer(cr3, address, output.len(), UserAccess::Read)?;
    AddressSpace::from_root(cr3)
        .map_err(|_| Errno::EFAULT)?
        .copy_from_user(address, output)
        .map_err(|_| Errno::EFAULT)
}

fn copy_to_user(cr3: u64, address: u64, input: &[u8]) -> Result<(), Errno> {
    validate_user_buffer(cr3, address, input.len(), UserAccess::Write)?;
    AddressSpace::from_root(cr3)
        .map_err(|_| Errno::EFAULT)?
        .copy_to_user(address, input)
        .map_err(|_| Errno::EFAULT)
}

fn copy_user_c_string<const N: usize>(
    cr3: u64,
    address: u64,
    output: &mut InlineString<N>,
) -> Result<(), Errno> {
    if address == 0 {
        return Err(Errno::EFAULT);
    }
    let mut bytes = [0u8; N];
    for index in 0..bytes.len() {
        copy_from_user(
            cr3,
            user_address_offset(address, index)?,
            &mut bytes[index..index + 1],
        )?;
        if bytes[index] == 0 {
            let path = core::str::from_utf8(&bytes[..index]).map_err(|_| Errno::EILSEQ)?;
            output.push_str(path).map_err(|_| Errno::ENAMETOOLONG)?;
            return Ok(());
        }
    }
    Err(Errno::ENAMETOOLONG)
}

fn copy_user_string_vector<const N: usize, const COUNT: usize>(
    cr3: u64,
    address: u64,
) -> Result<([InlineString<N>; COUNT], usize), Errno> {
    if address == 0 {
        return Err(Errno::EFAULT);
    }
    let mut strings = [InlineString::<N>::new(); COUNT];
    for index in 0..=COUNT {
        let pointer_address = user_address_offset(address, index * core::mem::size_of::<u64>())?;
        let mut pointer = [0u8; 8];
        copy_from_user(cr3, pointer_address, &mut pointer)?;
        let pointer = u64::from_ne_bytes(pointer);
        if pointer == 0 {
            return Ok((strings, index));
        }
        if index == COUNT {
            return Err(Errno::E2BIG);
        }
        copy_user_c_string(cr3, pointer, &mut strings[index]).map_err(|error| {
            if error == Errno::ENAMETOOLONG {
                Errno::E2BIG
            } else {
                error
            }
        })?;
    }
    Err(Errno::E2BIG)
}

fn resolve_at_path_nofollow(
    state: &SchedulerState,
    thread: usize,
    dirfd: i32,
    input: &str,
    output: &mut InlineString<MAX_VFS_PATH>,
) -> Result<(), Errno> {
    let base = if dirfd == AT_FDCWD {
        state.threads[thread].cwd
    } else {
        let mut fd_path = InlineString::<MAX_VFS_PATH>::new();
        path_from_fd(state, thread, dirfd, &mut fd_path)?;
        if !VfsService::new().is_directory(fd_path.as_str()) {
            return Err(Errno::ENOTDIR);
        }
        fd_path
    };
    crate::linux::path::normalize_in_root(
        state.threads[thread].root.as_str(),
        base.as_str(),
        input,
        output,
    )
    .map(|_| ())
}

fn resolve_at_path(
    state: &SchedulerState,
    thread: usize,
    dirfd: i32,
    input: &str,
    output: &mut InlineString<MAX_VFS_PATH>,
) -> Result<(), Errno> {
    resolve_at_path_nofollow(state, thread, dirfd, input, output)?;
    let canonical = *output;
    let root = state.threads[thread].root.as_str();
    let input = if canonical.as_str() == root {
        "/"
    } else if root == "/" {
        canonical.as_str()
    } else {
        canonical
            .as_str()
            .strip_prefix(root)
            .filter(|suffix| suffix.starts_with('/'))
            .ok_or(Errno::EACCES)?
    };
    let mut resolved = InlineString::<MAX_VFS_PATH>::new();
    VfsService::new().resolve_path(root, root, input, &mut resolved)?;
    output.set(resolved.as_str());
    Ok(())
}

fn path_from_fd(
    state: &SchedulerState,
    thread: usize,
    fd: i32,
    output: &mut InlineString<MAX_VFS_PATH>,
) -> Result<(), Errno> {
    let fd = fd_index(fd as u32 as u64)?;
    let FdTarget::File(index) = state.threads[thread].fds.entries[fd].target else {
        return Err(Errno::EBADF);
    };
    let description = state.open_files[usize::from(index)];
    if !description.used {
        return Err(Errno::EBADF);
    }
    output
        .push_str(description.path.as_str())
        .map_err(|_| Errno::ENAMETOOLONG)
}

fn release_fd_target(state: &mut SchedulerState, target: FdTarget) {
    match target {
        FdTarget::File(index) => {
            let description = &mut state.open_files[usize::from(index)];
            if description.references > 1 {
                description.references -= 1;
            } else {
                *description = OpenFileDescription::empty();
            }
        }
        FdTarget::PipeRead(index) | FdTarget::PipeWrite(index) => {
            let pipe = &mut state.pipes[usize::from(index)];
            if pipe.refs > 1 {
                pipe.refs -= 1;
            } else {
                *pipe = Pipe::empty();
            }
        }
        FdTarget::Epoll(index) => {
            let epoll = &mut state.epolls[usize::from(index)];
            if epoll.refs > 1 {
                epoll.refs -= 1;
            } else {
                *epoll = Epoll::empty();
            }
        }
        FdTarget::Socket(index) => {
            let socket_index = usize::from(index);
            let peer = state.sockets[socket_index].peer;
            let inet = state.sockets[socket_index].inet;
            if state.sockets[socket_index].refs > 1 {
                state.sockets[socket_index].refs -= 1;
            } else {
                state.sockets[socket_index] = Socket::empty();
                if peer >= 0 && state.sockets[peer as usize].used {
                    state.sockets[peer as usize].peer = -1;
                }
            }
            if inet >= 0 {
                let _ = crate::net::tcp_close(inet as usize);
            }
        }
        _ => {}
    }
}

fn write_console_bytes(bytes: &[u8]) {
    let mut remaining = bytes;
    while !remaining.is_empty() {
        match core::str::from_utf8(remaining) {
            Ok(text) => {
                crate::arch::console_write(text);
                return;
            }
            Err(error) => {
                let valid = error.valid_up_to();
                if valid != 0 {
                    let text = unsafe { core::str::from_utf8_unchecked(&remaining[..valid]) };
                    crate::arch::console_write(text);
                }
                crate::arch::console_write("�");
                remaining = &remaining[valid + error.error_len().unwrap_or(1)..];
            }
        }
    }
}

fn child_is_directory(parent: &str, name: &str) -> bool {
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    let _ = path.push_str(parent);
    if parent != "/" {
        let _ = path.push_byte(b'/');
    }
    let _ = path.push_str(name);
    VfsService::new().is_directory(path.as_str())
}

fn path_inode(parent: &str, name: &str) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for byte in parent.bytes().chain(name.bytes()) {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x100_0000_01b3);
    }
    hash.max(1)
}

fn put_u16(output: &mut [u8], offset: usize, value: u16) {
    output[offset..offset + 2].copy_from_slice(&value.to_ne_bytes());
}

fn put_u32(output: &mut [u8], offset: usize, value: u32) {
    output[offset..offset + 4].copy_from_slice(&value.to_ne_bytes());
}

fn put_u64(output: &mut [u8], offset: usize, value: u64) {
    output[offset..offset + 8].copy_from_slice(&value.to_ne_bytes());
}

fn endpoint_create(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let owner = state.threads[state.current].id;
    match state.ipc.create_endpoint(owner) {
        Ok((endpoint, capability)) => {
            frame.rax = capability.raw();
            frame.rdx = endpoint.raw();
        }
        Err(error) => set_syscall_error(frame, ipc_errno(error)),
    }
    frame
}

fn service_lookup(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let Ok(service_id) = u32::try_from(frame.rdi) else {
        set_syscall_error(frame, Errno::EINVAL);
        return frame;
    };
    let Some(service) = state.services.lookup(service_id) else {
        set_syscall_error(frame, Errno::ENOENT);
        return frame;
    };
    let caller = state.threads[state.current].id;
    if SharedTaskTable::new().get_by_pid(service.pid).is_none()
        || state
            .ipc
            .capability_for(service.pid, service.endpoint, IpcRights::MANAGE)
            != Some(service.owner_capability)
    {
        set_syscall_error(frame, Errno::EIO);
        return frame;
    }
    frame.rax = match state.ipc.grant(
        service.pid,
        service.owner_capability,
        caller,
        IpcRights::SEND,
    ) {
        Ok(capability) => capability.raw(),
        Err(error) => ipc_errno(error).return_value(),
    };
    frame
}

fn ipc_call_send(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let sender = state.threads[state.current];
    let capability = CapabilityId::from_raw(frame.rdi);
    let reply_capability = CapabilityId::from_raw(frame.r8);
    if let Err(error) = state
        .ipc
        .authorize_call(sender.id, capability, reply_capability)
    {
        IPC_USER_REJECTIONS.fetch_add(1, Ordering::Relaxed);
        set_syscall_error(frame, ipc_errno(error));
        return frame;
    }
    let message = match copy_message_from_user(sender, frame) {
        Ok(message) => message,
        Err(error) => {
            set_syscall_error(frame, error);
            return frame;
        }
    };
    match state
        .ipc
        .call(sender.id, capability, reply_capability, message)
    {
        Ok(message_id) => {
            frame.rax = message_id;
            wake_ipc_receiver(state);
        }
        Err(error) => set_syscall_error(frame, ipc_errno(error)),
    }
    frame
}

fn ipc_reply(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let replier = state.threads[state.current];
    let token = ReplyTokenId::from_raw(frame.rdi);
    if let Err(error) = state.ipc.authorize_reply(replier.id, token) {
        IPC_USER_REJECTIONS.fetch_add(1, Ordering::Relaxed);
        set_syscall_error(frame, ipc_errno(error));
        return frame;
    }
    let message = match copy_message_from_user(replier, frame) {
        Ok(message) => message,
        Err(error) => {
            set_syscall_error(frame, error);
            return frame;
        }
    };
    match state.ipc.reply(replier.id, token, message) {
        Ok(message_id) => {
            frame.rax = message_id;
            wake_ipc_receiver(state);
        }
        Err(error) => set_syscall_error(frame, ipc_errno(error)),
    }
    frame
}

fn ipc_receive(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let receiver = state.threads[current];
    let capacity = match usize::try_from(frame.rdx) {
        Ok(capacity) if (1..=MAX_PAYLOAD_BYTES).contains(&capacity) => capacity,
        _ => {
            set_syscall_error(frame, Errno::EINVAL);
            return frame;
        }
    };
    let capability = CapabilityId::from_raw(frame.rdi);
    match state.ipc.receive(receiver.id, capability) {
        Ok(envelope) => {
            match copy_ipc_to_user(receiver.cr3, frame.rsi, capacity, envelope) {
                Ok(()) => {
                    set_ipc_result(frame, envelope);
                    state.threads[current].ipc_delivered = true;
                }
                Err(error) => set_syscall_error(frame, error),
            }
            frame
        }
        Err(IpcError::QueueEmpty) => {
            state.threads[current].saved_rsp = frame as *mut RegisterFrame as u64;
            state.threads[current].state = ThreadState::IpcReceive {
                capability: frame.rdi,
                address: frame.rsi,
                capacity: capacity as u16,
            };
            let _ = SharedTaskTable::new().set_state(receiver.id, TaskState::IpcWait);
            let next = state.pick_next(0);
            switch_to(state, current, next)
        }
        Err(error) => {
            set_syscall_error(frame, ipc_errno(error));
            frame
        }
    }
}

fn ipc_send(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let sender = state.threads[state.current];
    let capability = CapabilityId::from_raw(frame.rdi);
    if let Err(error) = state.ipc.authorize_send(sender.id, capability) {
        IPC_USER_REJECTIONS.fetch_add(1, Ordering::Relaxed);
        set_syscall_error(frame, ipc_errno(error));
        return frame;
    }
    let message = match copy_message_from_user(sender, frame) {
        Ok(message) => message,
        Err(error) => {
            set_syscall_error(frame, error);
            return frame;
        }
    };
    match state.ipc.send(sender.id, capability, message) {
        Ok(message_id) => {
            frame.rax = message_id;
            wake_ipc_receiver(state);
        }
        Err(error) => set_syscall_error(frame, ipc_errno(error)),
    }
    frame
}

fn ipc_loan_send(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let sender = state.threads[state.current];
    let capability = CapabilityId::from_raw(frame.rdi);
    let recipient = match state.ipc.send_target(sender.id, capability) {
        Ok(recipient) => recipient,
        Err(error) => return syscall_failed(frame, ipc_errno(error)),
    };
    let kind = match u16::try_from(frame.rcx)
        .ok()
        .and_then(RequestKind::from_opcode)
    {
        Some(kind) => kind,
        None => return syscall_failed(frame, Errno::EINVAL),
    };
    let length = match usize::try_from(frame.rdx) {
        Ok(length) => length,
        Err(_) => return syscall_failed(frame, Errno::EINVAL),
    };
    let (frames, pages) = match detach_user_pages(sender.cr3, frame.rsi, length) {
        Ok(loan) => loan,
        Err(error) => return syscall_failed(frame, error),
    };
    let loan = match state
        .page_loans
        .create(sender.id, recipient, frames, pages, length)
    {
        Ok(loan) => loan,
        Err(error) => {
            restore_user_pages(sender.cr3, frame.rsi, &frames, pages);
            return syscall_failed(frame, ipc_errno(error));
        }
    };
    let message = match Message::page_loan(kind, loan, length) {
        Ok(message) => message,
        Err(error) => {
            let _ = state.page_loans.consume(recipient, loan);
            restore_user_pages(sender.cr3, frame.rsi, &frames, pages);
            return syscall_failed(frame, ipc_errno(error));
        }
    };
    match state.ipc.send(sender.id, capability, message) {
        Ok(message_id) => {
            frame.rax = message_id;
            frame.rdx = loan.raw();
            wake_ipc_receiver(state);
        }
        Err(error) => {
            let _ = state.page_loans.consume(recipient, loan);
            restore_user_pages(sender.cr3, frame.rsi, &frames, pages);
            set_syscall_error(frame, ipc_errno(error));
        }
    }
    frame
}

fn ipc_loan_map(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let receiver = state.threads[state.current];
    let loan_id = PageLoanId::from_raw(frame.rdi);
    let destination = frame.rsi;
    if frame.rdx & !1 != 0 || destination & (PAGE_SIZE - 1) != 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let loan = match state.page_loans.get(receiver.id, loan_id) {
        Ok(loan) => loan,
        Err(error) => return syscall_failed(frame, ipc_errno(error)),
    };
    if map_page_loan(receiver.cr3, destination, loan, frame.rdx & 1 != 0).is_err() {
        return syscall_failed(frame, Errno::EFAULT);
    }
    if state.page_loans.consume(receiver.id, loan_id).is_err() {
        unmap_page_loan(receiver.cr3, destination, loan.pages as usize);
        return syscall_failed(frame, Errno::EBADF);
    }
    IPC_ZERO_COPY_BYTES.fetch_add(u64::from(loan.length), Ordering::Relaxed);
    frame.rax = u64::from(loan.length);
    frame
}

fn detach_user_pages(
    cr3: u64,
    source: u64,
    length: usize,
) -> Result<([u64; MAX_LOAN_PAGES], usize), Errno> {
    if source < PAGE_SIZE || source & (PAGE_SIZE - 1) != 0 || length == 0 {
        return Err(Errno::EINVAL);
    }
    let pages = length
        .checked_add(PAGE_SIZE as usize - 1)
        .ok_or(Errno::EINVAL)?
        / PAGE_SIZE as usize;
    let end = source
        .checked_add((pages as u64).checked_mul(PAGE_SIZE).ok_or(Errno::EINVAL)?)
        .ok_or(Errno::EINVAL)?;
    if pages > MAX_LOAN_PAGES || end > USER_ADDRESS_LIMIT {
        return Err(Errno::EINVAL);
    }
    let mut space = AddressSpace::from_root(cr3).map_err(|_| Errno::EFAULT)?;
    space
        .validate_user_range(source, pages * PAGE_SIZE as usize, true)
        .map_err(|_| Errno::EFAULT)?;
    for index in 0..pages {
        let address =
            VirtualAddress::new(source + index as u64 * PAGE_SIZE).ok_or(Errno::EFAULT)?;
        if space.translate(address).is_none() {
            return Err(Errno::EFAULT);
        }
    }
    let mut frames = [0u64; MAX_LOAN_PAGES];
    for index in 0..pages {
        let address =
            VirtualAddress::new(source + index as u64 * PAGE_SIZE).ok_or(Errno::EFAULT)?;
        match space.unmap_user(address) {
            Ok(physical) => frames[index] = physical.as_u64(),
            Err(_) => {
                restore_user_pages(cr3, source, &frames, index);
                return Err(Errno::EFAULT);
            }
        }
    }
    Ok((frames, pages))
}

fn restore_user_pages(cr3: u64, start: u64, frames: &[u64; MAX_LOAN_PAGES], pages: usize) {
    let Ok(mut space) = AddressSpace::from_root(cr3) else {
        return;
    };
    let flags = MapFlags::PRESENT | MapFlags::WRITABLE | MapFlags::USER | MapFlags::NO_EXECUTE;
    for (index, raw_frame) in frames[..pages].iter().copied().enumerate() {
        let Some(address) = VirtualAddress::new(start + index as u64 * PAGE_SIZE) else {
            continue;
        };
        let Some(physical) = PhysicalAddress::new(raw_frame) else {
            continue;
        };
        let _ = space.map_user(address, physical, flags);
    }
}

fn map_page_loan(cr3: u64, destination: u64, loan: PageLoan, writable: bool) -> Result<(), ()> {
    let pages = loan.pages as usize;
    let end = destination
        .checked_add((pages as u64).checked_mul(PAGE_SIZE).ok_or(())?)
        .ok_or(())?;
    if destination < PAGE_SIZE || end > USER_ADDRESS_LIMIT {
        return Err(());
    }
    let mut space = AddressSpace::from_root(cr3).map_err(|_| ())?;
    for index in 0..pages {
        let address = VirtualAddress::new(destination + index as u64 * PAGE_SIZE).ok_or(())?;
        if space.translate(address).is_some() {
            return Err(());
        }
    }
    let mut flags = MapFlags::PRESENT | MapFlags::USER | MapFlags::NO_EXECUTE;
    if writable {
        flags |= MapFlags::WRITABLE;
    }
    for index in 0..pages {
        let address = VirtualAddress::new(destination + index as u64 * PAGE_SIZE).ok_or(())?;
        let physical = PhysicalAddress::new(loan.frame(index).ok_or(())?).ok_or(())?;
        if space.map_user(address, physical, flags).is_err() {
            unmap_page_loan(cr3, destination, index);
            return Err(());
        }
    }
    Ok(())
}

fn unmap_page_loan(cr3: u64, destination: u64, pages: usize) {
    let Ok(mut space) = AddressSpace::from_root(cr3) else {
        return;
    };
    for index in 0..pages {
        if let Some(address) = VirtualAddress::new(destination + index as u64 * PAGE_SIZE) {
            let _ = space.unmap_user(address);
        }
    }
}

fn copy_message_from_user(sender: Thread, frame: &RegisterFrame) -> Result<Message, Errno> {
    let length = usize::try_from(frame.rdx)
        .ok()
        .filter(|length| *length <= MAX_PAYLOAD_BYTES)
        .ok_or(Errno::EMSGSIZE)?;
    let kind = u16::try_from(frame.rcx)
        .ok()
        .and_then(RequestKind::from_opcode)
        .ok_or(Errno::EINVAL)?;
    let mut payload = [0u8; MAX_PAYLOAD_BYTES];
    copy_from_user(sender.cr3, frame.rsi, &mut payload[..length])?;
    Message::new(kind, &payload[..length]).map_err(ipc_errno)
}

fn wake_ipc_receiver(state: &mut SchedulerState) {
    for index in 0..state.threads.len() {
        let receiver = state.threads[index];
        let ThreadState::IpcReceive {
            capability,
            address,
            capacity,
        } = receiver.state
        else {
            continue;
        };
        let envelope = match state
            .ipc
            .receive(receiver.id, CapabilityId::from_raw(capability))
        {
            Ok(envelope) => envelope,
            Err(IpcError::QueueEmpty) => continue,
            Err(_) => {
                wake_ipc_with_error(state, index);
                continue;
            }
        };
        let frame = receiver.saved_rsp as *mut RegisterFrame;
        if frame.is_null() {
            state.threads[index].state = ThreadState::Runnable;
            let _ = SharedTaskTable::new().set_state(receiver.id, TaskState::Ready);
            let _ = state.enqueue(0, index);
            continue;
        }
        match copy_ipc_to_user(receiver.cr3, address, usize::from(capacity), envelope) {
            Ok(()) => {
                unsafe { set_ipc_result(&mut *frame, envelope) };
                state.threads[index].ipc_delivered = true;
            }
            Err(error) => unsafe { set_syscall_error(&mut *frame, error) },
        }
        state.threads[index].state = ThreadState::Runnable;
        let _ = SharedTaskTable::new().set_state(receiver.id, TaskState::Ready);
        let _ = state.enqueue(0, index);
    }
}

fn wake_ipc_with_error(state: &mut SchedulerState, index: usize) {
    let frame = state.threads[index].saved_rsp as *mut RegisterFrame;
    if !frame.is_null() {
        unsafe { set_syscall_error(&mut *frame, Errno::EIO) };
    }
    state.threads[index].state = ThreadState::Runnable;
    let _ = SharedTaskTable::new().set_state(state.threads[index].id, TaskState::Ready);
    let _ = state.enqueue(0, index);
}

fn copy_ipc_to_user(
    cr3: u64,
    address: u64,
    capacity: usize,
    envelope: crate::ipc::Envelope,
) -> Result<(), Errno> {
    if envelope.message.loan() != PageLoanId::NONE {
        return Ok(());
    }
    let payload = envelope.message.payload();
    if payload.len() > capacity {
        return Err(Errno::EMSGSIZE);
    }
    copy_to_user(cr3, address, payload)
}

fn set_ipc_result(frame: &mut RegisterFrame, envelope: crate::ipc::Envelope) {
    frame.rax = envelope.message.transferred_len() as u64;
    frame.rdx = u64::from(envelope.sender);
    frame.rcx = u64::from(envelope.message.opcode());
    frame.r10 = envelope.message.loan().raw();
    frame.r8 = envelope.id;
    frame.r9 = envelope.reply_token.raw();
}

fn futex_wait(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    key: u64,
) -> *mut RegisterFrame {
    if key == 0 || key & 3 != 0 {
        set_syscall_error(frame, Errno::EINVAL);
        return frame;
    }
    let current = state.current;
    if SharedTaskTable::new()
        .futex_wait(state.threads[current].id, key, 1, 1)
        .is_err()
    {
        set_syscall_error(frame, Errno::EAGAIN);
        return frame;
    }
    state.threads[current].saved_rsp = frame as *mut RegisterFrame as u64;
    state.threads[current].state = ThreadState::FutexWait(key);
    let next = state.pick_next(0);
    switch_to(state, current, next)
}

fn deliver_signal_frame(
    state: &SchedulerState,
    frame: &mut RegisterFrame,
    handler: u64,
) -> Result<(), ()> {
    let mut instruction = [0u8; 1];
    copy_from_user(state.threads[state.current].cr3, handler, &mut instruction).map_err(|_| ())?;
    let saved = UserSignalFrame {
        magic: SIGNAL_FRAME_MAGIC,
        rip: frame.rip,
        rsp: frame.rsp,
        rflags: frame.rflags,
        rax: 0,
    };
    let user_rsp = frame
        .rsp
        .checked_sub(core::mem::size_of::<UserSignalFrame>() as u64)
        .ok_or(())?;
    let bytes = unsafe {
        core::slice::from_raw_parts(
            &saved as *const UserSignalFrame as *const u8,
            core::mem::size_of::<UserSignalFrame>(),
        )
    };
    copy_to_user(state.threads[state.current].cr3, user_rsp, bytes).map_err(|_| ())?;
    frame.rsp = user_rsp;
    frame.rip = handler;
    frame.rdi = 10;
    Ok(())
}

fn restore_signal_frame(state: &SchedulerState, frame: &mut RegisterFrame) -> Result<(), ()> {
    let mut saved = UserSignalFrame {
        magic: 0,
        rip: 0,
        rsp: 0,
        rflags: 0,
        rax: 0,
    };
    let bytes = unsafe {
        core::slice::from_raw_parts_mut(
            &mut saved as *mut UserSignalFrame as *mut u8,
            core::mem::size_of::<UserSignalFrame>(),
        )
    };
    copy_from_user(state.threads[state.current].cr3, frame.rsp, bytes).map_err(|_| ())?;
    if saved.magic != SIGNAL_FRAME_MAGIC || saved.rip < PAGE_SIZE {
        return Err(());
    }
    frame.rip = saved.rip;
    frame.rsp = saved.rsp;
    frame.rflags = saved.rflags;
    frame.rax = saved.rax;
    SIGNAL_DELIVERIES.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

fn exec_current(state: &mut SchedulerState, frame: &mut RegisterFrame) -> Result<(), ()> {
    replace_current_image(
        state,
        frame,
        "/bin/elf-selftest",
        elf_test_program(),
        &["/bin/elf-selftest", "pie"],
        &["RUSTIX=1"],
    )
}

fn replace_current_image(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    task_name: &'static str,
    image: &'static [u8],
    arguments: &[&str],
    environment: &[&str],
) -> Result<(), ()> {
    let loaded = match elf::load_process(
        image,
        Some(("/lib/ld-rustix.so", elf_test_interpreter())),
        vdso_image(),
        arguments,
        environment,
        next_random(),
    ) {
        Ok(value) => value,
        Err(error) => {
            console_write("[dbg] load-error: ");
            let name = match error {
                elf::LoadError::Elf(_) => "elf",
                elf::LoadError::InterpreterMissing => "interp-missing",
                elf::LoadError::InterpreterMismatch => "interp-mismatch",
                elf::LoadError::InterpreterNested => "interp-nested",
                elf::LoadError::InvalidVdso => "vdso",
                elf::LoadError::InvalidSegment => "segment",
                elf::LoadError::TooManyPages => "too-many-pages",
                elf::LoadError::AddressConflict => "conflict",
                elf::LoadError::WriteExecute => "wx",
                elf::LoadError::OutOfMemory => "oom",
                elf::LoadError::InvalidArguments => "args",
                elf::LoadError::StackOverflow => "stack",
            };
            console_write(name);
            console_write("\n");
            return Err(());
        }
    };
    let current = state.current;
    let old_cr3 = state.threads[current].cr3;
    if SharedTaskTable::new()
        .execve(state.threads[current].id, task_name)
        .is_err()
    {
        let _ = AddressSpace::from_root(loaded.cr3).and_then(AddressSpace::destroy);
        return Err(());
    }
    close_cloexec_fds(state, current);
    state.threads[current].cr3 = loaded.cr3;
    state.threads[current].fs_base = 0;
    state.threads[current].owns_address_space = true;
    state.threads[current].image = image;
    frame.rip = loaded.entry;
    frame.cs = USER_CODE_SELECTOR;
    frame.rflags = INITIAL_RFLAGS;
    frame.rsp = loaded.stack_pointer;
    frame.ss = USER_DATA_SELECTOR;
    frame.rax = 0;
    set_fs_base(0);
    set_cr3(loaded.cr3);
    retire_address_space(state, old_cr3);
    reclaim_retired_spaces(state);
    EXEC_CYCLES.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

fn sys_fork(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    match spawn_from_current(state, frame, false) {
        Ok(child) => {
            frame.rax = child;
            frame
        }
        Err(error) => syscall_failed(frame, error),
    }
}

const CLONE_FS_FLAG: u64 = 0x200;
const CLONE_FILES_FLAG: u64 = 0x400;

fn sys_clone(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    // 仅支持 fork 语义的 clone；线程类 clone（共享地址空间等）暂不提供
    if frame.rdi & (CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS_FLAG | CLONE_FILES_FLAG) != 0
    {
        return syscall_failed(frame, Errno::ENOSYS);
    }
    sys_fork(state, frame)
}

fn sys_wait4(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let status_pointer = frame.rsi;
    let result = wait_for_child(state, frame);
    let return_value = unsafe { (*result).rax };
    // 内部编码：低 32 位 pid，高 32 位退出码；错误时 rax 为负 errno。
    // 注意：阻塞等待被唤醒时直接经保存帧回到用户态，本包装器不会继续执行；
    // 该路径的 status 写入由 exit_current 的唤醒逻辑完成，这里只覆盖未阻塞的立即回收路径。
    if (return_value as i64) > 0 && status_pointer != 0 {
        let code = (return_value >> 32) as u32;
        let linux_status = u32::from(code & 0xff) << 8;
        let bytes = linux_status.to_ne_bytes();
        let write_cr3 = state.threads[state.current].cr3;
        if let Err(error) = copy_to_user(write_cr3, status_pointer, &bytes) {
            return syscall_failed(unsafe { &mut *result }, error);
        }
    }
    result
}

fn sys_execve(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let cr3 = state.threads[state.current].cr3;
    let mut path = InlineString::<MAX_VFS_PATH>::new();
    if let Err(error) = copy_user_c_string(cr3, frame.rdi, &mut path) {
        return syscall_failed(frame, error);
    }
    // busybox / GNU 工具会通过 /proc/self/exe 重新执行自身
    let image = if path.as_str() == "/proc/self/exe" {
        let current_image = state.threads[state.current].image;
        if current_image.is_empty() {
            console_write("[dbg] exe-noimage\n");
            return syscall_failed(frame, Errno::ENOENT);
        }
        current_image
    } else {
        match elf_image(path.as_str()) {
            Some(image) => image,
            None => {
                console_write("[dbg] execve-enoent path=");
                console_write(path.as_str());
                console_write("\n");
                return syscall_failed(frame, Errno::ENOENT);
            }
        }
    };
    let (arguments, argument_count) = match copy_user_string_vector::<256, 8>(cr3, frame.rsi) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    let (environment, environment_count) = match copy_user_string_vector::<256, 8>(cr3, frame.rdx) {
        Ok(value) => value,
        Err(error) => return syscall_failed(frame, error),
    };
    if argument_count == 0 {
        return syscall_failed(frame, Errno::EINVAL);
    }
    let mut argument_references = [""; 8];
    let mut environment_references = [""; 8];
    for index in 0..argument_count {
        argument_references[index] = arguments[index].as_str();
    }
    for index in 0..environment_count {
        environment_references[index] = environment[index].as_str();
    }
    if replace_current_image(
        state,
        frame,
        match path.as_str() {
            "/bin/elf-selftest" => "/bin/elf-selftest",
            "/bin/static-selftest" => "/bin/static-selftest",
            "/bin/busybox" => "/bin/busybox",
            "/bin/bash" => "/bin/bash",
            "/bin/fwtest" => "/bin/fwtest",
            "/bin/curl" => "/bin/curl",
            "/bin/fastfetch" => "/bin/fastfetch",
            "/bin/ls" | "/bin/cat" | "/bin/cp" | "/bin/head" | "/bin/wc" | "/bin/true" => "coreutils",
            "/proc/self/exe" => "/proc/self/exe",
            _ => return syscall_failed(frame, Errno::ENOEXEC),
        },
        image,
        &argument_references[..argument_count],
        &environment_references[..environment_count],
    )
    .is_err()
    {
        return syscall_failed(frame, Errno::ENOEXEC);
    }
    frame
}

fn create_user_image(program: &[u8]) -> Result<(u64, u64), &'static str> {
    if program.len() > PAGE_SIZE as usize {
        return Err("user program is too large");
    }
    let mut user_space =
        AddressSpace::new_user().map_err(|_| "user address space allocation failed")?;
    let code_frame = frame::allocate().map_err(|_| "user code frame allocation failed")?;
    unsafe {
        core::ptr::write_bytes(code_frame.direct_mapped() as *mut u8, 0, PAGE_SIZE as usize);
        core::ptr::copy_nonoverlapping(
            program.as_ptr(),
            code_frame.direct_mapped() as *mut u8,
            program.len(),
        );
    }
    let user_code = VirtualAddress::new(USER_CODE_ADDRESS).ok_or("invalid user code address")?;
    user_space
        .map_user(user_code, code_frame, MapFlags::PRESENT | MapFlags::USER)
        .map_err(|_| "user code mapping failed")?;
    let user_stack = user_space
        .map_guarded_stack(USER_STACK_TOP, USER_STACK_PAGES)
        .map_err(|_| "user stack mapping failed")?;
    let user_rsp = user_stack.top - 8;
    user_space
        .copy_to_user(user_rsp, &[0; 8])
        .map_err(|_| "user stack initialization failed")?;
    Ok((user_space.root().as_u64(), user_rsp))
}

fn block_current_for(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    duration: u64,
) -> *mut RegisterFrame {
    let current = state.current;
    state.threads[current].saved_rsp = frame as *mut RegisterFrame as u64;
    let now = CPU_TICKS[0].load(Ordering::Relaxed);
    let deadline = now.saturating_add(duration);
    state.threads[current].state = ThreadState::Sleeping(deadline);
    let _ = SharedTaskTable::new()
        .set_state(state.threads[current].id, TaskState::Sleeping(0, deadline));
    let next = state.pick_next(0);
    switch_to(state, current, next)
}

fn spawn_from_current(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    share_address_space: bool,
) -> Result<u64, Errno> {
    let slot = state
        .threads
        .iter()
        .position(|thread| thread.state == ThreadState::Empty)
        .ok_or_else(|| {
            console_write("[dbg] spawn-eagain-slots\n");
            Errno::EAGAIN
        })?;
    state.threads[state.current].fpu_state.save();
    let parent = state.threads[state.current];
    let kernel_stack_top = if state.threads[slot].kernel_stack_top != 0 {
        state.threads[slot].kernel_stack_top
    } else {
        map_thread_stack(slot).map_err(|_| {
            console_write("[dbg] spawn-enomem-stack\n");
            Errno::ENOMEM
        })?
    };
    let child_cr3 = if share_address_space {
        parent.cr3
    } else {
        AddressSpace::from_root(parent.cr3)
            .and_then(|space| space.clone_user())
            .map_err(|_| Errno::ENOMEM)?
            .root()
            .as_u64()
    };
    let child_frame =
        (kernel_stack_top - core::mem::size_of::<RegisterFrame>() as u64) as *mut RegisterFrame;
    unsafe {
        core::ptr::copy_nonoverlapping(frame as *const RegisterFrame, child_frame, 1);
        (*child_frame).rax = 0;
    }

    let tasks = SharedTaskTable::new();
    let pid_result = if share_address_space {
        tasks.clone_task(
            parent.id,
            "user-thread",
            CLONE_VM | CLONE_SIGHAND | CLONE_THREAD,
        )
    } else {
        tasks.fork(parent.id)
    };
    let pid = match pid_result {
        Ok(pid) => pid,
        Err(_) => {
            if !share_address_space {
                let _ = AddressSpace::from_root(child_cr3).and_then(AddressSpace::destroy);
            }
            return Err(Errno::EAGAIN);
        }
    };
    retain_fd_table(state, parent.fds);
    state.threads[slot] = Thread {
        id: pid,
        state: ThreadState::Runnable,
        saved_rsp: child_frame as u64,
        kernel_stack_top,
        cr3: child_cr3,
        fs_base: parent.fs_base,
        fpu_state: parent.fpu_state,
        affinity: parent.affinity,
        policy: parent.policy,
        nice: parent.nice,
        rt_priority: parent.rt_priority,
        runtime_ticks: 0,
        ipc_delivered: false,
        owns_address_space: !share_address_space,
        image: parent.image,
        fds: parent.fds,
        mmap: parent.mmap,
        brk: parent.brk,
        cwd: parent.cwd,
        root: parent.root,
        uid: parent.uid,
        gid: parent.gid,
        umask: parent.umask,
    };
    let target_cpu = state.threads[slot].affinity.trailing_zeros() as usize;
    let _ = state.enqueue(target_cpu.min(MAX_CPUS - 1), slot);
    ACTIVE_THREADS.fetch_add(1, Ordering::Relaxed);
    Ok(pid as u64)
}

fn exit_current(
    state: &mut SchedulerState,
    frame: &mut RegisterFrame,
    status: i32,
) -> *mut RegisterFrame {
    let current = state.current;
    let exiting = state.threads[current];
    release_process_locks(state, exiting.id, "");
    let tasks = SharedTaskTable::new();
    let Some(exiting_task) = tasks.get_by_pid(exiting.id) else {
        set_syscall_error(frame, Errno::ESRCH);
        return frame;
    };
    let parent_pid = exiting_task.parent.unwrap_or(0);
    let detached_thread = exiting_task.exit_signal.is_none();
    if tasks.exit(exiting.id, status).is_err() {
        set_syscall_error(frame, Errno::EPERM);
        return frame;
    }
    close_all_fds(state, current);
    for slot in &mut state.async_io {
        if slot.state != AsyncIoState::Free && slot.owner == exiting.id {
            *slot = AsyncIoSlot::empty();
        }
    }
    state.page_loans.drain_subject(exiting.id, |loan| {
        for index in 0..loan.pages as usize {
            if let Some(physical) = loan.frame(index).and_then(PhysicalAddress::new) {
                let _ = frame::deallocate(physical);
            }
        }
    });
    state.ipc.remove_subject(exiting.id);
    state.services.remove_pid(exiting.id);
    state.threads[current].saved_rsp = frame as *mut RegisterFrame as u64;
    state.threads[current].state = ThreadState::Zombie(status);
    let mut reaped = false;
    let mut ready_parent = None;
    for (index, thread) in state.threads.iter_mut().enumerate() {
        if thread.id == parent_pid && thread.state == ThreadState::WaitingChild {
            thread.state = ThreadState::Runnable;
            let _ = tasks.set_state(thread.id, TaskState::Ready);
            if thread.saved_rsp != 0 {
                unsafe {
                    (*(thread.saved_rsp as *mut RegisterFrame)).rax =
                        u64::from(exiting.id) | ((status as u32 as u64) << 32);
                }
                let linux_status = ((status as u32) & 0xff) << 8;
                let bytes = linux_status.to_ne_bytes();
                let status_pointer =
                    unsafe { (*(thread.saved_rsp as *mut RegisterFrame)).rsi };
                if status_pointer != 0 {
                    let _ = copy_to_user(thread.cr3, status_pointer, &bytes);
                }
            }
            ready_parent = Some(index);
            reaped = true;
        }
    }
    if let Some(index) = ready_parent {
        let _ = state.enqueue(0, index);
    }
    if reaped || detached_thread {
        if reaped {
            let _ = tasks.wait(parent_pid, Some(exiting.id));
        }
        if exiting.owns_address_space {
            retire_address_space(state, exiting.cr3);
        }
        state.threads[current] = reusable_thread_slot(state.threads[current]);
        ACTIVE_THREADS.fetch_sub(1, Ordering::Relaxed);
        if reaped {
            PROCESS_CYCLES.fetch_add(1, Ordering::Relaxed);
        }
    }
    let next = state.pick_next(0);
    switch_to(state, current, next)
}

fn wait_for_child(state: &mut SchedulerState, frame: &mut RegisterFrame) -> *mut RegisterFrame {
    let current = state.current;
    let parent_pid = state.threads[current].id;
    let tasks = SharedTaskTable::new();
    if let Some(index) = state.threads.iter().position(|thread| {
        tasks.get_by_pid(thread.id).and_then(|task| task.parent) == Some(parent_pid)
            && matches!(thread.state, ThreadState::Zombie(_))
    }) {
        let child = state.threads[index];
        let status = match child.state {
            ThreadState::Zombie(status) => status,
            _ => 0,
        };
        let _ = tasks.wait(parent_pid, Some(child.id));
        if child.owns_address_space {
            retire_address_space(state, child.cr3);
        }
        frame.rax = u64::from(child.id) | ((status as u32 as u64) << 32);
        state.threads[index] = reusable_thread_slot(state.threads[index]);
        ACTIVE_THREADS.fetch_sub(1, Ordering::Relaxed);
        PROCESS_CYCLES.fetch_add(1, Ordering::Relaxed);
        return frame;
    }
    let has_child = state.threads.iter().any(|thread| {
        thread.state != ThreadState::Empty
            && tasks.get_by_pid(thread.id).and_then(|task| task.parent) == Some(parent_pid)
    });
    if !has_child {
        set_syscall_error(frame, Errno::ECHILD);
        return frame;
    }

    state.threads[current].saved_rsp = frame as *mut RegisterFrame as u64;
    state.threads[current].state = ThreadState::WaitingChild;
    let _ = SharedTaskTable::new().set_state(parent_pid, TaskState::ChildWait);
    let next = state.pick_next(0);
    if next == current || state.threads[next].saved_rsp == 0 {
        state.threads[current].state = ThreadState::Runnable;
        let _ = SharedTaskTable::new().set_state(parent_pid, TaskState::Ready);
        set_syscall_error(frame, Errno::EINTR);
        return frame;
    }
    switch_to(state, current, next)
}

fn reusable_thread_slot(thread: Thread) -> Thread {
    Thread {
        kernel_stack_top: thread.kernel_stack_top,
        ..Thread::empty()
    }
}

fn retain_fd_table(state: &mut SchedulerState, table: FdTable) {
    for entry in table.entries {
        match entry.target {
            FdTarget::File(index) => {
                state.open_files[usize::from(index)].references = state.open_files
                    [usize::from(index)]
                .references
                .saturating_add(1)
            }
            FdTarget::PipeRead(index) | FdTarget::PipeWrite(index) => {
                state.pipes[usize::from(index)].refs =
                    state.pipes[usize::from(index)].refs.saturating_add(1)
            }
            FdTarget::Epoll(index) => {
                state.epolls[usize::from(index)].refs =
                    state.epolls[usize::from(index)].refs.saturating_add(1)
            }
            FdTarget::Socket(index) => {
                state.sockets[usize::from(index)].refs =
                    state.sockets[usize::from(index)].refs.saturating_add(1)
            }
            _ => {}
        }
    }
}

fn close_cloexec_fds(state: &mut SchedulerState, thread: usize) {
    for fd in 0..MAX_FDS {
        let entry = state.threads[thread].fds.entries[fd];
        if entry.close_on_exec {
            state.threads[thread].fds.entries[fd] = FdEntry::CLOSED;
            release_fd_target(state, entry.target);
        }
    }
}

fn close_all_fds(state: &mut SchedulerState, thread: usize) {
    for fd in 0..MAX_FDS {
        let entry = state.threads[thread].fds.entries[fd];
        state.threads[thread].fds.entries[fd] = FdEntry::CLOSED;
        release_fd_target(state, entry.target);
    }
}

fn retire_address_space(state: &mut SchedulerState, cr3: u64) {
    if cr3 == 0 || state.retired_spaces.contains(&cr3) {
        return;
    }
    if let Some(slot) = state.retired_spaces.iter_mut().find(|entry| **entry == 0) {
        *slot = cr3;
    }
}

fn reclaim_retired_spaces(state: &mut SchedulerState) {
    let active_cr3 = current_cr3();
    for slot in &mut state.retired_spaces {
        let cr3 = *slot;
        if cr3 == 0 || cr3 == active_cr3 {
            continue;
        }
        if AddressSpace::from_root(cr3)
            .and_then(AddressSpace::destroy)
            .is_ok()
        {
            *slot = 0;
            RECLAIMED_ADDRESS_SPACES.fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn switch_to(state: &mut SchedulerState, current: usize, next: usize) -> *mut RegisterFrame {
    let previous = state.threads[current];
    if next == current || state.threads[next].saved_rsp == 0 {
        return previous.saved_rsp as *mut RegisterFrame;
    }
    state.threads[current].fpu_state.save();
    let next_thread = state.threads[next];
    state.current = next;
    let tasks = SharedTaskTable::new();
    if previous.id != 0 && previous.state == ThreadState::Runnable {
        let _ = tasks.set_state(previous.id, TaskState::Ready);
    }
    let _ = tasks.set_state(next_thread.id, TaskState::Running);
    crate::arch::set_kernel_stack_top(next_thread.kernel_stack_top);
    if next_thread.cr3 != previous.cr3 {
        set_cr3(next_thread.cr3);
    }
    set_fs_base(next_thread.fs_base);
    next_thread.fpu_state.restore();
    CONTEXT_SWITCHES.fetch_add(1, Ordering::Relaxed);
    next_thread.saved_rsp as *mut RegisterFrame
}

fn map_thread_stack(index: usize) -> Result<u64, &'static str> {
    let bottom = THREAD_STACK_BASE + index as u64 * THREAD_STACK_STRIDE;
    crate::mm::paging::map_guarded_kernel_stack(bottom, THREAD_STACK_PAGES)
        .ok_or("guarded kernel stack mapping failed")
}

fn run_queue_priority(thread: Thread) -> u8 {
    if thread.policy == ThreadPolicy::Normal {
        0
    } else {
        32 + (u16::from(thread.rt_priority) * 31 / 99) as u8
    }
}

fn disable_interrupts() -> u64 {
    let flags: u64;
    unsafe {
        asm!(
            "pushfq",
            "pop {}",
            "cli",
            out(reg) flags,
            options(nomem)
        );
    }
    flags
}

fn restore_interrupts(flags: u64) {
    if flags & (1 << 9) != 0 {
        unsafe { asm!("sti", options(nomem, nostack, preserves_flags)) };
    }
}

fn current_cr3() -> u64 {
    let cr3: u64;
    unsafe { asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack, preserves_flags)) };
    cr3
}

fn set_cr3(cr3: u64) {
    unsafe { asm!("mov cr3, {}", in(reg) cr3, options(nostack, preserves_flags)) };
}

fn set_fs_base(address: u64) {
    let low = address as u32;
    let high = (address >> 32) as u32;
    unsafe {
        asm!(
            "wrmsr",
            in("ecx") IA32_FS_BASE,
            in("eax") low,
            in("edx") high,
            options(nostack, preserves_flags)
        );
    }
}

unsafe fn build_kernel_frame(stack_top: u64, instruction_pointer: u64) -> u64 {
    const WORDS: usize = 20;
    let frame = (stack_top - WORDS as u64 * 8) as *mut u64;
    core::ptr::write_bytes(frame, 0, WORDS);
    *frame.add(15) = instruction_pointer;
    *frame.add(16) = KERNEL_CODE_SELECTOR;
    *frame.add(17) = INITIAL_RFLAGS;
    *frame.add(18) = stack_top - 8;
    *frame.add(19) = KERNEL_DATA_SELECTOR;
    frame as u64
}

unsafe fn build_user_frame_with_bootstrap(
    stack_top: u64,
    instruction_pointer: u64,
    user_rsp: u64,
    argument0: u64,
    argument1: u64,
) -> u64 {
    const WORDS: usize = 20;
    let frame = (stack_top - WORDS as u64 * 8) as *mut u64;
    core::ptr::write_bytes(frame, 0, WORDS);
    *frame.add(8) = argument0;
    *frame.add(9) = argument1;
    *frame.add(11) = IPC_ABI_VERSION;
    *frame.add(15) = instruction_pointer;
    *frame.add(16) = USER_CODE_SELECTOR;
    *frame.add(17) = INITIAL_RFLAGS;
    *frame.add(18) = user_rsp;
    *frame.add(19) = USER_DATA_SELECTOR;
    frame as u64
}

extern "C" fn kernel_thread_entry() -> ! {
    loop {
        KERNEL_HEARTBEAT.fetch_add(1, Ordering::Relaxed);
        core::hint::spin_loop();
    }
}

const IPC_SERVICE_PROGRAM: [u8; 82] = [
    0x49, 0x89, 0xfc, 0xb8, 0x04, 0x11, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x48, 0x83, 0xec, 0x10, 0x48,
    0x89, 0xe6, 0xba, 0x10, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x4c, 0x89, 0xcf, 0x48, 0xbb, 0x76, 0x66,
    0x73, 0x3a, 0x6f, 0x6b, 0x21, 0x0a, 0x48, 0x89, 0x1c, 0x24, 0xb8, 0x11, 0x00, 0x00, 0x00, 0xcd,
    0x80, 0xb8, 0x05, 0x11, 0x00, 0x00, 0x48, 0x89, 0xe6, 0xba, 0x08, 0x00, 0x00, 0x00, 0xb9, 0x03,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x05, 0x11, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x83, 0xc4, 0x10,
    0xeb, 0xb1,
];

const IPC_CLIENT_PROGRAM: [u8; 210] = [
    0xb8, 0x00, 0x11, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x83, 0xf8, 0x02, 0x0f, 0x85, 0xbf, 0x00, 0x00,
    0x00, 0xb8, 0x01, 0x11, 0x00, 0x00, 0xcd, 0x80, 0x49, 0x89, 0xc4, 0xb8, 0x02, 0x11, 0x00, 0x00,
    0xbf, 0x01, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x49, 0x89, 0xc5, 0x48, 0x83, 0xec, 0x10, 0x48, 0xbb,
    0x2f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x48, 0x89, 0x1c, 0x24, 0x4c, 0x89, 0xef, 0x48,
    0xbb, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x31, 0xdf, 0xb8, 0x03, 0x11, 0x00,
    0x00, 0x48, 0x89, 0xe6, 0xba, 0x08, 0x00, 0x00, 0x00, 0xb9, 0x03, 0x00, 0x00, 0x00, 0x4d, 0x89,
    0xe0, 0xcd, 0x80, 0x48, 0x83, 0xf8, 0xf7, 0x75, 0x67, 0xb8, 0x03, 0x11, 0x00, 0x00, 0x4c, 0x89,
    0xef, 0x48, 0x89, 0xe6, 0xba, 0x08, 0x00, 0x00, 0x00, 0xb9, 0x03, 0x00, 0x00, 0x00, 0x4d, 0x89,
    0xe0, 0xcd, 0x80, 0xb8, 0x04, 0x11, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x48, 0x89, 0xe6, 0xba, 0x08,
    0x00, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x83, 0xf9, 0x03, 0x75, 0x35, 0x4d, 0x85, 0xc9, 0x75, 0x30,
    0x48, 0xbb, 0x76, 0x66, 0x73, 0x3a, 0x6f, 0x6b, 0x21, 0x0a, 0x48, 0x39, 0x1c, 0x24, 0x75, 0x20,
    0xb8, 0x11, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x48, 0x83, 0xc4, 0x10, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0xcd, 0x80, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xbf, 0x02, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb, 0xeb,
    0xeb, 0xfe,
];

global_asm!(
    r#"
    .section .rodata.user_selftest,"a"
    .code64
    .global vkernel_user_program_start
    .global vkernel_user_program_end
vkernel_user_program_start:
    sub rsp, 768
    mov eax, 257
    mov edi, -100
    lea rsi, [rip + 10f]
    mov edx, 0x242
    xor r10d, r10d
    syscall
    test rax, rax
    js 99f
    mov r12, rax
    mov eax, 1
    mov rdi, r12
    lea rsi, [rip + 11f]
    mov edx, 8
    syscall
    cmp rax, 8
    jne 99f
    mov eax, 8
    mov rdi, r12
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne 99f
    xor eax, eax
    mov rdi, r12
    mov rsi, rsp
    mov edx, 8
    syscall
    cmp rax, 8
    jne 99f
    mov rbx, 0x0a21216b6f2d6466
    cmp qword ptr [rsp], rbx
    jne 99f

    lea rbx, [rsp + 512]
    mov qword ptr [rbx], 39
    mov qword ptr [rbx + 64], 102
    mov eax, 0x1200
    mov rdi, rbx
    mov esi, 2
    syscall
    cmp rax, 2
    jne 99f
    cmp qword ptr [rbx + 56], 0
    jle 99f
    cmp qword ptr [rbx + 120], 0
    jne 99f

    lea rbx, [rsp + 640]
    mov qword ptr [rbx], 0
    mov qword ptr [rbx + 8], r12
    mov qword ptr [rbx + 16], rsp
    mov qword ptr [rbx + 24], 8
    mov qword ptr [rbx + 32], 0
    mov eax, 0x1201
    mov rdi, rbx
    syscall
    test rax, rax
    jle 99f
    mov r13, rax
    mov eax, 0x1202
    mov rdi, r13
    mov rsi, rsp
    mov edx, 8
    syscall
    cmp rax, 8
    jne 99f
    mov rbx, 0x0a21216b6f2d6466
    cmp qword ptr [rsp], rbx
    jne 99f
    mov eax, 1
    mov edi, 1
    lea rsi, [rip + 18f]
    mov edx, 19
    syscall
    cmp rax, 19
    jne 99f

    mov eax, 332
    mov edi, -100
    lea rsi, [rip + 10f]
    xor edx, edx
    mov r10d, 0x7ff
    lea r8, [rsp + 256]
    syscall
    test rax, rax
    jne 99f
    cmp qword ptr [rsp + 296], 8
    jne 99f
    mov eax, 3
    mov rdi, r12
    syscall
    test rax, rax
    jne 99f
    mov eax, 257
    mov edi, -100
    lea rsi, [rip + 12f]
    mov edx, 0x10000
    xor r10d, r10d
    syscall
    test rax, rax
    js 99f
    mov r12, rax
    mov eax, 217
    mov rdi, r12
    mov rsi, rsp
    mov edx, 256
    syscall
    test rax, rax
    jle 99f
    mov eax, 3
    mov rdi, r12
    syscall
    test rax, rax
    jne 99f
    mov eax, 1
    mov edi, 1
    lea rsi, [rip + 13f]
    mov edx, 12
    syscall
    cmp rax, 12
    jne 99f

    mov eax, 228
    mov edi, 1
    mov rsi, rsp
    syscall
    test rax, rax
    jne 99f
    cmp qword ptr [rsp + 8], 1000000000
    jae 99f

    mov eax, 96
    mov rdi, rsp
    xor esi, esi
    syscall
    test rax, rax
    jne 99f
    cmp qword ptr [rsp + 8], 1000000
    jae 99f

    mov qword ptr [rsp], 0
    mov qword ptr [rsp + 8], 0
    mov eax, 35
    mov rdi, rsp
    xor esi, esi
    syscall
    test rax, rax
    jne 99f

    mov qword ptr [rsp + 8], 1000000000
    mov eax, 35
    mov rdi, rsp
    xor esi, esi
    syscall
    cmp rax, -22
    jne 99f

    mov qword ptr [rsp], 0
    mov qword ptr [rsp + 8], 0
    mov eax, 230
    mov edi, 1
    xor esi, esi
    mov rdx, rsp
    xor r10d, r10d
    syscall
    test rax, rax
    jne 99f

    mov eax, 201
    mov rdi, rsp
    syscall
    cmp qword ptr [rsp], rax
    jne 99f

    mov eax, 318
    mov rdi, rsp
    mov esi, 16
    xor edx, edx
    syscall
    cmp rax, 16
    jne 99f
    mov rbx, qword ptr [rsp]
    or rbx, qword ptr [rsp + 8]
    jz 99f

    mov eax, 63
    mov rdi, rsp
    syscall
    test rax, rax
    jne 99f
    cmp dword ptr [rsp], 0x74737552
    jne 99f
    cmp word ptr [rsp + 4], 0x7869
    jne 99f

    mov eax, 99
    mov rdi, rsp
    syscall
    test rax, rax
    jne 99f
    cmp dword ptr [rsp + 104], 1
    jne 99f

    mov eax, 309
    mov rdi, rsp
    lea rsi, [rsp + 4]
    xor edx, edx
    syscall
    test rax, rax
    jne 99f

    mov eax, 318
    xor edi, edi
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, -14
    jne 99f

    mov eax, 35
    mov rdi, -1
    xor esi, esi
    syscall
    cmp rax, -14
    jne 99f

    mov eax, 318
    mov rdi, -4
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, -14
    jne 99f

    mov eax, 318
    lea rdi, [rip + 10f]
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, -14
    jne 99f

    mov rdi, 0x00007fffffffeffc
    mov dword ptr [rdi], 0x12345678
    mov eax, 318
    mov esi, 8
    xor edx, edx
    syscall
    cmp rax, -14
    jne 99f
    cmp dword ptr [rdi], 0x12345678
    jne 99f

    mov eax, 318
    mov rdi, rsp
    mov esi, 0x100001
    xor edx, edx
    syscall
    cmp rax, -22
    jne 99f

    mov eax, 1
    mov edi, 1
    mov rsi, rsp
    mov rdx, 0x8000000000000000
    syscall
    cmp rax, -22
    jne 99f

    mov eax, 318
    xor edi, edi
    xor esi, esi
    xor edx, edx
    syscall
    test rax, rax
    jne 99f

    mov eax, 318
    mov rdi, rsp
    mov esi, 8
    mov edx, 6
    syscall
    cmp rax, -22
    jne 99f

    mov eax, 0xffff
    syscall
    cmp rax, -38
    jne 99f

    mov eax, 1
    mov edi, 1
    lea rsi, [rip + 14f]
    mov edx, 17
    syscall
    cmp rax, 17
    jne 99f

    add rsp, 768
    mov eax, 0x1003
    syscall
    test rax, rax
    jz 20f
    mov eax, 0x1005
    syscall
    sub rsp, 40
    lea rax, [rip + 15f]
    mov qword ptr [rsp], rax
    lea rax, [rip + 16f]
    mov qword ptr [rsp + 8], rax
    mov qword ptr [rsp + 16], 0
    lea rax, [rip + 17f]
    mov qword ptr [rsp + 24], rax
    mov qword ptr [rsp + 32], 0
    mov eax, 59
    lea rdi, [rip + 15f]
    mov rsi, rsp
    lea rdx, [rsp + 24]
    syscall
    ud2
20:
    mov eax, 0x1004
    mov edi, 7
    syscall
    ud2
99:
    ud2
10:
    .asciz "/home/fd-test.txt"
11:
    .ascii "fd-ok!!\n"
12:
    .asciz "/"
13:
    .ascii "[fd] abi=ok\n"
14:
    .ascii "[syscall] abi=ok\n"
15:
    .asciz "/bin/elf-selftest"
16:
    .asciz "pie"
17:
    .asciz "RUSTIX=1"
18:
    .ascii "[batch/aio] abi=ok\n"
vkernel_user_program_end:
    "#
);

fn user_program() -> &'static [u8] {
    unsafe extern "C" {
        static vkernel_user_program_start: u8;
        static vkernel_user_program_end: u8;
    }
    let start = core::ptr::addr_of!(vkernel_user_program_start);
    let end = core::ptr::addr_of!(vkernel_user_program_end);
    let length = end as usize - start as usize;
    unsafe { core::slice::from_raw_parts(start, length) }
}

global_asm!(
    r#"
    .section .rodata.user_elf,"a"
    .code64

    .global vkernel_elf_test_program_start
    .global vkernel_elf_test_program_end
vkernel_elf_test_program_start:
    .byte 0x7f, 'E', 'L', 'F', 2, 1, 1, 0
    .zero 8
    .short 3
    .short 62
    .long 1
    .quad elf_test_main_entry - vkernel_elf_test_program_start
    .quad 64
    .quad 0
    .long 0
    .short 64
    .short 56
    .short 3
    .short 0
    .short 0
    .short 0

    .long 6
    .long 4
    .quad 64
    .quad 64
    .quad 64
    .quad 168
    .quad 168
    .quad 8

    .long 3
    .long 4
    .quad elf_test_interp_path - vkernel_elf_test_program_start
    .quad elf_test_interp_path - vkernel_elf_test_program_start
    .quad elf_test_interp_path - vkernel_elf_test_program_start
    .quad 18
    .quad 18
    .quad 1

    .long 1
    .long 5
    .quad 0
    .quad 0
    .quad 0
    .quad vkernel_elf_test_program_end - vkernel_elf_test_program_start
    .quad vkernel_elf_test_program_end - vkernel_elf_test_program_start
    .quad 4096

elf_test_interp_path:
    .asciz "/lib/ld-rustix.so"
    .balign 16
elf_test_main_entry:
    mov rbp, rsp
    cmp qword ptr [rbp], 2
    jne elf_test_failure
    mov rsi, qword ptr [rbp + 8]
    lea rdi, [rip + elf_test_argv0]
    call elf_test_string_equal
    test eax, eax
    jz elf_test_failure
    mov rsi, qword ptr [rbp + 16]
    lea rdi, [rip + elf_test_argv1]
    call elf_test_string_equal
    test eax, eax
    jz elf_test_failure
    cmp qword ptr [rbp + 24], 0
    jne elf_test_failure
    mov rsi, qword ptr [rbp + 32]
    lea rdi, [rip + elf_test_env0]
    call elf_test_string_equal
    test eax, eax
    jz elf_test_failure
    cmp qword ptr [rbp + 40], 0
    jne elf_test_failure

    xor ebx, ebx
    xor r8d, r8d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    lea rsi, [rbp + 48]
elf_test_aux_loop:
    mov rax, qword ptr [rsi]
    mov rdx, qword ptr [rsi + 8]
    test rax, rax
    jz elf_test_aux_done
    cmp eax, 3
    je elf_test_aux_phdr
    cmp eax, 4
    je elf_test_aux_phent
    cmp eax, 5
    je elf_test_aux_phnum
    cmp eax, 6
    je elf_test_aux_pagesz
    cmp eax, 7
    je elf_test_aux_base
    cmp eax, 9
    je elf_test_aux_entry
    cmp eax, 25
    je elf_test_aux_random
    cmp eax, 31
    je elf_test_aux_execfn
elf_test_aux_next:
    add rsi, 16
    jmp elf_test_aux_loop
elf_test_aux_phdr:
    mov r15, rdx
    jmp elf_test_aux_next
elf_test_aux_phent:
    mov r10, rdx
    jmp elf_test_aux_next
elf_test_aux_phnum:
    mov r11, rdx
    jmp elf_test_aux_next
elf_test_aux_pagesz:
    mov r8, rdx
    jmp elf_test_aux_next
elf_test_aux_base:
    mov r13, rdx
    jmp elf_test_aux_next
elf_test_aux_entry:
    mov r12, rdx
    jmp elf_test_aux_next
elf_test_aux_random:
    mov r14, rdx
    jmp elf_test_aux_next
elf_test_aux_execfn:
    mov rbx, rdx
    jmp elf_test_aux_next
elf_test_aux_done:
    lea rax, [rip + elf_test_main_entry]
    cmp r12, rax
    jne elf_test_failure
    lea rax, [rip + vkernel_elf_test_program_start + 64]
    cmp r15, rax
    jne elf_test_failure
    cmp r10, 56
    jne elf_test_failure
    cmp r11, 3
    jne elf_test_failure
    cmp r8, 4096
    jne elf_test_failure
    test r13, r13
    jz elf_test_failure
    test r13d, 0x1fffff
    jnz elf_test_failure
    test r14, r14
    jz elf_test_failure
    mov rax, qword ptr [r14]
    or rax, qword ptr [r14 + 8]
    jz elf_test_failure
    cmp rbx, qword ptr [rbp + 8]
    jne elf_test_failure
    lea rax, [rip + vkernel_elf_test_program_start]
    test eax, 0x1fffff
    jnz elf_test_failure

    mov eax, 1
    mov edi, 1
    lea rsi, [rip + elf_test_ok]
    mov edx, 13
    syscall
    cmp rax, 13
    jne elf_test_failure

    mov eax, 0x1008
    lea rdi, [rip + elf_test_signal_handler]
    syscall
    mov eax, 0x1006
    syscall
    test rax, rax
    jz elf_test_clone_child
    mov eax, 0x1002
    mov edi, 1
    syscall
    mov eax, 0x100b
    mov edi, 0x44
    syscall
    mov eax, 0x100d
    mov edi, 2
    mov esi, 10
    syscall
    mov eax, 0x100e
    mov edi, 1
    syscall
    mov eax, 0x100c
    mov edi, -5
    syscall
    mov eax, 0x1002
    mov edi, 1
    syscall
    sub rsp, 40
    lea rax, [rip + elf_test_static_path]
    mov qword ptr [rsp], rax
    lea rax, [rip + elf_test_static_arg]
    mov qword ptr [rsp + 8], rax
    mov qword ptr [rsp + 16], 0
    lea rax, [rip + elf_test_static_env]
    mov qword ptr [rsp + 24], rax
    mov qword ptr [rsp + 32], 0
    mov eax, 59
    lea rdi, [rip + elf_test_static_path]
    mov rsi, rsp
    lea rdx, [rsp + 24]
    syscall
    ud2
elf_test_heartbeat:
    mov eax, 0x1000
    syscall
    mov eax, 0x1002
    mov edi, 2
    syscall
    jmp elf_test_heartbeat
elf_test_clone_child:
    mov eax, 0x100a
    mov edi, 0x44
    syscall
    mov eax, 0x1004
    xor edi, edi
    syscall
    ud2
elf_test_signal_handler:
    mov eax, 0x1009
    syscall
    ud2
elf_test_string_equal:
    mov al, byte ptr [rsi]
    cmp al, byte ptr [rdi]
    jne elf_test_string_not_equal
    test al, al
    jz elf_test_string_is_equal
    inc rsi
    inc rdi
    jmp elf_test_string_equal
elf_test_string_not_equal:
    xor eax, eax
    ret
elf_test_string_is_equal:
    mov eax, 1
    ret
elf_test_failure:
    ud2
elf_test_argv0:
    .asciz "/bin/elf-selftest"
elf_test_argv1:
    .asciz "pie"
elf_test_env0:
    .asciz "RUSTIX=1"
elf_test_ok:
    .ascii "[elf] abi=ok\n"
elf_test_static_path:
    .asciz "/bin/static-selftest"
elf_test_static_arg:
    .asciz "c-rust"
elf_test_static_env:
    .asciz "RUSTIX_STATIC=1"
vkernel_elf_test_program_end:

    .balign 16
    .global vkernel_elf_test_interpreter_start
    .global vkernel_elf_test_interpreter_end
vkernel_elf_test_interpreter_start:
    .byte 0x7f, 'E', 'L', 'F', 2, 1, 1, 0
    .zero 8
    .short 3
    .short 62
    .long 1
    .quad elf_test_interpreter_entry - vkernel_elf_test_interpreter_start
    .quad 64
    .quad 0
    .long 0
    .short 64
    .short 56
    .short 2
    .short 0
    .short 0
    .short 0

    .long 6
    .long 4
    .quad 64
    .quad 64
    .quad 64
    .quad 112
    .quad 112
    .quad 8

    .long 1
    .long 5
    .quad 0
    .quad 0
    .quad 0
    .quad vkernel_elf_test_interpreter_end - vkernel_elf_test_interpreter_start
    .quad vkernel_elf_test_interpreter_end - vkernel_elf_test_interpreter_start
    .quad 4096

    .balign 16
elf_test_interpreter_entry:
    mov rcx, qword ptr [rsp]
    lea rsi, [rsp + rcx * 8 + 16]
elf_test_interpreter_env:
    cmp qword ptr [rsi], 0
    je elf_test_interpreter_aux
    add rsi, 8
    jmp elf_test_interpreter_env
elf_test_interpreter_aux:
    add rsi, 8
elf_test_interpreter_aux_loop:
    mov rax, qword ptr [rsi]
    test rax, rax
    jz elf_test_interpreter_failure
    cmp rax, 9
    je elf_test_interpreter_jump
    add rsi, 16
    jmp elf_test_interpreter_aux_loop
elf_test_interpreter_jump:
    jmp qword ptr [rsi + 8]
elf_test_interpreter_failure:
    ud2
vkernel_elf_test_interpreter_end:
    "#
);

global_asm!(
    r#"
    .section .rodata.user_vdso,"a"
    .code64

    .global vkernel_vdso_start
    .global vkernel_vdso_end
vkernel_vdso_start:
    .byte 0x7f, 'E', 'L', 'F', 2, 1, 1, 0
    .zero 8
    .short 3
    .short 62
    .long 1
    .quad vdso_clock_gettime - vkernel_vdso_start
    .quad 64
    .quad 0
    .long 0
    .short 64
    .short 56
    .short 2
    .short 0
    .short 0
    .short 0

    .long 6
    .long 4
    .quad 64
    .quad 64
    .quad 64
    .quad 112
    .quad 112
    .quad 8

    .long 1
    .long 5
    .quad 0
    .quad 0
    .quad 0
    .quad vkernel_vdso_end - vkernel_vdso_start
    .quad vkernel_vdso_end - vkernel_vdso_start
    .quad 4096

    .balign 16
vdso_clock_gettime:
    mov eax, 228
    syscall
    ret
vkernel_vdso_end:

    .section .rodata.user_static,"a"
    .code64

    .global vkernel_static_test_program_start
    .global vkernel_static_test_program_end
vkernel_static_test_program_start:
    .byte 0x7f, 'E', 'L', 'F', 2, 1, 1, 0
    .zero 8
    .short 2
    .short 62
    .long 1
    .quad static_test_entry - vkernel_static_test_program_start + 0x400000
    .quad 64
    .quad 0
    .long 0
    .short 64
    .short 56
    .short 2
    .short 0
    .short 0
    .short 0

    .long 6
    .long 4
    .quad 64
    .quad 0x400040
    .quad 0x400040
    .quad 112
    .quad 112
    .quad 8

    .long 1
    .long 5
    .quad 0
    .quad 0x400000
    .quad 0x400000
    .quad vkernel_static_test_program_end - vkernel_static_test_program_start
    .quad vkernel_static_test_program_end - vkernel_static_test_program_start
    .quad 4096

    .balign 16
static_test_entry:
    mov rbp, rsp
    cmp qword ptr [rbp], 2
    jne static_test_failure
    mov rax, qword ptr [rbp + 8]
    mov rdx, 0x6174732f6e69622f
    cmp qword ptr [rax], rdx
    jne static_test_failure

    mov rcx, qword ptr [rbp]
    lea rsi, [rbp + rcx * 8 + 16]
static_test_environment:
    cmp qword ptr [rsi], 0
    je static_test_aux_start
    add rsi, 8
    jmp static_test_environment
static_test_aux_start:
    add rsi, 8
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
static_test_aux_loop:
    mov rax, qword ptr [rsi]
    mov rdx, qword ptr [rsi + 8]
    test rax, rax
    jz static_test_aux_done
    cmp eax, 7
    je static_test_aux_base
    cmp eax, 33
    je static_test_aux_vdso
static_test_aux_next:
    add rsi, 16
    jmp static_test_aux_loop
static_test_aux_base:
    mov r12, rdx
    mov r14d, 1
    jmp static_test_aux_next
static_test_aux_vdso:
    mov r13, rdx
    jmp static_test_aux_next
static_test_aux_done:
    cmp r14d, 1
    jne static_test_failure
    test r12, r12
    jnz static_test_failure
    test r13, r13
    jz static_test_failure
    cmp dword ptr [r13], 0x464c457f
    jne static_test_failure

    sub rsp, 48
    mov rax, 0x525553544c534f4b
    mov qword ptr [rsp], rax
    mov qword ptr [rsp + 8], 0
    mov eax, 158
    mov edi, 0x1002
    mov rsi, rsp
    syscall
    test rax, rax
    jne static_test_failure
    mov eax, 158
    mov edi, 0x1003
    lea rsi, [rsp + 16]
    syscall
    test rax, rax
    jne static_test_failure
    cmp qword ptr [rsp + 16], rsp
    jne static_test_failure
    mov rax, qword ptr fs:[0]
    mov rdx, 0x525553544c534f4b
    cmp rax, rdx
    jne static_test_failure

    mov rax, qword ptr [r13 + 24]
    add rax, r13
    mov edi, 1
    lea rsi, [rsp + 24]
    call rax
    test rax, rax
    jne static_test_failure
    cmp qword ptr [rsp + 32], 1000000000
    jae static_test_failure

    mov eax, 0x1006
    syscall
    test rax, rax
    jz static_test_clone_child
    mov eax, 0x1002
    mov edi, 1
    syscall
    cmp qword ptr fs:[8], 1
    jne static_test_failure

    mov eax, 1
    mov edi, 1
    lea rsi, [rip + static_test_ok]
    mov edx, 16
    syscall
    cmp rax, 16
    jne static_test_failure
    mov eax, 0x1003
    syscall
    test rax, rax
    jz static_test_busybox_child
static_test_heartbeat:
    mov eax, 0x1000
    syscall
    mov eax, 0x1002
    mov edi, 2
    syscall
    jmp static_test_heartbeat

static_test_clone_child:
    mov rax, qword ptr fs:[0]
    mov rdx, 0x525553544c534f4b
    cmp rax, rdx
    jne static_test_failure
    mov eax, 158
    mov edi, 0x1003
    lea rsi, [rsp + 16]
    syscall
    test rax, rax
    jne static_test_failure
    cmp qword ptr [rsp + 16], rsp
    jne static_test_failure
    mov qword ptr fs:[8], 1
    mov eax, 0x1004
    xor edi, edi
    syscall
static_test_busybox_child:
    sub rsp, 48
    lea rax, [rip + static_test_busybox_path]
    mov qword ptr [rsp], rax
    lea rax, [rip + static_test_busybox_sh]
    mov qword ptr [rsp + 8], rax
    lea rax, [rip + static_test_script_path]
    mov qword ptr [rsp + 16], rax
    mov qword ptr [rsp + 24], 0
    lea rax, [rip + static_test_busybox_env]
    mov qword ptr [rsp + 32], rax
    mov qword ptr [rsp + 40], 0
    mov eax, 59
    lea rdi, [rip + static_test_busybox_path]
    mov rsi, rsp
    lea rdx, [rsp + 32]
    syscall
    ud2
static_test_failure:
    ud2
static_test_ok:
    .ascii "[static] abi=ok\n"
static_test_busybox_path:
    .asciz "/bin/busybox"
static_test_busybox_sh:
    .asciz "sh"
static_test_script_path:
    .asciz "/etc/init.d/rcS"
static_test_busybox_env:
    .asciz "PATH=/bin"
vkernel_static_test_program_end:
    "#
);

fn elf_test_program() -> &'static [u8] {
    unsafe extern "C" {
        static vkernel_elf_test_program_start: u8;
        static vkernel_elf_test_program_end: u8;
    }
    symbol_range(
        core::ptr::addr_of!(vkernel_elf_test_program_start),
        core::ptr::addr_of!(vkernel_elf_test_program_end),
    )
}

fn static_test_program() -> &'static [u8] {
    unsafe extern "C" {
        static vkernel_static_test_program_start: u8;
        static vkernel_static_test_program_end: u8;
    }
    symbol_range(
        core::ptr::addr_of!(vkernel_static_test_program_start),
        core::ptr::addr_of!(vkernel_static_test_program_end),
    )
}

fn vdso_image() -> &'static [u8] {
    unsafe extern "C" {
        static vkernel_vdso_start: u8;
        static vkernel_vdso_end: u8;
    }
    symbol_range(
        core::ptr::addr_of!(vkernel_vdso_start),
        core::ptr::addr_of!(vkernel_vdso_end),
    )
}

fn elf_image(path: &str) -> Option<&'static [u8]> {
    match path {
        "/bin/elf-selftest" => Some(elf_test_program()),
        "/bin/static-selftest" => Some(static_test_program()),
        #[cfg(feature = "busybox")]
        "/bin/busybox" => Some(busybox_image()),
        "/bin/bash" => Some(TESTBIN_BASH),
        "/bin/fwtest" => Some(TESTBIN_FWTEST),
        "/bin/curl" => Some(TESTBIN_CURL),
        "/bin/fastfetch" => Some(TESTBIN_FASTFETCH),
        "/bin/ls" | "/bin/cat" | "/bin/cp" | "/bin/head" | "/bin/wc" | "/bin/true" => {
            Some(TESTBIN_COREUTILS)
        }
        _ => None,
    }
}

#[cfg(feature = "busybox")]
fn busybox_image() -> &'static [u8] {
    include_bytes!(env!("RUSTIX_BUSYBOX_PATH"))
}

// Linux 兼容性测试程序（musl 静态链接，由 .dbg/build_userland_tests.sh 提供）
const TESTBIN_BASH: &[u8] = include_bytes!("../../build/userland/testbin/bash");
const TESTBIN_FWTEST: &[u8] = include_bytes!("../../build/userland/testbin/fwtest");
const TESTBIN_COREUTILS: &[u8] = include_bytes!("../../build/userland/testbin/coreutils");
const TESTBIN_CURL: &[u8] = include_bytes!("../../build/userland/testbin/curl");
const TESTBIN_FASTFETCH: &[u8] = include_bytes!("../../build/userland/testbin/fastfetch");

fn elf_test_interpreter() -> &'static [u8] {
    unsafe extern "C" {
        static vkernel_elf_test_interpreter_start: u8;
        static vkernel_elf_test_interpreter_end: u8;
    }
    symbol_range(
        core::ptr::addr_of!(vkernel_elf_test_interpreter_start),
        core::ptr::addr_of!(vkernel_elf_test_interpreter_end),
    )
}

fn symbol_range(start: *const u8, end: *const u8) -> &'static [u8] {
    let length = end as usize - start as usize;
    unsafe { core::slice::from_raw_parts(start, length) }
}

/// Load an embedded ELF binary into a spawned child and set up its register frame
/// for user-mode execution. The child must already exist (spawned via SharedTaskTable).
pub fn exec_embedded(child_pid: Pid, path: &'static str, argv: &[&str]) -> Result<(), &'static str> {
    let image = elf_image(path).ok_or("unknown embedded binary")?;
    let state = unsafe { &mut *SCHEDULER.get() };
    let slot = state
        .threads
        .iter()
        .position(|t| t.id == child_pid)
        .ok_or("pid not found in scheduler")?;
    let old_cr3 = state.threads[slot].cr3;
    let loaded = elf::load_process(
        image,
        Some(("/lib/ld-rustix.so", elf_test_interpreter())),
        vdso_image(),
        argv,
        &[],
        next_random(),
    )
    .map_err(|_| "elf load failed")?;
    let _ = SharedTaskTable::new().execve(child_pid, path);
    state.threads[slot].cr3 = loaded.cr3;
    state.threads[slot].fs_base = 0;
    state.threads[slot].owns_address_space = true;
    state.threads[slot].image = image;
    let kernel_stack_top = state.threads[slot].kernel_stack_top;
    let frame_ptr =
        (kernel_stack_top - core::mem::size_of::<RegisterFrame>() as u64) as *mut RegisterFrame;
    unsafe {
        (*frame_ptr).rip = loaded.entry;
        (*frame_ptr).cs = USER_CODE_SELECTOR;
        (*frame_ptr).rflags = INITIAL_RFLAGS;
        (*frame_ptr).rsp = loaded.stack_pointer;
        (*frame_ptr).ss = USER_DATA_SELECTOR;
        (*frame_ptr).rax = 0;
    }
    retire_address_space(state, old_cr3);
    reclaim_retired_spaces(state);
    EXEC_CYCLES.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

pub fn is_embedded_binary(path: &str) -> bool {
    elf_image(path).is_some()
}
