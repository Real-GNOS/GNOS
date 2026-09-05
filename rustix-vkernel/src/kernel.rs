use core::fmt::Write;

use crate::apps;
use crate::fixed::InlineString;
use crate::ipc::{CapabilityId, EndpointId, IpcError, IpcRights, IpcTable, Message, RequestKind};
use crate::net;
use crate::services::{ConsoleService, DirEntries, VfsService};

mod task;

#[cfg(target_arch = "x86_64")]
mod run_queue;

#[cfg(target_arch = "x86_64")]
mod elf;

#[cfg(target_arch = "x86_64")]
pub mod scheduler;

use task::{
    Pid, SchedulingPolicy, SharedTaskTable, SignalAction, TaskKind, TaskState, WaitError,
    WaitResult, CLONE_FILES, CLONE_FS, CLONE_SIGHAND, CLONE_THREAD, CLONE_VM, MAX_TASKS, SIGTERM,
    SIGUSR1, WNOHANG,
};

pub mod sched {
    pub const MAX_CPUS: usize = 8;
}

const MAX_SERVICES: usize = 8;
const MAX_TRACE: usize = 64;
const MAX_ARGS: usize = 8;

#[derive(Clone, Copy)]
struct CachedCapability {
    pid: Pid,
    capability: CapabilityId,
}

#[derive(Clone, Copy)]
struct ServiceEntry {
    name: &'static str,
    endpoint: EndpointId,
    owner_capability: CapabilityId,
    pid: Pid,
}

struct IpcRuntime {
    table: IpcTable,
    client_capabilities: [[Option<CachedCapability>; MAX_TASKS]; MAX_SERVICES],
}

impl IpcRuntime {
    const fn new() -> Self {
        Self {
            table: IpcTable::new(),
            client_capabilities: [[None; MAX_TASKS]; MAX_SERVICES],
        }
    }
}

static IPC_RUNTIME: crate::sync::SpinMutex<IpcRuntime> =
    crate::sync::SpinMutex::new(IpcRuntime::new());

pub struct Kernel {
    shell_pid: Pid,
    tasks: SharedTaskTable,
    services: [Option<ServiceEntry>; MAX_SERVICES],
    service_count: usize,
    ipc_trace: [InlineString<96>; MAX_TRACE],
    ipc_count: usize,
    ipc_head: usize,
    console: ConsoleService,
    vfs: VfsService,
    task_lifecycle_ok: bool,
    ipc_self_test_ok: bool,
    vfs_namespace_ok: bool,
}

pub struct SysApi<'a> {
    kernel: &'a mut Kernel,
    pid: Pid,
}

impl Kernel {
    pub const fn new() -> Self {
        Self {
            shell_pid: 0,
            tasks: SharedTaskTable::new(),
            services: [None; MAX_SERVICES],
            service_count: 0,
            ipc_trace: [InlineString::new(); MAX_TRACE],
            ipc_count: 0,
            ipc_head: 0,
            console: ConsoleService::new(),
            vfs: VfsService::new(),
            task_lifecycle_ok: false,
            ipc_self_test_ok: false,
            vfs_namespace_ok: false,
        }
    }

    pub fn boot(&mut self) {
        self.task_lifecycle_ok = task::lifecycle_self_test();
        self.ipc_self_test_ok = crate::ipc::self_test();
        self.vfs_namespace_ok = self.vfs.namespace_self_test();
        let kernel_pid = self
            .tasks
            .get_by_pid(1)
            .map(|task| task.pid)
            .or_else(|| self.spawn_task(None, "kernel", TaskKind::Kernel).ok())
            .expect("kernel task table exhausted during boot");
        self.set_state(kernel_pid, TaskState::Running);
        self.register_kernel_endpoint(kernel_pid, "kernel")
            .expect("kernel endpoint registration failed");
        self.register_service(kernel_pid, "console")
            .expect("console service registration failed");
        self.register_service(kernel_pid, "vfs")
            .expect("vfs service registration failed");
        self.register_service(kernel_pid, "proc")
            .expect("proc service registration failed");
        if crate::net::init() {
            self.register_service(kernel_pid, "net")
                .expect("net service registration failed");
        }
        self.shell_pid = self
            .spawn_task(Some(kernel_pid), "shell", TaskKind::Shell)
            .expect("shell task table exhausted during boot");
        self.set_state(self.shell_pid, TaskState::Running);
    }

    pub fn run(&mut self) -> ! {
        self.print_banner();
        let mut lifecycle = InlineString::<96>::new();
        let _ = write!(
            &mut lifecycle,
            "[task] lifecycle={} ipc={} vfs={} active={} zombies={}",
            if self.task_lifecycle_ok {
                "ok"
            } else {
                "failed"
            },
            if self.ipc_self_test_ok {
                "ok"
            } else {
                "failed"
            },
            if self.vfs_namespace_ok {
                "ok"
            } else {
                "failed"
            },
            self.tasks.active_count(),
            self.tasks.zombie_count(),
        );
        self.write_line(lifecycle.as_str());
        self.write_line("[monitor] ring0 compatibility UI; isolated services use Ring 3 IPC");
        let mut line = InlineString::<128>::new();

        loop {
            self.poll_task_timers();
            line.clear();
            let input = self.prompt("root@rustix-vkernel:/# ", &mut line);
            if !self.shell_command(input.trim()) {
                crate::arch::shutdown();
            }
        }
    }

    fn shell_command(&mut self, line: &str) -> bool {
        if line.is_empty() {
            return true;
        }

        let mut args = [""; MAX_ARGS];
        let argc = parse_words(line, &mut args);
        if argc == 0 {
            return true;
        }

        match args[0] {
            "help" => {
                self.write_line("帮助: help uname pwd whoami services ps apps ls cat exec execve exec-bg fork clone wait wait4 kill sigdemo futexdemo waitqdemo timerdemo sleep nice chrt taskset rebalance touch mkfile mkdir clear ipc ifconfig route ping shutdown");
                self.write_line("编辑: edit <path>, vi <path>, nano <path>");
                self.write_line("应用: hello game echo calc settings video-player audio-player");
                self.write_line("嵌入式: /bin/curl /bin/bash /bin/fwtest /bin/fastfetch /bin/ls /bin/cat /bin/head /bin/wc /bin/true");
            }
            "uname" => {
                self.write_line(crate::arch::uname());
            }
            "pwd" => {
                self.write_line("/");
            }
            "whoami" => {
                self.write_line("root");
            }
            "services" => {
                self.log_ipc(
                    self.shell_pid,
                    "kernel",
                    RequestKind::KernelListServices,
                    None,
                );
                self.show_services();
            }
            "ps" => {
                self.log_ipc(self.shell_pid, "kernel", RequestKind::KernelListTasks, None);
                self.show_tasks();
            }
            "apps" => {
                for program in apps::programs() {
                    let mut text = InlineString::<96>::new();
                    let _ = write!(&mut text, "{} - {}", program.path, program.summary);
                    self.write_line(text.as_str());
                }
            }
            "ls" => {
                let path = if argc > 1 { args[1] } else { "/" };
                self.log_ipc(self.shell_pid, "vfs", RequestKind::VfsListDir, Some(path));
                let mut entries = DirEntries::new();
                match self.vfs.list_dir(path, &mut entries) {
                    Ok(()) => {
                        for entry in entries.iter() {
                            self.write_line(entry);
                        }
                    }
                    Err(err) => {
                        let mut text = InlineString::<96>::new();
                        let _ = write!(&mut text, "error: {}: {}", path, err);
                        self.write_line(text.as_str());
                    }
                }
            }
            "cat" => {
                if argc <= 1 {
                    self.write_line("usage: cat <path>");
                    return true;
                }
                self.cat_path(args[1]);
            }
            "touch" | "mkfile" => {
                if argc <= 1 {
                    if args[0] == "mkfile" {
                        self.write_line("usage: mkfile <path>");
                    } else {
                        self.write_line("usage: touch <path>");
                    }
                    return true;
                }
                match self.vfs.create_file(args[1]) {
                    Ok(()) => {}
                    Err(err) => {
                        let mut line = InlineString::<96>::new();
                        let _ = write!(&mut line, "{}: {}: {}", args[0], args[1], err);
                        self.write_line(line.as_str());
                    }
                }
            }
            "mkdir" => {
                if argc <= 1 {
                    self.write_line("usage: mkdir <path>");
                    return true;
                }
                match self.vfs.create_dir(args[1]) {
                    Ok(()) => {}
                    Err(err) => {
                        let mut line = InlineString::<96>::new();
                        let _ = write!(&mut line, "mkdir: {}: {}", args[1], err);
                        self.write_line(line.as_str());
                    }
                }
            }
            "clear" => {
                crate::arch::clear_screen();
                crate::arch::gfx_console_reset();
                crate::gui::render_desktop();
            }
            "edit" | "vi" | "nano" => {
                if argc <= 1 {
                    self.write_line("usage: edit <path>");
                    return true;
                }
                if !crate::gui::open_editor_for_path(args[1]) {
                    let mut line = InlineString::<96>::new();
                    let _ = write!(&mut line, "editor: cannot open {}", args[1]);
                    self.write_line(line.as_str());
                }
            }
            "exec" => {
                if argc <= 1 {
                    self.write_line("usage: exec <path> [args]");
                    return true;
                }
                self.exec_path(args[1], &args[1..argc], false);
            }
            "execve" => {
                if argc <= 1 {
                    self.write_line("usage: execve <path> [args]");
                    return true;
                }
                self.exec_path(args[1], &args[1..argc], false);
            }
            "exec-bg" => {
                if argc <= 1 {
                    self.write_line("usage: exec-bg <path> [args]");
                    return true;
                }
                self.exec_path(args[1], &args[1..argc], true);
            }
            "fork" => {
                self.fork_command(&args[..argc]);
            }
            "clone" => {
                self.clone_command(&args[..argc]);
            }
            "wait" => {
                self.wait_command(&args[..argc]);
            }
            "wait4" => {
                self.wait4_command(&args[..argc]);
            }
            "kill" => {
                self.kill_command(&args[..argc]);
            }
            "sigdemo" => {
                self.signal_demo();
            }
            "futexdemo" => {
                self.futex_demo();
            }
            "waitqdemo" => {
                self.wait_queue_demo();
            }
            "timerdemo" => {
                self.timer_demo();
            }
            "sleep" => {
                self.sleep_command(&args[..argc]);
            }
            "nice" => {
                self.nice_command(&args[..argc]);
            }
            "chrt" => {
                self.chrt_command(&args[..argc]);
            }
            "taskset" => {
                self.taskset_command(&args[..argc]);
            }
            "rebalance" => {
                self.rebalance_command();
            }
            "ipc" => {
                self.show_ipc_trace();
            }
            "ifconfig" => {
                self.log_ipc(self.shell_pid, "net", RequestKind::NetStatus, None);
                self.show_ifconfig();
            }
            "route" => {
                self.log_ipc(self.shell_pid, "net", RequestKind::NetStatus, None);
                self.show_route();
            }
            "ping" => {
                if argc <= 1 {
                    self.write_line("usage: ping <ipv4>");
                    return true;
                }
                self.log_ipc(self.shell_pid, "net", RequestKind::NetPing, Some(args[1]));
                self.ping_host(args[1]);
            }
            "shutdown" | "poweroff" | "exit" | "quit" => {
                self.write_line("Shutting down v-kernel.");
                return false;
            }
            command => {
                if let Some(path) = apps::resolve_command(command) {
                    self.exec_path(path, &args[..argc], false);
                } else {
                    let mut text = InlineString::<96>::new();
                    let _ = write!(&mut text, "unknown command: {}", line);
                    self.write_line(text.as_str());
                }
            }
        }

        true
    }

    fn cat_path(&mut self, path: &str) {
        self.log_ipc(self.shell_pid, "vfs", RequestKind::VfsReadFile, Some(path));

        let mut scratch = InlineString::<2048>::new();
        match self.vfs.read_file(path, &mut scratch) {
            Some(text) => self.write(text),
            None => {
                let mut message = InlineString::<96>::new();
                let _ = write!(&mut message, "error: {}: no such file", path);
                self.write_line(message.as_str());
            }
        }
    }

    fn exec_path(&mut self, path: &str, argv: &[&str], background: bool) {
        self.log_ipc(self.shell_pid, "proc", RequestKind::ProcExec, Some(path));

        if crate::kernel::scheduler::is_embedded_binary(path) {
            self.exec_embedded_binary(path, argv, background);
            return;
        }

        if !self.vfs.path_exists(path) {
            let mut line = InlineString::<96>::new();
            let _ = write!(&mut line, "error: {}: no such executable", path);
            self.write_line(line.as_str());
            return;
        }

        if !self.vfs.is_executable(path) {
            let mut line = InlineString::<96>::new();
            let _ = write!(&mut line, "error: {}: not executable", path);
            self.write_line(line.as_str());
            return;
        }

        let Some(program) = apps::lookup(path) else {
            let mut line = InlineString::<96>::new();
            let _ = write!(&mut line, "error: {}: loader missing", path);
            self.write_line(line.as_str());
            return;
        };

        let pid = match self.spawn_task(Some(self.shell_pid), program.path, TaskKind::User) {
            Ok(pid) => pid,
            Err(err) => {
                self.write_line(err);
                return;
            }
        };
        if let Err(error) = self.tasks.execve(pid, program.path) {
            self.write_line(error.message());
            return;
        }
        self.set_state(pid, TaskState::Running);

        let status = {
            let mut api = SysApi { kernel: self, pid };
            (program.entry)(argv, &mut api)
        };

        if let Err(error) = self.tasks.exit(pid, status) {
            self.write_line(error.message());
            return;
        }
        if background {
            let mut line = InlineString::<96>::new();
            let _ = write!(
                &mut line,
                "[{}] zombie exit={}; run `wait {}`",
                pid, status, pid
            );
            self.write_line(line.as_str());
            return;
        }
        let reaped = match self.tasks.wait(self.shell_pid, Some(pid)) {
            Ok(result) => result,
            Err(_) => {
                self.write_line("error: foreground child could not be reaped");
                return;
            }
        };
        if reaped.status != 0 {
            let mut line = InlineString::<96>::new();
            let _ = write!(&mut line, "program exited with status {}", reaped.status);
            self.write_line(line.as_str());
        }
    }

    fn exec_embedded_binary(&mut self, path: &str, argv: &[&str], background: bool) {
        let static_path: &'static str = match path {
            "/bin/bash" => "/bin/bash",
            "/bin/curl" => "/bin/curl",
            "/bin/fwtest" => "/bin/fwtest",
            "/bin/fastfetch" => "/bin/fastfetch",
            "/bin/ls" => "/bin/ls",
            "/bin/cat" => "/bin/cat",
            "/bin/cp" => "/bin/cp",
            "/bin/head" => "/bin/head",
            "/bin/wc" => "/bin/wc",
            "/bin/true" => "/bin/true",
            _ => {
                let mut line = InlineString::<96>::new();
                let _ = write!(&mut line, "error: {}: embedded binary not supported", path);
                self.write_line(line.as_str());
                return;
            }
        };
        let pid = match self.spawn_task(Some(self.shell_pid), static_path, TaskKind::User) {
            Ok(pid) => pid,
            Err(err) => {
                self.write_line(err);
                return;
            }
        };
        self.set_state(pid, TaskState::Running);
        match crate::kernel::scheduler::exec_embedded(pid, static_path, argv) {
            Ok(()) => {}
            Err(error) => {
                let mut line = InlineString::<96>::new();
                let _ = write!(&mut line, "error: {}: {}", path, error);
                self.write_line(line.as_str());
                return;
            }
        }
        if background {
            let mut line = InlineString::<96>::new();
            let _ = write!(&mut line, "[{}] embedded launched; run `wait {}`", pid, pid);
            self.write_line(line.as_str());
            return;
        }
        let deadline = crate::arch::monotonic_time_ns() + 30_000_000_000;
        loop {
            match self.tasks.wait(self.shell_pid, Some(pid)) {
                Ok(result) => {
                    if result.status != 0 {
                        let mut line = InlineString::<96>::new();
                        let _ = write!(
                            &mut line,
                            "program exited with status {}",
                            result.status
                        );
                        self.write_line(line.as_str());
                    }
                    break;
                }
                Err(_) => {
                    if crate::arch::monotonic_time_ns() >= deadline {
                        self.write_line("error: child timed out after 30s");
                        break;
                    }
                    core::hint::spin_loop();
                }
            }
        }
    }

    fn spawn_task(
        &mut self,
        parent: Option<Pid>,
        name: &'static str,
        kind: TaskKind,
    ) -> Result<Pid, &'static str> {
        let pid = self
            .tasks
            .spawn(parent, name, kind)
            .map_err(|error| error.message())?;
        let _ = self.rebalance_tasks();
        Ok(pid)
    }

    fn register_service(&mut self, parent: Pid, name: &'static str) -> Result<(), &'static str> {
        if self.service_count >= self.services.len() {
            return Err("error: service table is full");
        }

        let pid = self.spawn_task(Some(parent), name, TaskKind::Service)?;
        let (endpoint, owner_capability) = IPC_RUNTIME
            .lock()
            .table
            .create_endpoint(pid)
            .map_err(IpcError::message)?;

        self.services[self.service_count] = Some(ServiceEntry {
            name,
            endpoint,
            owner_capability,
            pid,
        });
        self.service_count += 1;

        self.set_state(pid, TaskState::Waiting);
        Ok(())
    }

    fn register_kernel_endpoint(
        &mut self,
        owner: Pid,
        name: &'static str,
    ) -> Result<(), &'static str> {
        if self.service_count >= self.services.len() {
            return Err("error: service table is full");
        }
        let (endpoint, owner_capability) = IPC_RUNTIME
            .lock()
            .table
            .create_endpoint(owner)
            .map_err(IpcError::message)?;
        self.services[self.service_count] = Some(ServiceEntry {
            name,
            endpoint,
            owner_capability,
            pid: owner,
        });
        self.service_count += 1;
        Ok(())
    }

    fn set_state(&mut self, pid: Pid, state: TaskState) {
        let _ = self.tasks.set_state(pid, state);
    }

    fn log_ipc(
        &mut self,
        caller: Pid,
        service: &str,
        request: RequestKind,
        detail: Option<&str>,
    ) -> bool {
        if let Err(error) = self.route_ipc(caller, service, request, detail) {
            if crate::arch::supports_ipc_trace() {
                let mut line = InlineString::<96>::new();
                let _ = write!(
                    &mut line,
                    "pid={} -> {}: denied ({})",
                    caller,
                    service,
                    error.message()
                );
                self.push_ipc_trace(line);
            }
            return false;
        }

        if !crate::arch::supports_ipc_trace() {
            return true;
        }

        let mut line = InlineString::<96>::new();
        let _ = write!(
            &mut line,
            "pid={} -> {}: {}[{}]",
            caller,
            service,
            request.as_str(),
            request.opcode()
        );
        if let Some(detail) = detail {
            let _ = write!(&mut line, " {}", detail);
        }

        self.push_ipc_trace(line);
        true
    }

    fn route_ipc(
        &mut self,
        caller: Pid,
        service_name: &str,
        request: RequestKind,
        detail: Option<&str>,
    ) -> Result<(), IpcError> {
        let service_index = self.services[..self.service_count]
            .iter()
            .position(|entry| entry.is_some_and(|entry| entry.name == service_name))
            .ok_or(IpcError::InvalidEndpoint)?;
        let service = self.services[service_index].ok_or(IpcError::InvalidEndpoint)?;
        if !service_accepts(service.name, request) {
            return Err(IpcError::ProtocolViolation);
        }
        let cache_index = caller as usize % MAX_TASKS;
        let mut ipc = IPC_RUNTIME.lock();
        let cached = ipc.client_capabilities[service_index][cache_index]
            .filter(|entry| entry.pid == caller)
            .map(|entry| entry.capability);
        let capability = match cached.or_else(|| {
            ipc.table
                .capability_for(caller, service.endpoint, IpcRights::SEND)
        }) {
            Some(capability) => capability,
            None => {
                let tasks = &self.tasks;
                ipc.table.retain_subjects(|pid| {
                    tasks
                        .get_by_pid(pid)
                        .is_some_and(|task| !matches!(task.state, TaskState::Zombie(_)))
                });
                ipc.table.grant(
                    service.pid,
                    service.owner_capability,
                    caller,
                    IpcRights::SEND,
                )?
            }
        };
        ipc.client_capabilities[service_index][cache_index] = Some(CachedCapability {
            pid: caller,
            capability,
        });
        let payload = detail.unwrap_or("").as_bytes();
        let message = Message::new(request, payload)?;
        let message_id = ipc.table.send(caller, capability, message)?;
        let envelope = ipc.table.receive(service.pid, service.owner_capability)?;
        if envelope.id != message_id
            || envelope.sender != caller
            || envelope.message.opcode() != request.opcode()
            || envelope.message.payload() != payload
        {
            return Err(IpcError::ProtocolViolation);
        }
        Ok(())
    }

    fn push_ipc_trace(&mut self, line: InlineString<96>) {
        self.ipc_trace[self.ipc_head] = line;
        self.ipc_head = (self.ipc_head + 1) % self.ipc_trace.len();
        if self.ipc_count < self.ipc_trace.len() {
            self.ipc_count += 1;
        }
    }

    fn show_services(&mut self) {
        for index in 0..self.service_count {
            if let Some(entry) = self.services[index] {
                let mut line = InlineString::<96>::new();
                let _ = write!(
                    &mut line,
                    "{} endpoint={} pid={}",
                    entry.name,
                    entry.endpoint.raw(),
                    entry.pid
                );
                self.write_line(line.as_str());
            }
        }
    }

    fn show_tasks(&mut self) {
        let mut summary = InlineString::<64>::new();
        let _ = write!(
            &mut summary,
            "tasks={} zombies={} timers={}",
            self.tasks.active_count(),
            self.tasks.zombie_count(),
            self.tasks.timer_count()
        );
        self.raw_write_line(summary.as_str());
        for index in 0..self.tasks.slots() {
            let Some(task) = self.tasks.get(index) else {
                continue;
            };
            let mut state = InlineString::<24>::new();
            render_state(task.state, &mut state);

            let mut line = InlineString::<256>::new();
            let _ = write!(
                &mut line,
                "pid={} tid={} tgid={} ppid={} pgid={} sid={} cpu={} mask=0x{:x} policy={} nice={} rt={} ticks={} timers={} kind={} state={} sig=0x{:x} flags=0x{:x} name={}",
                task.pid,
                task.tid,
                task.tgid,
                task.parent.unwrap_or(0),
                task.pgid,
                task.sid,
                task.cpu,
                task.cpu_affinity,
                task.scheduling_policy.name(),
                task.nice,
                task.rt_priority,
                task.runtime_ticks,
                task.timer_expirations,
                task_kind_name(task.kind),
                state.as_str(),
                task.pending_signals,
                task.clone_flags,
                task.name
            );
            self.raw_write_line(line.as_str());
        }
    }

    fn fork_command(&mut self, args: &[&str]) {
        if args.len() > 2 {
            self.write_line("usage: fork [exit-status]");
            return;
        }
        let status = match args.get(1) {
            Some(text) => match text.parse::<i32>() {
                Ok(status) => status,
                Err(_) => {
                    self.write_line("fork: invalid exit status");
                    return;
                }
            },
            None => 0,
        };
        self.log_ipc(self.shell_pid, "proc", RequestKind::ProcFork, None);
        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);

        let mut line = InlineString::<96>::new();
        let _ = write!(
            &mut line,
            "fork: parent sees child pid={}, child return value would be 0",
            child
        );
        self.write_line(line.as_str());

        self.log_ipc(child, "proc", RequestKind::ProcExit, None);
        if let Err(error) = self.tasks.exit(child, status) {
            self.write_line(error.message());
            return;
        }
        let mut exit_line = InlineString::<96>::new();
        let _ = write!(
            &mut exit_line,
            "fork: child pid={} exited status={}; run `wait4 {}`",
            child, status, child
        );
        self.write_line(exit_line.as_str());
    }

    fn clone_command(&mut self, args: &[&str]) {
        if args.len() > 2 {
            self.write_line("usage: clone [thread|process]");
            return;
        }
        let mode = args.get(1).copied().unwrap_or("thread");
        let flags = match mode {
            "thread" => CLONE_VM | CLONE_SIGHAND | CLONE_THREAD | CLONE_FS | CLONE_FILES,
            "process" => CLONE_VM | CLONE_FS | CLONE_FILES,
            _ => {
                self.write_line("usage: clone [thread|process]");
                return;
            }
        };

        self.log_ipc(self.shell_pid, "proc", RequestKind::ProcClone, Some(mode));
        let child = match self.tasks.clone_task(self.shell_pid, "clone-child", flags) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);

        if mode == "process" {
            self.log_ipc(child, "proc", RequestKind::ProcExit, None);
            if let Err(error) = self.tasks.exit(child, 0) {
                self.write_line(error.message());
                return;
            }
        } else {
            self.set_state(child, TaskState::Waiting);
        }

        let Some(task) = self.tasks.get_by_pid(child) else {
            return;
        };
        let mut line = InlineString::<128>::new();
        let _ = write!(
            &mut line,
            "clone: pid={} tid={} tgid={} pgid={} sid={} flags=0x{:x}",
            task.pid, task.tid, task.tgid, task.pgid, task.sid, task.clone_flags
        );
        self.write_line(line.as_str());
    }

    fn wait_command(&mut self, args: &[&str]) {
        if args.len() > 2 {
            self.write_line("usage: wait [pid|all]");
            return;
        }
        if args.get(1).is_some_and(|value| *value == "all") {
            let mut reaped = 0usize;
            while let Ok(result) = self.tasks.wait(self.shell_pid, None) {
                self.report_wait(result);
                reaped += 1;
            }
            if reaped == 0 {
                self.write_line("wait: no exited children");
            }
            return;
        }
        let target = match args.get(1) {
            Some(text) => match text.parse::<Pid>() {
                Ok(pid) if pid != 0 => Some(pid),
                _ => {
                    self.write_line("wait: invalid pid");
                    return;
                }
            },
            None => None,
        };
        match self.tasks.wait(self.shell_pid, target) {
            Ok(result) => self.report_wait(result),
            Err(WaitError::NoChild) => self.write_line("wait: no such child"),
            Err(WaitError::WouldBlock) => self.write_line("wait: child is still running"),
        }
    }

    fn wait4_command(&mut self, args: &[&str]) {
        if args.len() > 3 {
            self.write_line("usage: wait4 [pid|0|all] [nohang]");
            return;
        }
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcWait4,
            args.get(1).copied(),
        );
        let options = if args.get(2).is_some_and(|value| *value == "nohang") {
            WNOHANG
        } else {
            0
        };
        if args.get(1).is_some_and(|value| *value == "all") {
            let mut reaped = 0usize;
            while let Ok(Some(result)) = self.tasks.wait4(self.shell_pid, None, options) {
                self.report_wait(result);
                reaped += 1;
                if options & WNOHANG != 0 {
                    break;
                }
            }
            if reaped == 0 {
                self.write_line("wait4: no exited children");
            }
            return;
        }
        let target = match args.get(1) {
            Some(text) => match text.parse::<Pid>() {
                Ok(pid) => Some(pid),
                Err(_) => {
                    self.write_line("wait4: invalid pid");
                    return;
                }
            },
            None => None,
        };
        match self.tasks.wait4(self.shell_pid, target, options) {
            Ok(Some(result)) => self.report_wait(result),
            Ok(None) => self.write_line("wait4: no child has exited"),
            Err(WaitError::NoChild) => self.write_line("wait4: no such child"),
            Err(WaitError::WouldBlock) => self.write_line("wait4: child is still running"),
        }
    }

    fn report_wait(&mut self, result: WaitResult) {
        let mut line = InlineString::<64>::new();
        let _ = write!(
            &mut line,
            "reaped pid={} status={}",
            result.pid, result.status
        );
        self.write_line(line.as_str());
    }

    fn kill_command(&mut self, args: &[&str]) {
        if args.len() < 2 || args.len() > 3 {
            self.write_line("usage: kill <pid> [signal]");
            return;
        }
        let pid = match args[1].parse::<Pid>() {
            Ok(pid) if pid != 0 => pid,
            _ => {
                self.write_line("kill: invalid pid");
                return;
            }
        };
        let signal = match args.get(2) {
            Some(text) => match text.parse::<u8>() {
                Ok(signal) => signal,
                Err(_) => {
                    self.write_line("kill: invalid signal");
                    return;
                }
            },
            None => SIGTERM,
        };
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcSignal,
            Some(args[1]),
        );
        match self.tasks.send_signal(pid, signal) {
            Ok(()) => {
                let mut line = InlineString::<64>::new();
                let _ = write!(&mut line, "signal {} queued for pid={}", signal, pid);
                self.write_line(line.as_str());
            }
            Err(error) => self.write_line(error.message()),
        }
    }

    fn signal_demo(&mut self) {
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcFork,
            Some("sigdemo"),
        );
        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);
        let action = SignalAction {
            handler: 0x401000,
            flags: 0,
            mask: 0,
        };
        if let Err(error) = self.tasks.set_signal_action(child, SIGUSR1, action) {
            self.write_line(error.message());
            return;
        }
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcSignal,
            Some("SIGUSR1"),
        );
        if let Err(error) = self.tasks.send_signal(child, SIGUSR1) {
            self.write_line(error.message());
            return;
        }
        let frame = match self.tasks.deliver_signal(child) {
            Ok(frame) => frame,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        let mut line = InlineString::<160>::new();
        let _ = write!(
            &mut line,
            "rt_sigframe: pid={} sig={} handler=0x{:x} restorer=0x{:x} ip=0x{:x} sp=0x{:x}",
            child, frame.signum, frame.handler, frame.restorer, frame.saved_ip, frame.saved_sp
        );
        self.write_line(line.as_str());
        self.log_ipc(child, "proc", RequestKind::ProcExit, None);
        if let Err(error) = self.tasks.exit(child, 0) {
            self.write_line(error.message());
            return;
        }
        match self.tasks.wait4(self.shell_pid, Some(child), 0) {
            Ok(Some(result)) => self.report_wait(result),
            _ => self.write_line("sigdemo: child could not be reaped"),
        }
    }

    fn futex_demo(&mut self) {
        const FUTEX_ADDRESS: u64 = 0x7000_1000;

        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcFork,
            Some("futexdemo"),
        );
        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);
        self.log_ipc(child, "proc", RequestKind::ProcFutex, Some("FUTEX_WAIT"));
        if let Err(error) = self.tasks.futex_wait(child, FUTEX_ADDRESS, 1, 1) {
            self.write_line(error.message());
            return;
        }
        let changed_value_rejected = self
            .tasks
            .futex_wait(self.shell_pid, FUTEX_ADDRESS, 2, 1)
            .is_err();
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcFutex,
            Some("FUTEX_WAKE"),
        );
        let woken = match self.tasks.futex_wake(FUTEX_ADDRESS, 1) {
            Ok(woken) => woken,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        let mut line = InlineString::<128>::new();
        let _ = write!(
            &mut line,
            "futex: addr=0x{:x} wait=ok wake={} value-check={}",
            FUTEX_ADDRESS,
            woken,
            if changed_value_rejected {
                "EAGAIN"
            } else {
                "failed"
            }
        );
        self.write_line(line.as_str());
        if let Err(error) = self.tasks.exit(child, 0) {
            self.write_line(error.message());
            return;
        }
        match self.tasks.wait4(self.shell_pid, Some(child), 0) {
            Ok(Some(result)) => self.report_wait(result),
            _ => self.write_line("futexdemo: child could not be reaped"),
        }
    }

    fn wait_queue_demo(&mut self) {
        const QUEUE_KEY: u64 = 0x51;

        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);
        self.log_ipc(child, "proc", RequestKind::ProcWaitQueue, Some("wait"));
        if let Err(error) = self.tasks.wait_queue_wait(child, QUEUE_KEY) {
            self.write_line(error.message());
            return;
        }
        let woken = match self.tasks.wait_queue_wake(QUEUE_KEY, 1) {
            Ok(woken) => woken,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        let mut line = InlineString::<64>::new();
        let _ = write!(
            &mut line,
            "wait-queue: key=0x{:x} wake={}",
            QUEUE_KEY, woken
        );
        self.write_line(line.as_str());
        self.finish_demo_child(child, "waitqdemo");
    }

    fn timer_demo(&mut self) {
        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);
        let now = crate::arch::monotonic_time_ns();
        self.log_ipc(child, "proc", RequestKind::ProcTimer, Some("periodic"));
        let timer_id = match self.tasks.arm_timer(child, now, 1_000_000, 1_000_000) {
            Ok(timer_id) => timer_id,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        let result = self.tasks.poll_timers(now.saturating_add(3_500_000));
        let expirations = self
            .tasks
            .get_by_pid(child)
            .map(|task| task.timer_expirations)
            .unwrap_or(0);
        let mut line = InlineString::<96>::new();
        let _ = write!(
            &mut line,
            "timer: id={} expired={} rearmed={} notifications={}",
            timer_id, result.expirations, result.rearmed, expirations
        );
        self.write_line(line.as_str());
        if let Err(error) = self.tasks.cancel_timer(child, timer_id) {
            self.write_line(error.message());
            return;
        }
        self.finish_demo_child(child, "timerdemo");
    }

    fn sleep_command(&mut self, args: &[&str]) {
        if args.len() != 2 {
            self.write_line("usage: sleep <milliseconds>");
            return;
        }
        let milliseconds = match args[1].parse::<u64>() {
            Ok(value @ 1..=5_000) => value,
            _ => {
                self.write_line("sleep: milliseconds must be between 1 and 5000");
                return;
            }
        };
        let child = match self.tasks.fork(self.shell_pid) {
            Ok(pid) => pid,
            Err(error) => {
                self.write_line(error.message());
                return;
            }
        };
        self.set_state(child, TaskState::Running);
        let now = crate::arch::monotonic_time_ns();
        let duration_ns = milliseconds.saturating_mul(1_000_000);
        self.log_ipc(child, "proc", RequestKind::ProcTimer, Some("sleep"));
        if let Err(error) = self.tasks.sleep_for(child, now, duration_ns) {
            self.write_line(error.message());
            return;
        }
        let result = loop {
            let result = self.tasks.poll_timers(crate::arch::monotonic_time_ns());
            if !matches!(
                self.tasks.get_by_pid(child).map(|task| task.state),
                Some(TaskState::Sleeping(_, _))
            ) {
                break result;
            }
            crate::arch::wait_for_interrupt();
        };
        let mut line = InlineString::<64>::new();
        let _ = write!(
            &mut line,
            "sleep: {} ms expired={} wake={}",
            milliseconds, result.expirations, result.woken
        );
        self.write_line(line.as_str());
        self.finish_demo_child(child, "sleep");
    }

    fn finish_demo_child(&mut self, child: Pid, name: &str) {
        if let Err(error) = self.tasks.exit(child, 0) {
            self.write_line(error.message());
            return;
        }
        match self.tasks.wait4(self.shell_pid, Some(child), 0) {
            Ok(Some(result)) => self.report_wait(result),
            _ => {
                let mut line = InlineString::<64>::new();
                let _ = write!(&mut line, "{}: child could not be reaped", name);
                self.write_line(line.as_str());
            }
        }
    }

    fn poll_task_timers(&mut self) {
        let _ = self.tasks.poll_timers(crate::arch::monotonic_time_ns());
    }

    fn nice_command(&mut self, args: &[&str]) {
        if args.len() != 3 {
            self.write_line("usage: nice <pid> <-20..19>");
            return;
        }
        let (Ok(pid), Ok(nice)) = (args[1].parse::<Pid>(), args[2].parse::<i8>()) else {
            self.write_line("nice: invalid pid or value");
            return;
        };
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcSched,
            Some(args[1]),
        );
        match self.tasks.set_nice(pid, nice) {
            Ok(()) => self.write_line("nice: updated"),
            Err(error) => self.write_line(error.message()),
        }
    }

    fn chrt_command(&mut self, args: &[&str]) {
        if args.len() < 3 || args.len() > 4 {
            self.write_line("usage: chrt <pid> <other|fifo|rr> [priority]");
            return;
        }
        let Ok(pid) = args[1].parse::<Pid>() else {
            self.write_line("chrt: invalid pid");
            return;
        };
        let policy = match args[2] {
            "other" => SchedulingPolicy::Normal,
            "fifo" => SchedulingPolicy::Fifo,
            "rr" => SchedulingPolicy::RoundRobin,
            _ => {
                self.write_line("chrt: invalid policy");
                return;
            }
        };
        let priority = match args.get(3) {
            Some(value) => match value.parse::<u8>() {
                Ok(priority) => priority,
                Err(_) => {
                    self.write_line("chrt: invalid priority");
                    return;
                }
            },
            None if policy == SchedulingPolicy::Normal => 0,
            None => {
                self.write_line("chrt: real-time policy requires a priority");
                return;
            }
        };
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcSched,
            Some(args[1]),
        );
        match self.tasks.set_scheduler(pid, policy, priority) {
            Ok(()) => self.write_line("chrt: updated"),
            Err(error) => self.write_line(error.message()),
        }
    }

    fn taskset_command(&mut self, args: &[&str]) {
        if args.len() != 3 {
            self.write_line("usage: taskset <pid> <hex-mask>");
            return;
        }
        let Ok(pid) = args[1].parse::<Pid>() else {
            self.write_line("taskset: invalid pid");
            return;
        };
        let mask_text = args[2].strip_prefix("0x").unwrap_or(args[2]);
        let Ok(mask) = u64::from_str_radix(mask_text, 16) else {
            self.write_line("taskset: invalid hexadecimal mask");
            return;
        };
        self.log_ipc(
            self.shell_pid,
            "proc",
            RequestKind::ProcAffinity,
            Some(args[1]),
        );
        match self
            .tasks
            .set_affinity(pid, mask, crate::arch::online_cpu_count())
        {
            Ok(effective) => {
                let mut line = InlineString::<64>::new();
                let _ = write!(&mut line, "taskset: pid={} mask=0x{:x}", pid, effective);
                self.write_line(line.as_str());
                let _ = self.rebalance_tasks();
            }
            Err(error) => self.write_line(error.message()),
        }
    }

    fn rebalance_command(&mut self) {
        match self.rebalance_tasks() {
            Ok(migrations) => {
                let mut line = InlineString::<64>::new();
                let _ = write!(
                    &mut line,
                    "rebalance: cpus={} migrations={}",
                    crate::arch::online_cpu_count(),
                    migrations
                );
                self.write_line(line.as_str());
            }
            Err(error) => self.write_line(error),
        }
    }

    fn rebalance_tasks(&mut self) -> Result<usize, &'static str> {
        let migrations = self
            .tasks
            .rebalance(crate::arch::online_cpu_count())
            .map_err(|error| error.message())?;
        #[cfg(target_arch = "x86_64")]
        scheduler::record_migrations(migrations);
        Ok(migrations)
    }

    fn show_ipc_trace(&mut self) {
        if !crate::arch::supports_ipc_trace() {
            self.raw_write_line("ipc trace unavailable on this platform");
            return;
        }

        if self.ipc_count == 0 {
            self.raw_write_line("ipc trace is empty");
            return;
        }

        let start = if self.ipc_count < self.ipc_trace.len() {
            0
        } else {
            self.ipc_head
        };

        for offset in 0..self.ipc_count {
            let index = (start + offset) % self.ipc_trace.len();
            let line = self.ipc_trace[index];
            self.raw_write_line(line.as_str());
        }
    }

    fn print_banner(&mut self) {
        self.write_line(crate::arch::shell_banner());
    }

    fn show_ifconfig(&mut self) {
        let Some(status) = net::status() else {
            self.write_line("eth0: no carrier");
            return;
        };

        let mut ip = InlineString::<16>::new();
        let mut mask = InlineString::<16>::new();
        let mut gateway = InlineString::<16>::new();
        let mut mac = InlineString::<24>::new();
        net::format_ipv4(status.ip, &mut ip);
        net::format_ipv4(status.netmask, &mut mask);
        net::format_ipv4(status.gateway, &mut gateway);
        net::format_mac(status.mac, &mut mac);

        let mut line1 = InlineString::<128>::new();
        let _ = write!(
            &mut line1,
            "eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500  io=0x{:x}",
            status.io_base
        );
        self.write_line(line1.as_str());

        let mut line2 = InlineString::<128>::new();
        let _ = write!(
            &mut line2,
            "    inet {}  netmask {}  gateway {}",
            ip.as_str(),
            mask.as_str(),
            gateway.as_str()
        );
        self.write_line(line2.as_str());

        let mut line3 = InlineString::<64>::new();
        let _ = write!(&mut line3, "    ether {}", mac.as_str());
        self.write_line(line3.as_str());
    }

    fn show_route(&mut self) {
        let Some(status) = net::status() else {
            self.write_line("Kernel IP routing table is empty");
            return;
        };

        let mut gateway = InlineString::<16>::new();
        let mut network = InlineString::<16>::new();
        let mut source = InlineString::<16>::new();
        net::format_ipv4(status.gateway, &mut gateway);
        net::format_ipv4(
            [
                status.ip[0] & status.netmask[0],
                status.ip[1] & status.netmask[1],
                status.ip[2] & status.netmask[2],
                status.ip[3] & status.netmask[3],
            ],
            &mut network,
        );
        net::format_ipv4(status.ip, &mut source);

        let mut line1 = InlineString::<64>::new();
        let _ = write!(&mut line1, "default via {} dev eth0", gateway.as_str());
        self.write_line(line1.as_str());

        let mut line2 = InlineString::<64>::new();
        let _ = write!(
            &mut line2,
            "{}/24 dev eth0 src {}",
            network.as_str(),
            source.as_str()
        );
        self.write_line(line2.as_str());
    }

    fn ping_host(&mut self, text: &str) {
        let Some(target) = net::parse_ipv4(text) else {
            self.write_line("ping: invalid IPv4 address");
            return;
        };

        let mut target_text = InlineString::<16>::new();
        net::format_ipv4(target, &mut target_text);

        let mut header = InlineString::<64>::new();
        let _ = write!(
            &mut header,
            "PING {} ({}): 10 data bytes",
            target_text.as_str(),
            target_text.as_str()
        );
        self.write_line(header.as_str());

        match net::ping(target) {
            Ok(reply) => {
                let mut source = InlineString::<16>::new();
                net::format_ipv4(reply.from, &mut source);
                let mut line = InlineString::<96>::new();
                let _ = write!(
                    &mut line,
                    "{} bytes from {}: icmp_seq={} ttl={}",
                    reply.bytes,
                    source.as_str(),
                    reply.seq,
                    reply.ttl
                );
                self.write_line(line.as_str());
            }
            Err(err) => {
                let mut line = InlineString::<96>::new();
                let _ = write!(&mut line, "ping: {}", err);
                self.write_line(line.as_str());
            }
        }
    }

    pub fn write(&mut self, text: &str) {
        if !self.log_ipc(self.shell_pid, "console", RequestKind::ConsoleWrite, None) {
            return;
        }
        self.raw_write(text);
        crate::gui::sync_console();
    }

    pub fn write_line(&mut self, text: &str) {
        if !self.log_ipc(self.shell_pid, "console", RequestKind::ConsoleWrite, None) {
            return;
        }
        self.raw_write(text);
        self.raw_write("\n");
        crate::gui::sync_console();
    }

    fn prompt<'a>(&mut self, prompt: &str, buffer: &'a mut InlineString<128>) -> &'a str {
        self.prompt_for(self.shell_pid, prompt, buffer)
    }

    fn prompt_for<'a>(
        &mut self,
        caller: Pid,
        prompt: &str,
        buffer: &'a mut InlineString<128>,
    ) -> &'a str {
        if !self.log_ipc(caller, "console", RequestKind::ConsolePrompt, None) {
            buffer.clear();
            return buffer.as_str();
        }
        let console = &mut self.console;
        let tasks = &mut self.tasks;
        console.prompt(prompt, buffer, || {
            let _ = tasks.poll_timers(crate::arch::monotonic_time_ns());
        })
    }

    fn raw_write(&mut self, text: &str) {
        self.console.write(text);
    }

    fn raw_write_line(&mut self, text: &str) {
        self.console.write(text);
        self.console.write("\n");
        crate::gui::sync_console();
    }
}

impl<'a> SysApi<'a> {
    pub fn pid(&self) -> Pid {
        self.pid
    }

    pub fn ppid(&self) -> Pid {
        self.kernel
            .tasks
            .get_by_pid(self.pid)
            .and_then(|task| task.parent)
            .unwrap_or(0)
    }

    pub fn tgid(&self) -> Pid {
        self.kernel
            .tasks
            .get_by_pid(self.pid)
            .map(|task| task.tgid)
            .unwrap_or(0)
    }

    pub fn pgid(&self) -> Pid {
        self.kernel
            .tasks
            .get_by_pid(self.pid)
            .map(|task| task.pgid)
            .unwrap_or(0)
    }

    pub fn sid(&self) -> Pid {
        self.kernel
            .tasks
            .get_by_pid(self.pid)
            .map(|task| task.sid)
            .unwrap_or(0)
    }

    pub fn write(&mut self, text: &str) {
        if !self
            .kernel
            .log_ipc(self.pid, "console", RequestKind::ConsoleWrite, None)
        {
            return;
        }
        self.kernel.console.write(text);
        crate::gui::sync_console();
    }

    pub fn write_line(&mut self, text: &str) {
        if !self
            .kernel
            .log_ipc(self.pid, "console", RequestKind::ConsoleWrite, None)
        {
            return;
        }
        self.kernel.console.write(text);
        self.kernel.console.write("\n");
        crate::gui::sync_console();
    }

    pub fn write_line_num(&mut self, prefix: &str, value: i32) {
        let mut line = InlineString::<64>::new();
        let _ = write!(&mut line, "{}{}", prefix, value);
        self.write_line(line.as_str());
    }

    pub fn prompt<'b>(&mut self, prompt: &str, buffer: &'b mut InlineString<128>) -> &'b str {
        self.kernel.prompt_for(self.pid, prompt, buffer)
    }

    pub fn read_file(&mut self, path: &str) -> Result<&'static str, &'static str> {
        if !self
            .kernel
            .log_ipc(self.pid, "vfs", RequestKind::VfsReadFile, Some(path))
        {
            return Err("error: IPC access denied");
        }
        self.kernel
            .vfs
            .read_static_file(path)
            .ok_or("error: requested file is not available")
    }

    pub fn read_path<'b>(
        &mut self,
        path: &str,
        scratch: &'b mut InlineString<2048>,
    ) -> Result<&'b str, &'static str> {
        if !self
            .kernel
            .log_ipc(self.pid, "vfs", RequestKind::VfsReadFile, Some(path))
        {
            return Err("error: IPC access denied");
        }
        self.kernel
            .vfs
            .read_file(path, scratch)
            .ok_or("error: requested file is not available")
    }

    pub fn write_file(&mut self, path: &str, text: &str) -> crate::linux::Result<()> {
        if !self
            .kernel
            .log_ipc(self.pid, "vfs", RequestKind::VfsWriteFile, Some(path))
        {
            return Err(crate::linux::Errno::Access);
        }
        self.kernel.vfs.write_text(path, text)
    }

    pub fn clear_screen(&mut self) {
        crate::arch::clear_screen();
        crate::arch::gfx_console_reset();
        crate::gui::render_desktop();
    }

    pub fn sleep_ms(&mut self, milliseconds: u32) {
        if milliseconds == 0 {
            return;
        }
        let now = crate::arch::monotonic_time_ns();
        let duration_ns = u64::from(milliseconds).saturating_mul(1_000_000);
        if self
            .kernel
            .tasks
            .sleep_for(self.pid, now, duration_ns)
            .is_err()
        {
            return;
        }
        loop {
            let _ = self
                .kernel
                .tasks
                .poll_timers(crate::arch::monotonic_time_ns());
            if !matches!(
                self.kernel
                    .tasks
                    .get_by_pid(self.pid)
                    .map(|task| task.state),
                Some(TaskState::Sleeping(_, _))
            ) {
                break;
            }
            crate::arch::wait_for_interrupt();
        }
    }
}

fn task_kind_name(kind: TaskKind) -> &'static str {
    match kind {
        TaskKind::Kernel => "kernel",
        TaskKind::Service => "service",
        TaskKind::User => "user",
        TaskKind::Shell => "shell",
    }
}

fn service_accepts(service: &str, request: RequestKind) -> bool {
    match service {
        "kernel" => matches!(
            request,
            RequestKind::KernelListServices | RequestKind::KernelListTasks
        ),
        "console" => matches!(
            request,
            RequestKind::ConsoleWrite | RequestKind::ConsolePrompt
        ),
        "vfs" => matches!(
            request,
            RequestKind::VfsReadFile | RequestKind::VfsWriteFile | RequestKind::VfsListDir
        ),
        "proc" => matches!(
            request,
            RequestKind::ProcExec
                | RequestKind::ProcFork
                | RequestKind::ProcClone
                | RequestKind::ProcExit
                | RequestKind::ProcWait4
                | RequestKind::ProcSignal
                | RequestKind::ProcFutex
                | RequestKind::ProcSched
                | RequestKind::ProcAffinity
                | RequestKind::ProcWaitQueue
                | RequestKind::ProcTimer
        ),
        "net" => matches!(request, RequestKind::NetStatus | RequestKind::NetPing),
        _ => false,
    }
}

fn render_state(state: TaskState, out: &mut InlineString<24>) {
    match state {
        TaskState::Ready => out.set("ready"),
        TaskState::Running => out.set("running"),
        TaskState::Waiting => out.set("waiting"),
        TaskState::FutexWait(address) => {
            let _ = write!(out, "futex(0x{:x})", address);
        }
        TaskState::WaitQueue(key) => {
            let _ = write!(out, "waitq(0x{:x})", key);
        }
        TaskState::ChildWait => out.set("wait-child"),
        TaskState::IpcWait => out.set("wait-ipc"),
        TaskState::Sleeping(_, deadline) => {
            let _ = write!(out, "sleep({})", deadline);
        }
        TaskState::Zombie(code) => {
            let _ = write!(out, "zombie({})", code);
        }
    }
}

fn parse_words<'a>(line: &'a str, out: &mut [&'a str; MAX_ARGS]) -> usize {
    let mut count = 0;
    for part in line.split_whitespace() {
        if count >= out.len() {
            break;
        }
        out[count] = part;
        count += 1;
    }
    count
}
